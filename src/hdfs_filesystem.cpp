#include "hdfs_filesystem.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret.hpp"

#include <algorithm>

namespace duckdb {

namespace {

//! Split a URL into its authority ("hdfs://host:port", or "" for the default FS) and the scheme-less path ("/a/b").
//! Inputs without the hdfs:// scheme are a path on the default FS.
void ParseHdfsPath(const string &path, string &authority, string &hdfs_path) {
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
	entry.mtime = Timestamp::FromEpochMs(NumericCast<int64_t>(status.mtime));
	entry.atime = Timestamp::FromEpochMs(NumericCast<int64_t>(status.atime));
	return entry;
}

//! DuckDB's extended file info from a listing, so opening the file costs no extra stat.
shared_ptr<ExtendedOpenFileInfo> MakeExtendedInfo(const HdfsEntry &entry) {
	auto info = make_shared_ptr<ExtendedOpenFileInfo>();
	info->options.emplace("type", Value(entry.is_dir ? "directory" : "file"));
	info->options.emplace("file_size", Value::BIGINT(entry.size));
	info->options.emplace("last_modified", Value::TIMESTAMP(entry.mtime));
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

	//! Run `op(client)`. A connection failure (stale client after a failover, a dropped socket, an expired ticket)
	//! is retried once on a new client; other HdfsErrors propagate.
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

	const string authority;
	const HdfsConnectionConfig config;

private:
	mutex lock;
	shared_ptr<HdfsClient> client;
};

void HdfsFileHandle::Close() {
	if (reader || writer) {
		DUCKDB_LOG_FILE_SYSTEM_CLOSE((*this));
	}
	reader.reset();
	if (writer) {
		// Release it first, so a second Close() (e.g. from the destructor) is a no-op even if this one throws.
		auto closing = std::move(writer);
		try {
			closing->Close();
		} catch (const HdfsError &error) {
			ThrowHdfsError(StringUtil::Format("Failed to close HDFS file '%s'", path), error);
		}
	}
}

HdfsFileSystem::HdfsFileSystem() = default;

HdfsFileSystem::~HdfsFileSystem() = default;

HdfsConnectionConfig HdfsFileSystem::LookupConfig(const string &url, optional_ptr<FileOpener> opener) {
	HdfsConnectionConfig config;
	if (!opener) {
		return config;
	}
	FileOpenerInfo info {url};
	KeyValueSecretReader secret(*opener, &info, "hdfs");
	Value value;
	if (secret.TryGetSecretKey("config_dir", value) && !value.IsNull()) {
		config.config_dir = value.ToString();
	}
	if (secret.TryGetSecretKey("user", value) && !value.IsNull()) {
		config.user = value.ToString();
	}
	return config;
}

HdfsConnection &HdfsFileSystem::GetConnection(const string &url, const string &authority,
                                              optional_ptr<FileOpener> opener) {
	auto config = LookupConfig(url, opener);
	auto key = authority + '\0' + config.config_dir + '\0' + config.user;
	lock_guard<mutex> guard(connections_lock);
	auto &connection = connections[key];
	if (!connection) {
		connection = make_uniq<HdfsConnection>(authority, std::move(config));
	}
	return *connection;
}

unique_ptr<FileHandle> HdfsFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                        optional_ptr<FileOpener> opener) {
	if (!file.extended_info || flags.OpenForWriting()) {
		return OpenFileInternal(file.path, flags, opener, -1);
	}
	int64_t expected_length = -1;
	idx_t file_size;
	if (file.extended_info->TryGetOption("file_size", file_size)) {
		expected_length = NumericCast<int64_t>(file_size);
	}
	auto handle = OpenFileInternal(file.path, flags, opener, expected_length);
	timestamp_t mtime;
	if (handle && file.extended_info->TryGetOption("last_modified", mtime)) {
		auto &hdfs_handle = handle->Cast<HdfsFileHandle>();
		hdfs_handle.mtime = mtime;
		hdfs_handle.has_mtime = true;
	}
	return handle;
}

unique_ptr<FileHandle> HdfsFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                optional_ptr<FileOpener> opener) {
	return OpenFileInternal(path, flags, opener, -1);
}

