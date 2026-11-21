#include "hdfs_filesystem.hpp"
#include "hdfs_functions.hpp"
#include "hdfs_glob.hpp"
#include "hdfs_logging.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <algorithm>

namespace duckdb {

namespace {

//! Split a URL into its authority ("hdfs://host:port", or "" for the default FS) and the scheme-less path ("/a/b").
//! Inputs without the hdfs:// scheme are a path on the default FS.
void ParseHdfsUrl(const string &path, string &authority, string &hdfs_path) {
	const string scheme = "hdfs://";
	if (path.rfind(scheme, 0) != 0) {
		authority = "";
		hdfs_path = path;
		return;
	}
	auto authority_start = scheme.size();
	auto slash_pos = path.find('/', authority_start);
	if (slash_pos == string::npos) {
		authority = path;
		hdfs_path = "/";
	} else if (slash_pos == authority_start) {
		// "hdfs:///a/b": the default FS
		authority = "";
		hdfs_path = path.substr(slash_pos);
	} else {
		authority = path.substr(0, slash_pos);
		hdfs_path = path.substr(slash_pos);
	}
}

//! Refuse a path with a "." or ".." component. HDFS forbids them in names, and DuckDB doesn't canonicalize remote
//! paths before its allowed_directories check, so "hdfs://nn/allowed/../x" would otherwise pass that check.
void CheckNoDotComponents(const string &url, const string &hdfs_path) {
	idx_t start = 0;
	while (start <= hdfs_path.size()) {
		auto slash = hdfs_path.find('/', start);
		if (slash == string::npos) {
			slash = hdfs_path.size();
		}
		auto length = slash - start;
		if ((length == 1 || length == 2) && hdfs_path.compare(start, length, string(length, '.')) == 0) {
			throw InvalidInputException("HDFS path '%s' has a '%s' component; HDFS paths cannot contain '.' or '..'",
			                            url, hdfs_path.substr(start, length));
		}
		start = slash + 1;
	}
}

//! ParseHdfsUrl, refusing "." and ".." components.
void ParseHdfsPath(const string &path, string &authority, string &hdfs_path) {
	ParseHdfsUrl(path, authority, hdfs_path);
	CheckNoDotComponents(path, hdfs_path);
}

//! Reattach an authority to a scheme-less path.
string MakeUrl(const string &authority, const string &hdfs_path) {
	auto prefix = authority.empty() ? string("hdfs://") : authority;
	if (hdfs_path.empty() || hdfs_path[0] != '/') {
		return prefix + "/" + hdfs_path;
	}
	return prefix + hdfs_path;
}

HdfsEntry MakeHdfsEntry(const string &authority, const HdfsFileStatus &status) {
	HdfsEntry entry;
	entry.url = MakeUrl(authority, status.path);
	auto slash = status.path.find_last_of('/');
	entry.name = slash == string::npos ? status.path : status.path.substr(slash + 1);
	entry.is_dir = status.is_dir;
	entry.size = status.length;
	entry.owner = status.owner;
	entry.group = status.group;
	entry.permission = status.permission;
	entry.replication = status.replication;
	entry.block_size = status.block_size;
	entry.file_id = status.file_id;
	entry.mtime = HdfsTimestampFromEpochMs(static_cast<int64_t>(status.mtime), entry.url);
	entry.atime = HdfsTimestampFromEpochMs(static_cast<int64_t>(status.atime), entry.url);
	return entry;
}

//! DuckDB's extended file info from a listing, so opening the file costs no extra stat.
shared_ptr<ExtendedOpenFileInfo> MakeExtendedInfo(const HdfsEntry &entry) {
	auto info = make_shared_ptr<ExtendedOpenFileInfo>();
	info->options.emplace("type", Value(entry.is_dir ? "directory" : "file"));
	info->options.emplace("file_size", Value::BIGINT(entry.size));
	info->options.emplace("last_modified", Value::TIMESTAMP(entry.mtime));
	if (entry.file_id > 0) {
		// Lets OpenFileExtended share an open reader only with the same file, not one that replaced it.
		// UBIGINT: core exports ExtendedOpenFileInfo::TryGetOption for idx_t, not for int64_t.
		info->options.emplace("hdfs_file_id", Value::UBIGINT(static_cast<uint64_t>(entry.file_id)));
	}
	return info;
}

//! Throw `error` as the DuckDB exception for its type, with `context` (naming the URL) in front of its message.
[[noreturn]] void ThrowHdfsError(const string &context, const HdfsError &error) {
	auto message = context + ": " + error.message;
	switch (error.type) {
	case HdfsErrorType::INVALID_INPUT:
		throw InvalidInputException(message);
	case HdfsErrorType::INVALID_CONFIGURATION:
		throw InvalidConfigurationException(message);
	default:
		throw IOException(message);
	}
}

//! A walk setting from the client's settings, or `fallback` if it isn't set.
bool WalkSetting(optional_ptr<FileOpener> opener, const char *name, bool fallback) {
	Value value;
	if (FileOpener::TryGetCurrentSetting(opener, name, value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	return fallback;
}

} // namespace

//! A reconnectable client for one authority, established on first use. Operations hold the client they run on, so
//! another thread dropping it after a failure (e.g. a NameNode failover) doesn't pull it out from under them.
class HdfsConnection {
public:
	HdfsConnection(string authority_p, HdfsConnectionConfig config_p)
	    : authority(std::move(authority_p)), config(std::move(config_p)) {
	}

	shared_ptr<HdfsClient> Get() {
		lock_guard<mutex> guard(lock);
		if (!client) {
			try {
				client = HdfsClient::Connect(authority, config);
			} catch (const HdfsError &error) {
				ThrowHdfsError(
				    StringUtil::Format("Failed to connect to HDFS (%s)", authority.empty() ? "default fs" : authority),
				    error);
			}
		}
		return client;
	}

	//! Drop `stale` if it is still the current client, so the next Get() reconnects.
	void Invalidate(const shared_ptr<HdfsClient> &stale) {
		lock_guard<mutex> guard(lock);
		if (client == stale) {
			client.reset();
		}
	}

	//! Run a read-only (idempotent) `op(client)`. A connection failure (stale client after a failover, a dropped
	//! socket, an expired ticket) is retried once on a new client; other HdfsErrors propagate.
	template <class FN>
	auto Run(FN &&op) -> decltype(op(std::declval<const shared_ptr<HdfsClient> &>())) {
		for (idx_t attempt = 0;; attempt++) {
			auto current = Get();
			try {
				return op(current);
			} catch (const HdfsError &error) {
				if (attempt > 0 || error.type != HdfsErrorType::CONNECTION) {
					throw;
				}
				Invalidate(current);
			}
		}
	}

	//! Run a mutation `op(client)` (create, delete, rename). It is not simply retried after a connection failure:
	//! the NameNode may have applied the call before the connection failed, and a new client doesn't share the
	//! old one's NameNode RetryCache entry. Instead `recover(client, error)` runs on a new client, to find out what
	//! happened and finish the job (or throw).
	template <class FN, class RECOVER>
	auto RunMutation(FN &&op, RECOVER &&recover) -> decltype(op(std::declval<const shared_ptr<HdfsClient> &>())) {
		auto current = Get();
		unique_ptr<HdfsError> failure;
		try {
			return op(current);
		} catch (const HdfsError &error) {
			if (error.type != HdfsErrorType::CONNECTION) {
				throw;
			}
			failure = make_uniq<HdfsError>(error);
		}
		Invalidate(current);
		return recover(Get(), *failure);
	}

	const string authority;
	const HdfsConnectionConfig config;

private:
	mutex lock;
	shared_ptr<HdfsClient> client;
};

HdfsFileHandle::~HdfsFileHandle() {
	HdfsRpcLoggerScope log_scope(rpc_logger.lock());
	// Some core writers drop a handle without calling Close() (EXPORT DATABASE's schema.sql / load.sql), as local
	// files allow, so a writer destroyed normally still commits. One destroyed while an exception unwinds, or after a
	// failed write, belongs to a failed write: it is aborted, and the file it created is removed. Neither path lets an
	// exception escape.
	if (writer && !failed && !Exception::UncaughtException()) {
		try {
			Close();
			return;
		} catch (std::exception &error) {
			// Close() already aborted (removed) the file. The caller dropped the handle without Close(), so the error
			// has nowhere to go but the log.
			try {
				if (logger) {
					auto message =
					    StringUtil::Format("Failed to complete HDFS file '%s' (handle dropped without Close; "
					                       "the file was removed): %s",
					                       path, ErrorData(error).RawMessage());
					DUCKDB_LOG_ERROR(logger, message);
				}
			} catch (...) { // NOLINT: destructors must not throw
			}
			return;
		} catch (...) { // NOLINT: destructors must not throw
			return;
		}
	}
	try {
		Abort();
	} catch (...) { // NOLINT: destructors must not throw
	}
}

void HdfsFileHandle::Close() {
	HdfsRpcLoggerScope log_scope(rpc_logger.lock());
	if (reader || writer) {
		DUCKDB_LOG_FILE_SYSTEM_CLOSE((*this));
	}
	reader.reset();
	if (!writer) {
		return;
	}
	if (failed) {
		Abort();
		throw IOException("Failed to close HDFS file '%s': a previous write failed; the incomplete file was removed",
		                  path);
	}
	try {
		writer->Close();
	} catch (const HdfsError &error) {
		// Completing the file failed: it may be incomplete, so it is removed rather than left behind.
		failed = true;
		string cleanup;
		try {
			Abort();
		} catch (const std::exception &abort_error) {
			cleanup = "; " + ErrorData(abort_error).RawMessage();
		}
		ThrowHdfsError(StringUtil::Format("Failed to close HDFS file '%s'", path),
		               HdfsError(error.type, error.message + cleanup));
	}
	writer.reset();
}

void HdfsFileHandle::Abort() {
	HdfsRpcLoggerScope log_scope(rpc_logger.lock());
	reader.reset();
	if (!writer) {
		return;
	}
	// Released first, so the write is abandoned once even if removing the file fails.
	auto aborting = std::move(writer);
	try {
		aborting->Abort();
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to abort the write of HDFS file '%s'", path), error);
	}
}

HdfsFileSystem::HdfsFileSystem() = default;

HdfsFileSystem::~HdfsFileSystem() = default;

namespace {

//! Whether a secret scope `prefix` of `url` ends at a path-component boundary: scope ".../permtest" covers
//! ".../permtest" and ".../permtest/a", not ".../permtest2". An empty scope covers everything.
bool ScopeCovers(const string &prefix, const string &url) {
	if (!StringUtil::StartsWith(url, prefix)) {
		return false;
	}
	return prefix.empty() || prefix.size() == url.size() || prefix.back() == '/' || url[prefix.size()] == '/';
}

//! The hdfs secret whose scope covers `url` (by whole path components), for one connection.
struct HdfsSecretMatch {
	HdfsConnectionConfig config;
	//! Which secret ("<storage>.<name>"), empty without one.
	string secret;
};

HdfsSecretMatch LookupSecret(const string &url, optional_ptr<FileOpener> opener) {
	HdfsSecretMatch result;
	if (!opener) {
		return result;
	}
	auto db = opener->TryGetDatabase();
	if (!db) {
		return result;
	}
	auto context = opener->TryGetClientContext();
	auto &secret_manager = db->GetSecretManager();
	auto transaction = context ? CatalogTransaction::GetSystemCatalogTransaction(*context)
	                           : CatalogTransaction::GetSystemTransaction(*db);
	// DuckDB matches scopes as plain string prefixes, so ".../permtest" also matches ".../permtest2". When the best
	// match ends inside a path component, look again for a scope that ends at an earlier component boundary.
	auto lookup = url;
	for (;;) {
		auto match = secret_manager.LookupSecret(transaction, lookup, "hdfs");
		if (!match.HasMatch()) {
			return result;
		}
		auto &secret = match.GetSecret();
		// The scope DuckDB matched: the longest one that is a prefix of `lookup`.
		auto &scopes = secret.GetScope();
		string longest;
		for (auto &scope : scopes) {
			if (StringUtil::StartsWith(lookup, scope) && scope.size() >= longest.size()) {
				longest = scope;
			}
		}
		if (scopes.empty() || ScopeCovers(longest, url)) {
			auto &kv_secret = secret.Cast<KeyValueSecret>();
			Value value;
			if (kv_secret.TryGetValue("config_dir", value) && !value.IsNull()) {
				result.config.config_dir = value.ToString();
			}
			if (kv_secret.TryGetValue("user", value) && !value.IsNull()) {
				result.config.user = value.ToString();
			}
			result.secret = match.secret_entry->storage_mode + "." + secret.GetName();
			return result;
		}
		// `longest` ends inside a component: retry with the URL cut at the last '/' before that point.
		auto slash = longest.empty() ? string::npos : url.rfind('/', longest.size() - 1);
		if (slash == string::npos) {
			return result;
		}
		lookup = url.substr(0, slash + 1);
	}
}

//! The connection cache key of an authority: a host given with a port is a DNS name, so its case doesn't matter.
//! (A name without a port may be an HA nameservice, which Hadoop matches case-sensitively.)
string NormalizeAuthority(const string &authority) {
	auto host_start = authority.find("://");
	host_start = host_start == string::npos ? 0 : host_start + 3;
	auto at = authority.find('@', host_start);
	if (at != string::npos) {
		host_start = at + 1;
	}
	if (authority.find(':', host_start) == string::npos) {
		return authority;
	}
	return authority.substr(0, host_start) + StringUtil::Lower(authority.substr(host_start));
}

bool SameConfig(const HdfsConnectionConfig &a, const HdfsConnectionConfig &b) {
	return a.config_dir == b.config_dir && a.user == b.user;
}

} // namespace

shared_ptr<HdfsConnection> HdfsFileSystem::GetConnection(const string &url, const string &authority,
                                                         optional_ptr<FileOpener> opener) {
	// Every operation gets its connection here first: log its NameNode RPCs to the caller's query (HDFS log type).
	HdfsClient::SetThreadLogger(HdfsLoggerFor(opener));
	auto match = LookupSecret(url, opener);
	// One connection per authority and secret: when the secret is replaced with a new configuration, so is its
	// connection. Open files and listings keep the client they hold.
	auto key = NormalizeAuthority(authority) + '\0' + match.secret;
	lock_guard<mutex> guard(connections_lock);
	auto &connection = connections[key];
	if (!connection || !SameConfig(connection->config, match.config)) {
		connection = make_shared_ptr<HdfsConnection>(authority, std::move(match.config));
	}
	return connection;
}

idx_t HdfsFileSystem::ResetConnections() {
	lock_guard<mutex> guard(connections_lock);
	auto count = connections.size();
	connections.clear();
	return count;
}

unique_ptr<FileHandle> HdfsFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                        optional_ptr<FileOpener> opener) {
	if (!file.extended_info || flags.OpenForWriting()) {
		return OpenFileInternal(file.path, flags, opener, nullptr);
	}
	// The listing's status names the file version to open; without both size and mtime, the file is stat'ed.
	idx_t file_size;
	timestamp_t last_modified;
	string type;
	if (!file.extended_info->TryGetOption("file_size", file_size) ||
	    !file.extended_info->TryGetOption("last_modified", last_modified) ||
	    (file.extended_info->TryGetOption("type", type) && type != "file")) {
		return OpenFileInternal(file.path, flags, opener, nullptr);
	}
	HdfsFileStatus expected;
	expected.length = NumericCast<int64_t>(file_size);
	expected.mtime = NumericCast<uint64_t>(MaxValue<int64_t>(Timestamp::GetEpochMs(last_modified), 0));
	idx_t file_id;
	if (file.extended_info->TryGetOption("hdfs_file_id", file_id)) {
		expected.file_id = NumericCast<int64_t>(file_id);
	}
	return OpenFileInternal(file.path, flags, opener, &expected);
}

unique_ptr<FileHandle> HdfsFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                optional_ptr<FileOpener> opener) {
	return OpenFileInternal(path, flags, opener, nullptr);
}

namespace {

//! The writer for `hdfs_path`, created by a mutation that isn't retried blindly: after a connection failure the
//! create is tried once more on a new client, and an "already exists" then may be the first attempt's own file.
unique_ptr<HdfsWriter> CreateWriter(HdfsConnection &connection, const string &hdfs_path, bool overwrite,
                                    shared_ptr<HdfsClient> &used_client) {
	return connection.RunMutation(
	    [&](const shared_ptr<HdfsClient> &client) {
		    used_client = client;
		    return client->Create(hdfs_path, overwrite);
	    },
	    [&](const shared_ptr<HdfsClient> &client, const HdfsError &failure) {
		    used_client = client;
		    try {
			    return client->Create(hdfs_path, overwrite);
		    } catch (const HdfsError &error) {
			    if (error.type != HdfsErrorType::ALREADY_EXISTS) {
				    throw;
			    }
			    throw HdfsError(HdfsErrorType::IO,
			                    failure.message +
			                        "; after reconnecting, the file exists (or is being written), possibly "
			                        "created by the failed attempt: the outcome is unknown");
		    }
	    });
}

} // namespace

unique_ptr<FileHandle> HdfsFileSystem::OpenFileInternal(const string &path, FileOpenFlags flags,
                                                        optional_ptr<FileOpener> opener,
                                                        optional_ptr<const HdfsFileStatus> expected) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(path, authority, hdfs_path);
	auto connection = GetConnection(path, authority, opener);
	unique_ptr<HdfsFileHandle> handle;
	if (flags.OpenForWriting()) {
		if (flags.OpenForReading()) {
			throw NotImplementedException("Cannot open HDFS file '%s' for reading and writing at the same time", path);
		}
		if (flags.OpenForAppending()) {
			throw NotImplementedException("Cannot open HDFS file '%s' for appending: appending is not supported", path);
		}
		// Exclusive create (and "null if it exists") must not replace an existing file: create without overwrite,
		// which the NameNode checks atomically.
		auto overwrite = flags.OverwriteExistingFile() && !flags.ExclusiveCreate() && !flags.ReturnNullIfExists();
		try {
			shared_ptr<HdfsClient> client;
			auto writer = CreateWriter(*connection, hdfs_path, overwrite, client);
			handle = make_uniq<HdfsFileHandle>(*this, path, flags, client, nullptr, std::move(writer));
		} catch (const HdfsError &error) {
			if (error.type == HdfsErrorType::ALREADY_EXISTS && flags.ReturnNullIfExists()) {
				return nullptr;
			}
			ThrowHdfsError(StringUtil::Format("Failed to open HDFS file for writing '%s'", path), error);
		}
	} else {
		try {
			connection->Run([&](const shared_ptr<HdfsClient> &client) {
				auto reader = client->Open(hdfs_path, expected);
				handle = make_uniq<HdfsFileHandle>(*this, path, flags, client, std::move(reader), nullptr);
			});
		} catch (const HdfsError &error) {
			// Only a genuine "not found" is a missing file; connection or permission failures are errors.
			if (error.type == HdfsErrorType::NOT_FOUND && flags.ReturnNullIfNotExists()) {
				return nullptr;
			}
			ThrowHdfsError(StringUtil::Format("Failed to open HDFS file for reading '%s'", path), error);
		}
		handle->length = NumericCast<idx_t>(MaxValue<int64_t>(handle->reader->Length(), 0));
		// The version the reader reads (a shared reader may be older than a newer listing; never mix the two).
		handle->mtime = HdfsTimestampFromEpochMs(static_cast<int64_t>(handle->reader->ModificationTime()), path);
		handle->has_mtime = true;
	}
	handle->rpc_logger = HdfsLoggerFor(opener);
	if (opener) {
		handle->TryAddLogger(*opener);
	}
	DUCKDB_LOG_FILE_SYSTEM_OPEN((*handle));
	return std::move(handle);
}

void HdfsFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (!hdfs_handle.reader) {
		throw IOException("File not opened for reading: " + handle.path);
	}
	if (nr_bytes == 0) {
		return;
	}
	// Written to avoid unsigned overflow on `location + nr_bytes`.
	if (nr_bytes < 0 || location > hdfs_handle.length || NumericCast<idx_t>(nr_bytes) > hdfs_handle.length - location) {
		throw IOException("Cannot read %lld bytes at offset %llu from HDFS file '%s' of size %llu", nr_bytes, location,
		                  handle.path, hdfs_handle.length);
	}
	try {
		hdfs_handle.reader->Read(static_cast<data_ptr_t>(buffer), NumericCast<idx_t>(nr_bytes), location);
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to read from HDFS file '%s'", handle.path), error);
	}
	DUCKDB_LOG_FILE_SYSTEM_READ(handle, nr_bytes, location);
}

int64_t HdfsFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (nr_bytes <= 0 || hdfs_handle.position >= hdfs_handle.length) {
		return 0;
	}
	auto to_read = MinValue<idx_t>(NumericCast<idx_t>(nr_bytes), hdfs_handle.length - hdfs_handle.position);
	Read(handle, buffer, NumericCast<int64_t>(to_read), hdfs_handle.position);
	hdfs_handle.position += to_read;
	return NumericCast<int64_t>(to_read);
}

void HdfsFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (hdfs_handle.writer && location != hdfs_handle.length) {
		throw NotImplementedException("Cannot write at offset %llu of HDFS file '%s': HDFS files are written "
		                              "sequentially, so the next write must be at offset %llu",
		                              location, handle.path, hdfs_handle.length);
	}
	// A positional write doesn't move the cursor.
	auto position = hdfs_handle.position;
	hdfs_handle.position = location;
	try {
		Write(handle, buffer, nr_bytes);
	} catch (...) {
		hdfs_handle.position = position;
		throw;
	}
	hdfs_handle.position = position;
}

int64_t HdfsFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (!hdfs_handle.writer) {
		throw IOException("File not opened for writing: " + handle.path);
	}
	if (hdfs_handle.failed) {
		throw IOException("Cannot write to HDFS file '%s': a previous write failed, so the file is incomplete",
		                  handle.path);
	}
	if (nr_bytes < 0) {
		throw InvalidInputException("Cannot write a negative number of bytes to HDFS file '%s'", handle.path);
	}
	if (hdfs_handle.position != hdfs_handle.length) {
		// After a Seek: HDFS can only append.
		throw NotImplementedException("Cannot write at offset %llu of HDFS file '%s': HDFS files are written "
		                              "sequentially, so the next write must be at offset %llu",
		                              hdfs_handle.position, handle.path, hdfs_handle.length);
	}
	try {
		hdfs_handle.writer->Write(static_cast<const_data_ptr_t>(buffer), NumericCast<idx_t>(nr_bytes));
	} catch (const HdfsError &error) {
		// Part of the data may have been sent: the file can't be completed correctly any more.
		hdfs_handle.failed = true;
		ThrowHdfsError(StringUtil::Format("Failed to write to HDFS file '%s'", handle.path), error);
	}
	DUCKDB_LOG_FILE_SYSTEM_WRITE(handle, nr_bytes, hdfs_handle.position);
	hdfs_handle.position += NumericCast<idx_t>(nr_bytes);
	hdfs_handle.length += NumericCast<idx_t>(nr_bytes);
	return nr_bytes;
}

