//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_client.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class Logger;
class HdfsGlobPlan;

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
	//! The NameNode's inode id (0 if unknown). A file replaced by another one under the same path gets a new id.
	int64_t file_id = 0;
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
	//! Maximum number of concurrent listing calls (worker threads of one walk), 1 to MAX_PARALLELISM.
	idx_t max_parallelism = 1;
	//! Upper bound of max_parallelism (and of the hdfs_list_parallelism setting): each walk starts its own threads.
	static constexpr idx_t MAX_PARALLELISM = 64;
};

//! The providers of the `hdfs` secrets this version creates and reads. `config` names where the configuration is
//! (CONFIG_DIR, read at each connect); `config_snapshot` holds the configuration itself (Hadoop properties).
static constexpr const char *HDFS_SECRET_PROVIDER_CONFIG = "config";
static constexpr const char *HDFS_SECRET_PROVIDER_SNAPSHOT = "config_snapshot";
//! The format of the keys a `config_snapshot` secret stores (its `format_version`). A secret with a higher version
//! was written by a newer version of the extension and is refused.
static constexpr int32_t HDFS_SNAPSHOT_FORMAT_VERSION = 1;

//! Where a connection's configuration comes from (an `hdfs` secret); empty fields fall back to the Hadoop
//! environment (HADOOP_CONF_DIR or HADOOP_HOME/etc/hadoop, HADOOP_USER_NAME).
struct HdfsConnectionConfig {
	//! Directory holding core-site.xml / hdfs-site.xml.
	string config_dir;
	//! The user to act as (simple authentication only).
	string user;
	//! A configuration held in a `config_snapshot` secret: Hadoop properties (raw keys and values, as an XML file
	//! would give them), used instead of any XML file or HADOOP_CONF_DIR. HADOOP_USER_NAME still applies when `user`
	//! is empty. With a snapshot, `config_dir` is empty and `fingerprint` is set; `properties` itself is only filled in
	//! when a connection is built (see HdfsFileSystem::GetConnection).
	shared_ptr<const map<string, string>> properties;
	//! The snapshot's Fingerprint(), stored in its secret; empty without a snapshot.
	string fingerprint;

	//! Whether this is a snapshot's configuration.
	bool IsSnapshot() const {
		return !fingerprint.empty();
	}
	//! What the connection is configured from, as one string: two configs are the same configuration iff their
	//! identities are equal. For a CONFIG_DIR (or the environment) it covers what the config names (the directory and
	//! the user), not the contents of the files there (an edited core-site.xml keeps its identity) nor the environment
	//! variables; for a snapshot, its fingerprint.
	string Identity() const;

	//! A digest of a snapshot: SHA-256, as 64 lowercase hex digits, over a versioned, length-prefixed encoding of
	//! `user` and the properties in bytewise key order. It is stored in the secret, so it must never change for the
	//! same input.
	static string Fingerprint(const map<string, string> &properties, const string &user);
};

//! An open file. Reads may run concurrently.
class HdfsReader {
public:
	virtual ~HdfsReader() = default;

	virtual int64_t Length() const = 0;
	//! The modification time (epoch milliseconds) of the file version this reader reads.
	virtual uint64_t ModificationTime() const = 0;
	//! The NameNode inode id of that version (0 if unknown).
	virtual int64_t FileId() const = 0;
	//! Read exactly `nr_bytes` at `offset`; files in an encryption zone are decrypted.
	virtual void Read(data_ptr_t buffer, idx_t nr_bytes, idx_t offset) = 0;
};

//! A file being written (HDFS files are append-only). The writer created the file, so it is the writer's to
//! remove: a writer destroyed without Close() is aborted.
class HdfsWriter {
public:
	virtual ~HdfsWriter() = default;

