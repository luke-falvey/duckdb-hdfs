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
};

struct HdfsFileHandle : public FileHandle {
public:
	HdfsFileHandle(FileSystem &fs, string path, FileOpenFlags flags, shared_ptr<HdfsClient> client,
	               shared_ptr<HdfsReader> reader, unique_ptr<HdfsWriter> writer)
	    : FileHandle(fs, std::move(path), flags), client(std::move(client)), reader(std::move(reader)),
	      writer(std::move(writer)) {
	}
	~HdfsFileHandle() override {
		// Close() can throw (a writer flush may fail); an explicit Close() surfaces the error.
		try {
			HdfsFileHandle::Close();
		} catch (...) { // NOLINT: destructors must not throw
		}
	}

	//! The client the handle was opened on: a reconnect may replace the filesystem's client while it is open.
	shared_ptr<HdfsClient> client;
	shared_ptr<HdfsReader> reader;
	unique_ptr<HdfsWriter> writer;
	//! Cursor for the streaming Read/Write/Seek interface.
	idx_t position = 0;
	//! The file length for readers; bytes written so far for writers.
	idx_t length = 0;
	//! The modification time, from the listing that found the file or fetched once.
	bool has_mtime = false;
	timestamp_t mtime;

public:
	void Close() override;
};

//! A listing or glob running in the background (backs hdfs_ls): entries arrive in batches while the walk
//! continues, in no particular order. Not thread-safe.
class HdfsListStream {
public:
	HdfsListStream(HdfsConnection &connection, string url, bool glob, HdfsWalkOptions options);

	//! Replace `out` with the next batch (at most `max_entries`), blocking until one arrives. Returns false at the
	//! end. A connection failure is retried once while no entries have been handed out yet. With a `context`, the
	//! wait checks for interrupts, so the query can be cancelled during a long walk.
	bool Next(vector<HdfsEntry> &out, idx_t max_entries, optional_ptr<ClientContext> context = nullptr);

private:
	void Open();

	HdfsConnection &connection;
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
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

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
	//! HDFS mkdirs creates missing parents, so SINGLE behaves like RECURSIVE.
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	//! SINGLE removes only an existing, empty directory.
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override;
	//! Each entry carries type/size/last-modified in OpenFileInfo::extended_info.
	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;
	bool SupportsListFilesExtended() const override {
		return true;
	}
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;
	//! Durability comes with closing the writer (HdfsWriter::Close).
	void FileSync(FileHandle &handle) override {
	}
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

	bool CanHandleFile(const string &fpath) override;
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
	//! The configuration of the hdfs secret matching `url` (empty without one).
	static HdfsConnectionConfig LookupConfig(const string &url, optional_ptr<FileOpener> opener);
	//! The connection for `url`: its authority ("hdfs://host:port", or "" for the default FS) and the configuration
	//! of the hdfs secret matching it. Created once, never removed.
	HdfsConnection &GetConnection(const string &url, const string &authority, optional_ptr<FileOpener> opener);
	//! `expected_length`: the size a listing reported (-1 if unknown).
	unique_ptr<FileHandle> OpenFileInternal(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener,
	                                        int64_t expected_length);
	//! Status of `url`, or false if it doesn't exist.
	bool TryStat(const string &url, HdfsFileStatus &result, optional_ptr<FileOpener> opener);

	mutex connections_lock;
	unordered_map<string, unique_ptr<HdfsConnection>> connections;
};

} // namespace duckdb