void HdfsFileSystem::AbortFileWrite(FileHandle &handle) {
	handle.Cast<HdfsFileHandle>().Abort();
}

void HdfsFileSystem::FileSync(FileHandle &handle) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (!hdfs_handle.writer) {
		return; // nothing to sync for a reader
	}
	if (hdfs_handle.failed) {
		throw IOException("Cannot sync HDFS file '%s': a previous write failed, so the file is incomplete",
		                  handle.path);
	}
	try {
		hdfs_handle.writer->Sync();
	} catch (const HdfsError &error) {
		hdfs_handle.failed = true;
		ThrowHdfsError(StringUtil::Format("Failed to sync HDFS file '%s'", handle.path), error);
	}
}

void HdfsFileSystem::Seek(FileHandle &handle, idx_t location) {
	handle.Cast<HdfsFileHandle>().position = location;
}

idx_t HdfsFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<HdfsFileHandle>().position;
}

void HdfsFileSystem::Reset(FileHandle &handle) {
	handle.Cast<HdfsFileHandle>().position = 0;
}

int64_t HdfsFileSystem::GetFileSize(FileHandle &handle) {
	return NumericCast<int64_t>(handle.Cast<HdfsFileHandle>().length);
}

timestamp_t HdfsFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (hdfs_handle.has_mtime) {
		return hdfs_handle.mtime;
	}
	string authority;
	string hdfs_path;
	ParseHdfsPath(handle.path, authority, hdfs_path);
	HdfsFileStatus status;
	try {
		HdfsRpcLoggerScope log_scope(hdfs_handle.rpc_logger.lock());
		status = hdfs_handle.client->Stat(hdfs_path);
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to stat HDFS file '%s'", handle.path), error);
	}
	hdfs_handle.mtime = HdfsTimestampFromEpochMs(static_cast<int64_t>(status.mtime), handle.path);
	// A reader sees the file as it was at open, so its mtime can be cached; a writer's keeps changing.
	hdfs_handle.has_mtime = hdfs_handle.reader != nullptr;
	return hdfs_handle.mtime;
}

