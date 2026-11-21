#define DUCKDB_EXTENSION_MAIN

#include "hdfs_extension.hpp"

#include "duckdb.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/secret/secret.hpp"
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

static void RegisterHdfsSecret(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = "hdfs";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_function;
	config_function.secret_type = "hdfs";
	config_function.provider = "config";
	config_function.function = CreateHdfsSecretFromConfig;
	config_function.named_parameters["config_dir"] = LogicalType::VARCHAR;
	config_function.named_parameters["user"] = LogicalType::VARCHAR;
	loader.RegisterFunction(config_function);
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