	//! Append `nr_bytes`; files in an encryption zone are encrypted.
	virtual void Write(const_data_ptr_t buffer, idx_t nr_bytes) = 0;
	//! Send everything written so far to the DataNodes and have them sync it to disk (hsync). The NameNode keeps
	//! the file under construction: readers may not see the new length until Close().
	virtual void Sync() = 0;
	//! Flush and complete the file; the data is durable once this returns. If it throws, the file may be
	//! incomplete; Abort() still removes it.
	virtual void Close() = 0;
	//! Abandon the write without completing the file: delete it on the NameNode, then release the stream. Never
	//! waits for the file to complete. Throws if the delete fails: the file then stays under construction while
	//! this client keeps renewing its lease, and once the client disconnects and the lease expires, HDFS recovers
	//! it under its name with what was written.
	virtual void Abort() = 0;
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
	//! Log the NameNode RPCs this thread makes from now on (HdfsLogType, one entry per call) to `logger`, or stop
	//! logging them (nullptr). Only a weak reference is kept: once the logger is gone (its query has ended), nothing
	//! is logged. Walks log to the logger that was current when they started, from all their threads. Calls on an
	//! open file use HdfsRpcLoggerScope with the logger of the query that opened it.
	static void SetThreadLogger(const shared_ptr<Logger> &logger);
	//! This thread's current RPC logger (null if none, or if it is gone).
	static shared_ptr<Logger> GetThreadLogger();

	virtual HdfsFileStatus Stat(const string &path) = 0;
	//! Open a file for reading. `expected` is the file's status from a listing (length, mtime and, if known, file
	//! id), or null: the file is then stat'ed first. A file opened again while a reader on it is still open shares
	//! that reader only if the reader reads the same version (same file id, mtime and length); the reader's own
	//! version is what ModificationTime() and Length() report.
	virtual shared_ptr<HdfsReader> Open(const string &path, optional_ptr<const HdfsFileStatus> expected) = 0;
	//! Create a file (and missing parent directories) for writing. Without `overwrite`, an existing file is an
	//! ALREADY_EXISTS error (checked atomically by the NameNode).
	virtual unique_ptr<HdfsWriter> Create(const string &path, bool overwrite) = 0;
	//! Create a directory and its missing parents.
	virtual void CreateDirectories(const string &path) = 0;
	//! Create a directory whose parent exists (a missing parent is a NOT_FOUND error). An existing directory is
	//! not an error.
	virtual void CreateDirectory(const string &path) = 0;
	//! Delete a file or directory; returns false if it doesn't exist.
	virtual bool Delete(const string &path, bool recursive) = 0;
	//! Atomically rename `source` to `target`; with `overwrite`, an existing file (or empty directory) is replaced.
	virtual void Rename(const string &source, const string &target, bool overwrite) = 0;
	//! List `path`'s children (`recursive`: the whole subtree); a file lists as itself.
	virtual unique_ptr<HdfsWalk> List(const string &path, bool recursive, const HdfsWalkOptions &options) = 0;
	//! The entries (files and directories) a parsed glob matches (`*`, `?`, `[...]`, `\` escapes, `{a,b}` and `**`;
	//! HdfsGlobPlan::Parse, which also decides hidden names). `plan` must not be null. The caller parses once, before
	//! connecting; the walk and its retries share the plan.
	virtual unique_ptr<HdfsWalk> Glob(shared_ptr<const HdfsGlobPlan> plan, const HdfsWalkOptions &options) = 0;
};

//! While alive, sends this thread's NameNode RPCs to `logger` (HdfsClient::SetThreadLogger), then restores the
//! previous logger. For calls on an open file (abort, close, stat), which can run on any thread, after the thread
//! last served another query or connection.
class HdfsRpcLoggerScope {
public:
	explicit HdfsRpcLoggerScope(const shared_ptr<Logger> &logger) : previous(HdfsClient::GetThreadLogger()) {
		HdfsClient::SetThreadLogger(logger);
	}
	~HdfsRpcLoggerScope() {
		HdfsClient::SetThreadLogger(previous);
	}
	HdfsRpcLoggerScope(const HdfsRpcLoggerScope &) = delete;
	HdfsRpcLoggerScope &operator=(const HdfsRpcLoggerScope &) = delete;

private:
	shared_ptr<Logger> previous;
};

} // namespace duckdb