namespace {

//! The version tag of a file: the same identity reader sharing uses (file id, mtime, length). A file replaced by
//! one of the same length with its mtime kept (`hdfs dfs -cp -p`, `distcp -pt`) has a new inode id. With a tag,
//! DuckDB's external file cache trusts it alone (no recently-modified margin), so it must tell those versions apart.
string HdfsVersionTag(int64_t file_id, timestamp_t mtime, int64_t length) {
	return std::to_string(file_id) + ":" + std::to_string(Timestamp::GetEpochMs(mtime)) + ":" + std::to_string(length);
}

} // namespace

string HdfsFileSystem::GetVersionTag(FileHandle &handle) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (!hdfs_handle.reader) {
		return string();
	}
	return HdfsVersionTag(hdfs_handle.reader->FileId(), GetLastModifiedTime(handle),
	                      NumericCast<int64_t>(hdfs_handle.length));
}

FileType HdfsFileSystem::GetFileType(FileHandle &handle) {
	return FileType::FILE_TYPE_REGULAR;
}

optional<FileMetadata> HdfsFileSystem::GetStatsIfExists(const OpenFileInfo &file, optional_ptr<FileOpener> opener) {
	FileMetadata metadata;
	idx_t file_size;
	timestamp_t last_modified;
	string type;
	if (file.extended_info && file.extended_info->TryGetOption("file_size", file_size) &&
	    file.extended_info->TryGetOption("last_modified", last_modified) &&
	    (!file.extended_info->TryGetOption("type", type) || type == "file")) {
		metadata.file_size = NumericCast<int64_t>(file_size);
		metadata.last_modification_time = last_modified;
		metadata.file_type = FileType::FILE_TYPE_REGULAR;
		idx_t file_id;
		if (file.extended_info->TryGetOption("hdfs_file_id", file_id)) {
			metadata.file_id = file_id;
			metadata.version_tag = HdfsVersionTag(NumericCast<int64_t>(file_id), last_modified, metadata.file_size);
		}
		return metadata;
	}
	HdfsEntry entry;
	if (!TryStat(file.path, entry, opener)) {
		return nullopt;
	}
	metadata.file_size = entry.size;
	metadata.last_modification_time = entry.mtime;
	metadata.file_type = entry.is_dir ? FileType::FILE_TYPE_DIR : FileType::FILE_TYPE_REGULAR;
	if (entry.file_id > 0) {
		metadata.file_id = NumericCast<idx_t>(entry.file_id);
		metadata.version_tag = HdfsVersionTag(entry.file_id, entry.mtime, entry.size);
	}
	return metadata;
}

