//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_client.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//! What kind of failure an HdfsError is, for callers that react to it (retry, "not found", ...).
enum class HdfsErrorType {
	IO,
	NOT_FOUND,
	PERMISSION,
	ALREADY_EXISTS,
	CONNECTION,
	INVALID_INPUT,
	INVALID_CONFIGURATION
};

//! A failed HDFS operation. HdfsFileSystem turns these into DuckDB exceptions with the URL as context.
class HdfsError : public IOException {
public:
	HdfsError(HdfsErrorType type, const string &message) : IOException(message), type(type), message(message) {
	}

	HdfsErrorType type;
	//! The message without DuckDB's error formatting.
	string message;
};

//! One file or directory's status. `path` is scheme-less ("/a/b"); `replication` and `block_size` are -1 for
//! directories; times are epoch milliseconds.
struct HdfsFileStatus {
	string path;
	bool is_dir = false;
	int64_t length = 0;
	uint64_t mtime = 0;
	uint64_t atime = 0;
	string owner;
	string group;
	uint16_t permission = 0;
	int32_t replication = -1;
	int64_t block_size = -1;
};

//! Behavior of listings and globs that walk more than one directory.
struct HdfsWalkOptions {
	//! Prune subtrees below the walk root whose listing fails with a permission error, instead of failing.
	bool skip_permission_errors = false;
	//! Return (and descend into) entries named with a leading '.' or '_' (Hadoop's hidden convention). When false,
	//! they are skipped unless a glob component names them explicitly or the entry is the listed path itself.
	bool include_hidden = false;
	//! List several directories per NameNode call (getBatchedListing, Hadoop 3.3+).
	bool batched_listing = true;
	//! Maximum number of concurrent listing calls.
	idx_t max_parallelism = 1;
};

//! Where a connection's configuration comes from (an `hdfs` secret); empty fields fall back to the Hadoop
//! environment (HADOOP_CONF_DIR or HADOOP_HOME/etc/hadoop, HADOOP_USER_NAME).
struct HdfsConnectionConfig {
	//! Directory holding core-site.xml / hdfs-site.xml.
	string config_dir;
	//! The user to act as (simple authentication only).
	string user;
};

//! An open file. Reads may run concurrently.
class HdfsReader {
public:
	virtual ~HdfsReader() = default;

	virtual int64_t Length() const = 0;
	//! Read exactly `nr_bytes` at `offset`; files in an encryption zone are decrypted.
	virtual void Read(data_ptr_t buffer, idx_t nr_bytes, idx_t offset) = 0;
};

//! A file being written (HDFS files are append-only).
class HdfsWriter {
public:
	virtual ~HdfsWriter() = default;

	//! Append `nr_bytes`; files in an encryption zone are encrypted.
	virtual void Write(const_data_ptr_t buffer, idx_t nr_bytes) = 0;
	//! Flush and complete the file; the data is durable once this returns.
	virtual void Close() = 0;
};

//! A directory walk running in the background (a listing or a glob). Not thread-safe.
class HdfsWalk {
public:
	virtual ~HdfsWalk() = default;

	//! Replace `out` with up to `max_entries` entries, waiting up to `timeout_ms` (-1: no limit) for at least one.
	//! Sets `pending` (with `out` empty) if the wait ran out first. Returns false once the walk is complete.
	virtual bool Next(vector<HdfsFileStatus> &out, idx_t max_entries, int32_t timeout_ms, bool &pending) = 0;
};

//! A connection to one HDFS cluster. Thread-safe. Failures throw HdfsError.
class HdfsClient {
public:
	virtual ~HdfsClient() = default;

	//! Connect to `authority` ("hdfs://host:port"), or to fs.defaultFS if empty. On a Kerberos cluster, the identity
	//! is the principal in the Kerberos ticket cache.
	static shared_ptr<HdfsClient> Connect(const string &authority, const HdfsConnectionConfig &config);

	virtual HdfsFileStatus Stat(const string &path) = 0;
	//! Open a file for reading. A file opened again while a reader on it is still open shares that reader, unless
	//! `expected_length` (the size a listing reported; -1 if unknown) shows the file changed since.
	virtual shared_ptr<HdfsReader> Open(const string &path, int64_t expected_length) = 0;
	//! Create a file (and missing parent directories) for writing.
	virtual unique_ptr<HdfsWriter> Create(const string &path, bool overwrite) = 0;
	//! Create a directory and its missing parents.
	virtual void CreateDirectories(const string &path) = 0;
	//! Delete a file or directory; returns false if it doesn't exist.
	virtual bool Delete(const string &path, bool recursive) = 0;
	//! Atomically rename `source` to `target`; with `overwrite`, an existing file (or empty directory) is replaced.
	virtual void Rename(const string &source, const string &target, bool overwrite) = 0;
	//! List `path`'s children (`recursive`: the whole subtree); a file lists as itself.
	virtual unique_ptr<HdfsWalk> List(const string &path, bool recursive, const HdfsWalkOptions &options) = 0;
	//! The entries (files and directories) a glob pattern matches: `*`, `?`, `[...]`, `\` escapes, `{a,b}` and `**`.
	//! Throws an INVALID_INPUT HdfsError for an invalid pattern.
	virtual unique_ptr<HdfsWalk> Glob(const string &pattern, const HdfsWalkOptions &options) = 0;
};

} // namespace duckdb
