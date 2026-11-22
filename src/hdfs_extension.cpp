#define DUCKDB_EXTENSION_MAIN

#include "hdfs_extension.hpp"

#include "duckdb.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/string_util.hpp"
#include "hdfs_filesystem.hpp"
#include "hdfs_functions.hpp"
#include "hdfs_logging.hpp"

namespace duckdb {

//! CREATE SECRET (TYPE hdfs, ...): where to find the Hadoop configuration, and who to act as, for the URLs in its
//! scope (default: all of hdfs://).
static unique_ptr<BaseSecret> CreateHdfsSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope.empty() ? vector<string> {"hdfs://"} : input.scope;
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->TrySetValue("config_dir", input);
	secret->TrySetValue("user", input);
	return std::move(secret);
}

//! Hadoop property names whose values are credentials (or point at them): a snapshot never stores them, since the
//! secret file is not encrypted and redaction only hides values from duckdb_secrets().
static bool IsSensitiveHadoopProperty(const string &key) {
	auto lower = StringUtil::Lower(key);
	return lower.find("password") != string::npos || lower.find("passwd") != string::npos ||
	       lower.find("secret") != string::npos || lower.find("keytab") != string::npos ||
	       StringUtil::StartsWith(lower, "hadoop.security.credential.");
}

//! Limits on a snapshot's size, so a stored secret stays small and a corrupt one can't feed libhdfs3 anything huge.
static constexpr idx_t HDFS_SNAPSHOT_MAX_PROPERTIES = 4096;
static constexpr idx_t HDFS_SNAPSHOT_MAX_BYTES = 1 << 20;

//! CREATE SECRET (TYPE hdfs, PROVIDER config_snapshot, PROPERTIES MAP {...}, ...): the Hadoop configuration itself,
//! so a persistent secret keeps working without any local file. Nothing is read from the environment at connect,
//! except HADOOP_USER_NAME without a USER.
static unique_ptr<BaseSecret> CreateHdfsSecretFromSnapshot(ClientContext &context, CreateSecretInput &input) {
	auto name = input.name.GetIdentifierName();
	auto what = "HDFS secret '" + name + "'";
	auto lookup = input.options.find("properties");
	if (lookup == input.options.end()) {
		throw InvalidInputException("%s: PROVIDER config_snapshot needs PROPERTIES (a MAP of Hadoop properties)", what);
	}
	// Properties name the NameNodes, the KMS and local paths (the ticket cache): with external access disabled,
	// SQL must not be able to point an allowed hdfs:// URL somewhere else. Create the secret beforehand.
	if (!Settings::Get<EnableExternalAccessSetting>(DBConfig::GetConfig(context))) {
		throw PermissionException("%s: PROPERTIES can't be set while enable_external_access = false. Create the "
		                          "secret before disabling external access",
		                          what);
	}
	auto properties = HdfsPropertiesFromValue(lookup->second, what);
	if (properties.empty()) {
		throw InvalidInputException("%s: PROPERTIES is empty", what);
	}
	if (properties.size() > HDFS_SNAPSHOT_MAX_PROPERTIES) {
		throw InvalidInputException("%s: PROPERTIES has %llu entries, more than the %llu allowed", what,
		                            properties.size(), HDFS_SNAPSHOT_MAX_PROPERTIES);
	}
	idx_t bytes = 0;
	for (auto &property : properties) {
		if (IsSensitiveHadoopProperty(property.first)) {
			throw InvalidInputException("%s: property '%s' looks like a credential, and secrets are stored "
			                            "unencrypted: leave it out (it is not used to connect to HDFS)",
			                            what, property.first);
		}
		bytes += property.first.size() + property.second.size();
	}
	if (bytes > HDFS_SNAPSHOT_MAX_BYTES) {
		throw InvalidInputException("%s: PROPERTIES holds %llu bytes, more than the %llu allowed", what, bytes,
		                            HDFS_SNAPSHOT_MAX_BYTES);
	}
	string user;
	auto user_lookup = input.options.find("user");
	if (user_lookup != input.options.end() && !user_lookup->second.IsNull()) {
		user = user_lookup->second.ToString();
	}

	auto scope = input.scope.empty() ? vector<string> {"hdfs://"} : input.scope;
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->secret_map["format_version"] = Value::INTEGER(HDFS_SNAPSHOT_FORMAT_VERSION);
	secret->secret_map["fingerprint"] = Value(HdfsConnectionConfig::Fingerprint(properties, user));
	secret->secret_map["properties"] = HdfsPropertiesToValue(properties);
	if (!user.empty()) {
		secret->secret_map["user"] = Value(user);
	}
	return std::move(secret);
}