bool HdfsFileSystem::TryStat(const string &url, HdfsEntry &result, optional_ptr<FileOpener> opener) {
	HdfsFileStatus status;
	if (!TryStat(url, status, opener)) {
		return false;
	}
	string authority;
	string hdfs_path;
	ParseHdfsPath(url, authority, hdfs_path);
	result = MakeHdfsEntry(authority, status);
	return true;
}

bool HdfsFileSystem::TryStat(const string &url, HdfsFileStatus &result, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(url, authority, hdfs_path);
	try {
		result = GetConnection(url, authority, opener)->Run([&](const shared_ptr<HdfsClient> &client) {
			return client->Stat(hdfs_path);
		});
		return true;
	} catch (const HdfsError &error) {
		// A connection or permission error is not the same as "does not exist".
		if (error.type == HdfsErrorType::NOT_FOUND) {
			return false;
		}
		ThrowHdfsError(StringUtil::Format("Failed to stat HDFS path '%s'", url), error);
	}
}

bool HdfsFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	HdfsFileStatus status;
	return TryStat(filename, status, opener) && !status.is_dir;
}

bool HdfsFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	HdfsFileStatus status;
	return TryStat(directory, status, opener) && status.is_dir;
}

bool HdfsFileSystem::Exists(const string &url, optional_ptr<FileOpener> opener) {
	HdfsFileStatus status;
	return TryStat(url, status, opener);
}

HdfsEntry HdfsFileSystem::Stat(const string &url, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(url, authority, hdfs_path);
	try {
		return MakeHdfsEntry(authority,
		                     GetConnection(url, authority, opener)->Run([&](const shared_ptr<HdfsClient> &client) {
			                     return client->Stat(hdfs_path);
		                     }));
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to stat HDFS path '%s'", url), error);
	}
}

void HdfsFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(directory, authority, hdfs_path);
	try {
		// mkdirs is idempotent, so it can be retried like a read.
		GetConnection(directory, authority, opener)->Run([&](const shared_ptr<HdfsClient> &client) {
			client->CreateDirectories(hdfs_path);
		});
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to create HDFS directory '%s'", directory), error);
	}
}

namespace {

//! Delete `hdfs_path`; false if it didn't exist. After a connection failure the outcome is checked on a new
//! client instead of deleting blindly again (which would report a successful delete as "not found"): a path that
//! is gone was deleted; one that is still there is deleted again.
bool DeletePath(HdfsConnection &connection, const string &hdfs_path, bool recursive) {
	return connection.RunMutation(
	    [&](const shared_ptr<HdfsClient> &client) { return client->Delete(hdfs_path, recursive); },
	    [&](const shared_ptr<HdfsClient> &client, const HdfsError &) {
		    try {
			    client->Stat(hdfs_path);
		    } catch (const HdfsError &error) {
			    if (error.type == HdfsErrorType::NOT_FOUND) {
				    return true;
			    }
			    throw;
		    }
		    return client->Delete(hdfs_path, recursive);
	    });
}

//! A non-recursive delete refused because the directory has entries.
bool IsNotEmptyError(const HdfsError &error) {
	return error.message.find("PathIsNotEmptyDirectoryException") != string::npos ||
	       error.message.find("is non empty") != string::npos;
}

} // namespace

void HdfsFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(directory, authority, hdfs_path);
	bool deleted = false;
	try {
		deleted = DeletePath(*GetConnection(directory, authority, opener), hdfs_path, true);
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to remove HDFS directory '%s'", directory), error);
	}
	if (!deleted) {
		throw IOException("Failed to remove HDFS directory '%s': path not found", directory);
	}
}

bool HdfsFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                             optional_ptr<FileOpener> opener) {
	if (options.mode != CreateDirectoryMode::SINGLE && options.mode != CreateDirectoryMode::RECURSIVE) {
		throw InternalException("Unknown CreateDirectoryMode");
	}
	if (DirectoryExists(directory, opener)) {
		return false;
	}
	string authority;
	string hdfs_path;
	ParseHdfsPath(directory, authority, hdfs_path);
	try {
		GetConnection(directory, authority, opener)->Run([&](const shared_ptr<HdfsClient> &client) {
			if (options.mode == CreateDirectoryMode::RECURSIVE) {
				client->CreateDirectories(hdfs_path);
			} else {
				// SINGLE: a missing parent is an error, as for local directories.
				client->CreateDirectory(hdfs_path);
			}
		});
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to create HDFS directory '%s'", directory), error);
	}
	return true;
}

bool HdfsFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                             optional_ptr<FileOpener> opener) {
	if (options.mode != RemoveDirectoryMode::SINGLE && options.mode != RemoveDirectoryMode::RECURSIVE) {
		throw InternalException("Unknown RemoveDirectoryMode");
	}
	// Files are never removed here (an HDFS delete removes files too).
	if (!DirectoryExists(directory, opener)) {
		return false;
	}
	string authority;
	string hdfs_path;
	ParseHdfsPath(directory, authority, hdfs_path);
	auto recursive = options.mode == RemoveDirectoryMode::RECURSIVE;
	try {
		// SINGLE: the NameNode refuses a non-recursive delete of a non-empty directory (hidden entries count). A
		// directory removed concurrently is just not removed by us.
		return DeletePath(*GetConnection(directory, authority, opener), hdfs_path, recursive);
	} catch (const HdfsError &error) {
		if (!recursive && IsNotEmptyError(error)) {
			return false;
		}
		ThrowHdfsError(StringUtil::Format("Failed to remove HDFS directory '%s'", directory), error);
	}
}

void HdfsFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(filename, authority, hdfs_path);
	bool deleted = false;
	try {
		deleted = DeletePath(*GetConnection(filename, authority, opener), hdfs_path, false);
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to remove HDFS file '%s'", filename), error);
	}
	if (!deleted) {
		throw IOException("Failed to remove HDFS file '%s': path not found", filename);
	}
}

void HdfsFileSystem::MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener) {
	string source_authority;
	string source_path;
	ParseHdfsPath(source, source_authority, source_path);
	string target_authority;
	string target_path;
	ParseHdfsPath(target, target_authority, target_path);
	if (NormalizeAuthority(source_authority) != NormalizeAuthority(target_authority)) {
		throw NotImplementedException("Cannot move HDFS files across different NameNodes ('%s' -> '%s')", source,
		                              target);
	}
	// A rename is one NameNode call, made as one identity: both paths must resolve to the same configuration.
	auto connection = GetConnection(source, source_authority, opener);
	auto target_connection = GetConnection(target, target_authority, opener);
	if (!SameConfig(connection->config, target_connection->config)) {
		throw NotImplementedException(
		    "Cannot move HDFS file '%s' -> '%s': the two paths are configured by different hdfs secrets", source,
		    target);
	}
	try {
		// The source's length, to recognise a rename that took effect before a connection failure.
		auto source_status =
		    connection->Run([&](const shared_ptr<HdfsClient> &client) { return client->Stat(source_path); });
		connection->RunMutation(
		    [&](const shared_ptr<HdfsClient> &client) { client->Rename(source_path, target_path, true); },
		    [&](const shared_ptr<HdfsClient> &client, const HdfsError &failure) {
			    HdfsFileStatus status;
			    try {
				    client->Stat(source_path);
				    // The source is still there: the rename did not happen, so it is safe to send again.
				    client->Rename(source_path, target_path, true);
				    return;
			    } catch (const HdfsError &error) {
				    if (error.type != HdfsErrorType::NOT_FOUND) {
					    throw;
				    }
			    }
			    try {
				    status = client->Stat(target_path);
			    } catch (const HdfsError &error) {
				    if (error.type != HdfsErrorType::NOT_FOUND) {
					    throw;
				    }
				    status.is_dir = true; // no target: not a match below
			    }
			    if (status.is_dir || status.length != source_status.length) {
				    throw HdfsError(HdfsErrorType::IO,
				                    failure.message + "; after reconnecting, the source is gone but the target doesn't "
				                                      "match it: the outcome is unknown");
			    }
			    // The source is gone and the target is the source's file: the rename took effect.
		    });
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to move HDFS file '%s' -> '%s'", source, target), error);
	}
}

bool HdfsFileSystem::ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
                                       optional_ptr<FileOpener> opener) {
	// Hidden entries are always included: DuckDB acts on this listing (e.g. COPY's directory-not-empty check and
	// OVERWRITE deletion), so hiding them would hide files from those checks.
	HdfsWalkOptions options;
	options.include_hidden = true;
	auto stream = OpenListStream(directory, options, opener);
	auto context = FileOpener::TryGetClientContext(opener);
	bool any = false;
	vector<HdfsEntry> batch;
	while (stream->Next(batch, STANDARD_VECTOR_SIZE, context)) {
		for (auto &entry : batch) {
			// The callback receives bare child names.
			OpenFileInfo info(entry.name);
			info.extended_info = MakeExtendedInfo(entry);
			callback(info);
			any = true;
		}
	}
	return any;
}

namespace {

//! The files a glob matches. The walk lists directories in parallel and finishes in no fixed order, so the result
//! is collected and sorted on first use: scans see files in path order, as with local globs.
class HdfsGlobResult : public LazyMultiFileList {
public:
	HdfsGlobResult(HdfsFileSystem &fs, string pattern_p, HdfsWalkOptions options, optional_ptr<FileOpener> opener)
	    : LazyMultiFileList(FileOpener::TryGetClientContext(opener)), fs(fs), pattern(std::move(pattern_p)),
	      options(options), opener(opener) {
	}

protected:
	bool ExpandNextPath() const override {
		if (expanded) {
			return false;
		}
		expanded = true;
		auto stream = fs.OpenGlobStream(pattern, options, opener);
		vector<HdfsEntry> batch;
		while (stream->Next(batch, STANDARD_VECTOR_SIZE, context)) {
			for (auto &entry : batch) {
				// Globs match directories too (e.g. "dir/*"); only files are scanned.
				if (!entry.is_dir) {
					expanded_files.push_back(MakeOpenFileInfo(entry));
				}
			}
		}
		// As in DuckDB's local glob: a pattern that matches nothing may be a file whose name contains glob characters.
		// One stat answers both "does it exist" and "what is it".
		HdfsEntry entry;
		if (expanded_files.empty() && IsHdfsGlob(pattern) && fs.TryStat(pattern, entry, opener) && !entry.is_dir) {
			expanded_files.push_back(MakeOpenFileInfo(entry));
		}
		std::sort(expanded_files.begin(), expanded_files.end());
		return !expanded_files.empty();
	}

private:
	static OpenFileInfo MakeOpenFileInfo(const HdfsEntry &entry) {
		OpenFileInfo info(entry.url);
		info.extended_info = MakeExtendedInfo(entry);
		return info;
	}

	HdfsFileSystem &fs;
	const string pattern;
	const HdfsWalkOptions options;
	optional_ptr<FileOpener> opener;
	mutable bool expanded = false;
};

} // namespace