unique_ptr<FileHandle> HdfsFileSystem::OpenFileInternal(const string &path, FileOpenFlags flags,
                                                        optional_ptr<FileOpener> opener, int64_t expected_length) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(path, authority, hdfs_path);
	auto &connection = GetConnection(path, authority, opener);
	unique_ptr<HdfsFileHandle> handle;
	if (flags.OpenForWriting()) {
		auto overwrite = flags.OverwriteExistingFile();
		try {
			connection.Run([&](const shared_ptr<HdfsClient> &client) {
				auto writer = client->Create(hdfs_path, overwrite);
				handle = make_uniq<HdfsFileHandle>(*this, path, flags, client, nullptr, std::move(writer));
			});
		} catch (const HdfsError &error) {
			ThrowHdfsError(StringUtil::Format("Failed to open HDFS file for writing '%s'", path), error);
		}
	} else {
		try {
			connection.Run([&](const shared_ptr<HdfsClient> &client) {
				auto reader = client->Open(hdfs_path, expected_length);
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
	}
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
	throw NotImplementedException("Random/positional writes are not supported on HDFS (append-only)");
}

int64_t HdfsFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (!hdfs_handle.writer) {
		throw IOException("File not opened for writing: " + handle.path);
	}
	if (nr_bytes < 0) {
		throw InvalidInputException("Cannot write a negative number of bytes to HDFS file '%s'", handle.path);
	}
	try {
		hdfs_handle.writer->Write(static_cast<const_data_ptr_t>(buffer), NumericCast<idx_t>(nr_bytes));
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to write to HDFS file '%s'", handle.path), error);
	}
	DUCKDB_LOG_FILE_SYSTEM_WRITE(handle, nr_bytes, hdfs_handle.position);
	hdfs_handle.position += NumericCast<idx_t>(nr_bytes);
	hdfs_handle.length += NumericCast<idx_t>(nr_bytes);
	return nr_bytes;
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
		status = hdfs_handle.client->Stat(hdfs_path);
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to stat HDFS file '%s'", handle.path), error);
	}
	hdfs_handle.mtime = Timestamp::FromEpochMs(NumericCast<int64_t>(status.mtime));
	// A reader sees the file as it was at open, so its mtime can be cached; a writer's keeps changing.
	hdfs_handle.has_mtime = hdfs_handle.reader != nullptr;
	return hdfs_handle.mtime;
}

string HdfsFileSystem::GetVersionTag(FileHandle &handle) {
	auto &hdfs_handle = handle.Cast<HdfsFileHandle>();
	if (!hdfs_handle.reader) {
		return string();
	}
	return std::to_string(Timestamp::GetEpochMs(GetLastModifiedTime(handle))) + ":" +
	       std::to_string(hdfs_handle.length);
}

bool HdfsFileSystem::TryStat(const string &url, HdfsFileStatus &result, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(url, authority, hdfs_path);
	try {
		result = GetConnection(url, authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
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
		                     GetConnection(url, authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
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
		GetConnection(directory, authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
			client->CreateDirectories(hdfs_path);
		});
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to create HDFS directory '%s'", directory), error);
	}
}

void HdfsFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(directory, authority, hdfs_path);
	bool deleted = false;
	try {
		deleted = GetConnection(directory, authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
			return client->Delete(hdfs_path, true);
		});
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to remove HDFS directory '%s'", directory), error);
	}
	if (!deleted) {
		throw IOException("Failed to remove HDFS directory '%s': path not found", directory);
	}
}

bool HdfsFileSystem::CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
                                             optional_ptr<FileOpener> opener) {
	if (DirectoryExists(directory, opener)) {
		return false;
	}
	CreateDirectory(directory, opener);
	return true;
}

bool HdfsFileSystem::RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
                                             optional_ptr<FileOpener> opener) {
	if (!DirectoryExists(directory, opener)) {
		return false;
	}
	if (options.mode == RemoveDirectoryMode::RECURSIVE) {
		RemoveDirectory(directory, opener);
		return true;
	}
	// SINGLE: never remove contents; hidden entries count.
	HdfsWalkOptions walk_options;
	walk_options.include_hidden = true;
	vector<HdfsEntry> children;
	if (OpenListStream(directory, walk_options, opener)->Next(children, 1, FileOpener::TryGetClientContext(opener))) {
		return false;
	}
	string authority;
	string hdfs_path;
	ParseHdfsPath(directory, authority, hdfs_path);
	try {
		return GetConnection(directory, authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
			return client->Delete(hdfs_path, false);
		});
	} catch (const HdfsError &error) {
		ThrowHdfsError(StringUtil::Format("Failed to remove HDFS directory '%s'", directory), error);
	}
}

void HdfsFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	string authority;
	string hdfs_path;
	ParseHdfsPath(filename, authority, hdfs_path);
	bool deleted = false;
	try {
		deleted = GetConnection(filename, authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
			return client->Delete(hdfs_path, false);
		});
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
	if (source_authority != target_authority) {
		throw NotImplementedException("Cannot move HDFS files across different NameNodes ('%s' -> '%s')", source,
		                              target);
	}
	try {
		GetConnection(source, source_authority, opener).Run([&](const shared_ptr<HdfsClient> &client) {
			client->Rename(source_path, target_path, true);
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
		if (expanded_files.empty() && FileSystem::HasGlob(pattern) && fs.Exists(pattern, opener)) {
			auto entry = fs.Stat(pattern, opener);
			if (!entry.is_dir) {
				expanded_files.push_back(MakeOpenFileInfo(entry));
			}
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

HdfsListStream::HdfsListStream(HdfsConnection &connection, string url_p, bool glob, HdfsWalkOptions options)
    : connection(connection), url(std::move(url_p)), glob(glob), options(options) {
	ParseHdfsPath(url, authority, hdfs_path);
	Open();
}

void HdfsListStream::Open() {
	client = connection.Get();
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
				connection.Invalidate(client);
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
			out.push_back(MakeHdfsEntry(connection.authority, status));
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
	return StringUtil::StartsWith(fpath, "hdfs://");
}

} // namespace duckdb
