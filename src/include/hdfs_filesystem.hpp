//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_filesystem.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "hdfs_client.hpp"

namespace duckdb {

class ExtensionLoader;
class HdfsConnection;

//! Default for the hdfs_list_parallelism setting.
static constexpr idx_t DEFAULT_HDFS_LIST_PARALLELISM = 16;

//! A metadata row for one HDFS path (hdfs_ls / hdfs_stat). `url` carries the authority ("hdfs://host:port/a/b");
//! `name` is the basename. `replication` and `block_size` are -1 for directories.
struct HdfsEntry {
	string url;
	string name;
	bool is_dir = false;
	int64_t size = 0;
	string owner;
	string group;
	uint16_t permission = 0;
	int32_t replication = -1;
	int64_t block_size = -1;
	timestamp_t mtime;
	timestamp_t atime;
	//! The NameNode's inode id (0 if unknown).
	int64_t file_id = 0;
};

struct HdfsFileHandle : public FileHandle {
public:
	HdfsFileHandle(FileSystem &fs, string path, FileOpenFlags flags, shared_ptr<HdfsClient> client,
	               shared_ptr<HdfsReader> reader, unique_ptr<HdfsWriter> writer)
	    : FileHandle(fs, std::move(path), flags), client(std::move(client)), reader(std::move(reader)),
	      writer(std::move(writer)) {
	}
	//! A writer dropped without Close() commits, as local files do (core's EXPORT DATABASE relies on it), unless it
	//! is destroyed while an exception unwinds or after a failed write: that write is aborted instead.
	~HdfsFileHandle() override;

	//! The client the handle was opened on: a reconnect may replace the filesystem's client while it is open.
	shared_ptr<HdfsClient> client;
	shared_ptr<HdfsReader> reader;
	unique_ptr<HdfsWriter> writer;
	//! Cursor for the streaming Read/Write/Seek interface.
	idx_t position = 0;
	//! The file length for readers; bytes written so far for writers.
	idx_t length = 0;
	//! The modification time: a reader's is that of the version it reads; a writer's is fetched when asked.
	bool has_mtime = false;
	timestamp_t mtime;
	//! A write (or closing the writer) failed: the file is incomplete, and only Abort() is left.
	bool failed = false;

public:
	//! Complete the file (writers). If that fails, the file is aborted (removed) and the error thrown.
	void Close() override;
	//! Abandon a write: remove the file this handle created, without completing it. Readers just close. Throws if
	//! the file could not be removed.
	void Abort();
};

//! A listing or glob running in the background (backs hdfs_ls): entries arrive in batches while the walk
//! continues, in no particular order. Not thread-safe.
class HdfsListStream {
public:
	HdfsListStream(shared_ptr<HdfsConnection> connection, string url, bool glob, HdfsWalkOptions options);

	//! Replace `out` with the next batch (at most `max_entries`), blocking until one arrives. Returns false at the
	//! end. A connection failure is retried once while no entries have been handed out yet. With a `context`, the
	//! wait checks for interrupts, so the query can be cancelled during a long walk.
	bool Next(vector<HdfsEntry> &out, idx_t max_entries, optional_ptr<ClientContext> context = nullptr);

private:
	void Open();

	shared_ptr<HdfsConnection> connection;
	const string url;
	string authority;
	string hdfs_path;
	const bool glob;
	const HdfsWalkOptions options;
	shared_ptr<HdfsClient> client;
	unique_ptr<HdfsWalk> walk;
	vector<HdfsFileStatus> batch;
	bool emitted_any = false;
	bool retried = false;
};

class HdfsFileSystem : public FileSystem {
public:
	HdfsFileSystem();
	~HdfsFileSystem() override;

public:
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	//! Uses the size/mtime a listing returned (OpenFileInfo::extended_info) instead of asking again.
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;
	bool SupportsOpenFileExtended() const override {
		return true;
	}

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	//! HDFS files are written sequentially: a write must start at the end of what was written so far.
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	//! Removes the file the handle created, without completing it (see HdfsFileHandle::Abort).
	void AbortFileWrite(FileHandle &handle) override;

	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	void Reset(FileHandle &handle) override;

	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	//! "<mtime>:<length>", so the external file cache validates without another RPC.
	string GetVersionTag(FileHandle &handle) override;

	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	bool DirectoryExists(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	//! SINGLE requires the parent to exist; RECURSIVE creates missing parents. Returns false if the directory exists.
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	//! SINGLE removes only an existing, empty directory (one non-recursive delete); it returns false, not an error,
	//! if the directory is missing or not empty, also when that changes concurrently.
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	//! Each entry carries type/size/last-modified in OpenFileInfo::extended_info.
	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;
	bool SupportsListFilesExtended() const override {
		return true;
	}
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	//! Source and target must be on the same NameNode, under the same hdfs secret configuration.
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;
	//! Writers: hsync what was written so far to the DataNodes' disks (the file completes only on Close).
	void FileSync(FileHandle &handle) override;
	//! Globs (with Hadoop-style {a,b} alternation) run as a parallel walk; see HdfsGlobResult.
	unique_ptr<MultiFileList> GlobFilesExtended(const string &path, const FileGlobInput &input,
	                                            optional_ptr<FileOpener> opener = nullptr) override;
	bool SupportsGlobExtended() const override {
		return true;
	}

	//! Metadata for the SQL functions. Unlike globs, these keep directories and the full file status.
	//! OpenListStream lists `url`'s children (a file lists as itself); OpenGlobStream returns the entries a pattern
	//! matches (throws for an invalid pattern). The `opener` finds the hdfs secret for the URL.
	unique_ptr<HdfsListStream> OpenListStream(const string &url, HdfsWalkOptions options,
	                                          optional_ptr<FileOpener> opener);
	unique_ptr<HdfsListStream> OpenGlobStream(const string &url, HdfsWalkOptions options,
	                                          optional_ptr<FileOpener> opener);
	HdfsEntry Stat(const string &url, optional_ptr<FileOpener> opener);
	//! Whether `url` exists (file or directory).
	bool Exists(const string &url, optional_ptr<FileOpener> opener);
	//! Drop every cached connection (hdfs_reset_connections()), so the next access reconnects with the current
	//! secrets and Hadoop configuration. Open files and running listings keep their client. Returns the number
	//! dropped.
	idx_t ResetConnections();

	bool CanHandleFile(const string &fpath) override;
	//! Whether `path` is an hdfs:// URL (the paths this file system handles).
	static bool IsHdfsUrl(const string &path);
	string GetName() const override {
		return "HdfsFileSystem";
	}
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}

private:
	//! The connection for `url`: its authority ("hdfs://host:port", or "" for the default FS) and the configuration
	//! of the hdfs secret matching it. Cached per (normalized) authority and secret, and replaced when that secret's
	//! configuration changes.
	shared_ptr<HdfsConnection> GetConnection(const string &url, const string &authority,
	                                         optional_ptr<FileOpener> opener);
	//! `expected`: the status a listing reported (null if unknown).
	unique_ptr<FileHandle> OpenFileInternal(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener,
	                                        optional_ptr<const HdfsFileStatus> expected);
	//! Status of `url`, or false if it doesn't exist.
	bool TryStat(const string &url, HdfsFileStatus &result, optional_ptr<FileOpener> opener);

	mutex connections_lock;
	unordered_map<string, shared_ptr<HdfsConnection>> connections;
};

//! Registers hdfs_reset_connections().
void RegisterHdfsConnectionFunctions(ExtensionLoader &loader, HdfsFileSystem &hdfs);

} // namespace duckdb