unique_ptr<MultiFileList> HdfsFileSystem::GlobFilesExtended(const string &path, const FileGlobInput &input,
                                                            optional_ptr<FileOpener> opener) {
	HdfsWalkOptions options;
	options.max_parallelism = DEFAULT_HDFS_LIST_PARALLELISM;
	Value setting;
	if (FileOpener::TryGetCurrentSetting(opener, "hdfs_list_parallelism", setting) && !setting.IsNull()) {
		options.max_parallelism = UBigIntValue::Get(setting);
	}
	options.skip_permission_errors = WalkSetting(opener, "hdfs_skip_permission_errors", false);
	options.include_hidden = WalkSetting(opener, "hdfs_include_hidden", false);
	options.batched_listing = WalkSetting(opener, "hdfs_batched_listing", true);
	return make_uniq<HdfsGlobResult>(*this, path, options, opener);
}

HdfsListStream::HdfsListStream(shared_ptr<HdfsConnection> connection_p, string url_p, bool glob,
                               HdfsWalkOptions options)
    : connection(std::move(connection_p)), url(std::move(url_p)), glob(glob), options(options) {
	ParseHdfsPath(url, authority, hdfs_path);
	Open();
}

void HdfsListStream::Open() {
	client = connection->Get();
	try {
		walk = glob ? client->Glob(hdfs_path, options) : client->List(hdfs_path, false, options);
	} catch (const HdfsError &error) {
		// Only an invalid pattern fails here; cluster errors surface in Next().
		ThrowHdfsError(StringUtil::Format("Failed to glob HDFS pattern '%s'", url), error);
	}
}

bool HdfsListStream::Next(vector<HdfsEntry> &out, idx_t max_entries, optional_ptr<ClientContext> context) {
	// How long to wait for entries between interrupt checks.
	static constexpr int32_t INTERRUPT_CHECK_INTERVAL_MS = 100;
	out.clear();
	for (;;) {
		bool pending = false;
		bool more;
		try {
			more = walk->Next(batch, max_entries, context ? INTERRUPT_CHECK_INTERVAL_MS : -1, pending);
		} catch (const HdfsError &error) {
			// Retry once on a new client, as HdfsConnection::Run does, but only while nothing has been handed out:
			// the walk restarts from the root, so a later retry would repeat entries.
			if (error.type == HdfsErrorType::CONNECTION && !emitted_any && !retried) {
				retried = true;
				walk.reset();
				connection->Invalidate(client);
				Open();
				continue;
			}
			ThrowHdfsError(
			    StringUtil::Format(glob ? "Failed to glob HDFS pattern '%s'" : "Failed to list HDFS path '%s'", url),
			    error);
		}
		if (pending) {
			context->BlockingInterruptCheck();
			continue;
		}
		for (auto &status : batch) {
			// The caller's own spelling of the authority: a connection is shared by spellings that differ in the
			// host's case, and the URLs are matched against secret scopes and allowed paths as given.
			out.push_back(MakeHdfsEntry(authority, status));
		}
		emitted_any |= !out.empty();
		return more;
	}
}

unique_ptr<HdfsListStream> HdfsFileSystem::OpenListStream(const string &url, HdfsWalkOptions options,
                                                          optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(url, authority, hdfs_path);
	return make_uniq<HdfsListStream>(GetConnection(url, authority, opener), url, false, options);
}

unique_ptr<HdfsListStream> HdfsFileSystem::OpenGlobStream(const string &url, HdfsWalkOptions options,
                                                          optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(url, authority, hdfs_path);
	return make_uniq<HdfsListStream>(GetConnection(url, authority, opener), url, true, options);
}

bool HdfsFileSystem::CanHandleFile(const string &fpath) {
	return IsHdfsUrl(fpath);
}

bool HdfsFileSystem::IsHdfsUrl(const string &path) {
	return StringUtil::StartsWith(path, "hdfs://");
}

namespace {

struct HdfsResetInfo : public TableFunctionInfo {
	explicit HdfsResetInfo(HdfsFileSystem &hdfs) : hdfs(hdfs) {
	}

	reference<HdfsFileSystem> hdfs;
};

struct HdfsResetBindData : public TableFunctionData {
	explicit HdfsResetBindData(HdfsFileSystem &hdfs) : hdfs(hdfs) {
	}

	reference<HdfsFileSystem> hdfs;
};

struct HdfsResetState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> HdfsResetBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.push_back(LogicalType::UBIGINT);
	names.emplace_back("connections_dropped");
	return make_uniq<HdfsResetBindData>(input.info->Cast<HdfsResetInfo>().hdfs.get());
}

unique_ptr<GlobalTableFunctionState> HdfsResetInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<HdfsResetState>();
}

void HdfsResetExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<HdfsResetState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto dropped = input.bind_data->Cast<HdfsResetBindData>().hdfs.get().ResetConnections();
	output.data[0].Append(Value::UBIGINT(dropped));
}

} // namespace

void RegisterHdfsConnectionFunctions(ExtensionLoader &loader, HdfsFileSystem &hdfs) {
	// CALL hdfs_reset_connections(): the next access reconnects, picking up replaced secrets, edited
	// core-site.xml / hdfs-site.xml and a new Kerberos ticket cache.
	TableFunction reset("hdfs_reset_connections", {}, HdfsResetExecute, HdfsResetBind, HdfsResetInit);
	reset.function_info = make_shared_ptr<HdfsResetInfo>(hdfs);
	loader.RegisterFunction(std::move(reset));
}

} // namespace duckdb