static void RegisterHdfsSecret(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = "hdfs";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = HDFS_SECRET_PROVIDER_CONFIG;
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_function;
	config_function.secret_type = "hdfs";
	config_function.provider = HDFS_SECRET_PROVIDER_CONFIG;
	config_function.function = CreateHdfsSecretFromConfig;
	config_function.named_parameters["config_dir"] = LogicalType::VARCHAR;
	config_function.named_parameters["user"] = LogicalType::VARCHAR;
	loader.RegisterFunction(config_function);

	CreateSecretFunction snapshot_function;
	snapshot_function.secret_type = "hdfs";
	snapshot_function.provider = HDFS_SECRET_PROVIDER_SNAPSHOT;
	snapshot_function.function = CreateHdfsSecretFromSnapshot;
	snapshot_function.named_parameters["properties"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	snapshot_function.named_parameters["user"] = LogicalType::VARCHAR;
	loader.RegisterFunction(snapshot_function);
}

//! hdfs_list_parallelism is the number of threads each walk starts: 1 to HdfsWalkOptions::MAX_PARALLELISM.
static void SetHdfsListParallelism(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		return;
	}
	auto value = UBigIntValue::Get(parameter);
	if (value < 1 || value > HdfsWalkOptions::MAX_PARALLELISM) {
		throw InvalidInputException("hdfs_list_parallelism must be between 1 and %llu, got %llu",
		                            HdfsWalkOptions::MAX_PARALLELISM, value);
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	// Connections are configured from an hdfs secret matching the URL, else from the Hadoop environment
	// (HADOOP_CONF_DIR, HADOOP_USER_NAME, Kerberos ticket cache), as for the Hadoop CLI.
	loader.SetDescription("Reads and writes hdfs:// URLs with a native HDFS client, and lists HDFS metadata");
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	// One entry per NameNode RPC: CALL enable_logging('HDFS'), then duckdb_logs_parsed('HDFS').
	db.GetLogManager().RegisterLogType(make_uniq<HdfsLogType>());
	config.AddExtensionOption("hdfs_list_parallelism",
	                          "Maximum number of concurrent HDFS directory-listing RPCs (threads) used by each listing "
	                          "or glob that walks more than one directory (e.g. hdfs_ls('hdfs:///path/**')), from 1 "
	                          "(no parallelism) to 64",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_HDFS_LIST_PARALLELISM),
	                          SetHdfsListParallelism);
	config.AddExtensionOption("hdfs_skip_permission_errors",
	                          "Prune subtrees whose listing fails with a permission error during HDFS globs and "
	                          "recursive listings instead of failing the query; an error on the listed path or "
	                          "glob root itself still fails",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("hdfs_include_hidden",
	                          "Return hidden HDFS entries (names starting with '_' or '.', e.g. _temporary, "
	                          "_SUCCESS) from listings, globs and scans; by default they are excluded unless a "
	                          "glob component names them explicitly (e.g. '_*')",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	config.AddExtensionOption("hdfs_batched_listing",
	                          "List several HDFS directories per NameNode call (getBatchedListing, Hadoop 3.3+) in "
	                          "recursive listings and globs; NameNodes that can't are detected and listed one "
	                          "directory per call",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));

	auto hdfs_fs = make_uniq<HdfsFileSystem>();
	// The VirtualFileSystem owns the filesystem for the database's lifetime; the SQL functions refer to it.
	auto &hdfs = *hdfs_fs;
	db.GetFileSystem().RegisterSubSystem(std::move(hdfs_fs));
	RegisterHdfsFunctions(loader, hdfs);
	RegisterHdfsConnectionFunctions(loader, hdfs);
	RegisterHdfsSecret(loader);
}

void HdfsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string HdfsExtension::Name() {
	return "hdfs";
}

std::string HdfsExtension::Version() const {
#ifdef EXT_VERSION_HDFS
	return EXT_VERSION_HDFS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(hdfs, loader) {
	duckdb::LoadInternal(loader);
}
}
