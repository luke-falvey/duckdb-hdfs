// HDFS client layer behind the C interface in hdfs_bridge.h, built on the
// native C++ libhdfs3 client. This is the only translation unit that includes
// libhdfs3 headers, so its macros (e.g. LOG) never meet DuckDB's.

// Must come before any libhdfs3/protobuf header: drops Abseil's LOG macro,
// which clashes with libhdfs3's.
#include "absl_log_undef.h"

#include "client/FileStatus.h"
#include "client/FileSystem.h"
#include "client/InputStream.h"
#include "client/OutputStream.h"
#include "client/Permission.h"
#include "common/Exception.h"
#include "common/Logger.h"
#include "common/SessionConfig.h"
#include "common/XmlConfig.h"
#include "server/LocatedBlocks.h"

#include "hdfs_bridge.h"
#include "hdfs_crypto.hpp"
#include "hdfs_glob.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using std::string;

struct hdfs_client_t {
	std::shared_ptr<Hdfs::FileSystem> fs;
	int replication = 0;
	int64_t block_size = 0;
	// Bytes of block locations fetched when opening a file (as libhdfs3's own
	// open does: dfs.prefetchsize blocks).
	int64_t prefetch_size = 0;
	// For encryption zones: the user sent to the KMS, and the KMS provider URI
	// from hadoop.security.key.provider.path / dfs.encryption.key.provider.uri
	// (empty if not configured).
	std::string user;
	std::string key_provider;
	// Kerberos cluster: the KMS is called with SPNEGO instead of user.name.
	bool kerberos = false;
	// Files open for reading, by path. DuckDB often opens a file again while a
	// handle on it is still open (Parquet: one handle for the metadata, one
	// for the scan); the second open shares the reader instead of asking the
	// NameNode again.
	std::mutex readers_mu;
	std::unordered_map<std::string, hdfs_reader_t *> open_readers;
};

namespace {

// --- errors ---------------------------------------------------------------

// A classified failure, carried from where it happens to a `status`.
struct Failure {
	int32_t code = HDFS_ERR_IO;
	string message;
};

// The first line of an exception message: libhdfs3 appends the server-side
// Java stack trace to RPC errors.
string FirstLine(const char *what) {
	string msg = what ? what : "";
	auto eol = msg.find('\n');
	if (eol != string::npos) {
		msg.erase(eol);
	}
	while (!msg.empty() && (msg.back() == ' ' || msg.back() == '\t' || msg.back() == '\r')) {
		msg.pop_back();
	}
	return msg;
}

// Classify a Hadoop server-side exception by its Java class name.
int32_t ClassifyServerClass(const string &cls) {
	if (cls.find("AccessControlException") != string::npos || cls.find("SecurityException") != string::npos) {
		return HDFS_ERR_PERMISSION;
	}
	if (cls.find("FileNotFoundException") != string::npos) {
		return HDFS_ERR_NOT_FOUND;
	}
	if (cls.find("FileAlreadyExistsException") != string::npos || cls.find("AlreadyBeingCreated") != string::npos) {
		return HDFS_ERR_ALREADY_EXISTS;
	}
	if (cls.find("StandbyException") != string::npos || cls.find("RetriableException") != string::npos) {
		return HDFS_ERR_CONNECTION;
	}
	return HDFS_ERR_IO;
}

// Classify one exception (not its nested causes).
int32_t ClassifyOne(const std::exception_ptr &ep) {
	try {
		std::rethrow_exception(ep);
	} catch (const hdfs_glob::GlobError &) {
		return HDFS_ERR_INVALID_ARGUMENT;
	} catch (const hdfs_crypto::CryptoError &) {
		return HDFS_ERR_IO;
	} catch (const Hdfs::AccessControlException &) {
		return HDFS_ERR_PERMISSION;
	} catch (const Hdfs::FileNotFoundException &) {
		return HDFS_ERR_NOT_FOUND;
	} catch (const Hdfs::FileAlreadyExistsException &) {
		return HDFS_ERR_ALREADY_EXISTS;
	} catch (const Hdfs::AlreadyBeingCreatedException &) {
		return HDFS_ERR_ALREADY_EXISTS;
	} catch (const Hdfs::InvalidPath &) {
		return HDFS_ERR_INVALID_ARGUMENT;
	} catch (const Hdfs::InvalidParameter &) {
		return HDFS_ERR_INVALID_ARGUMENT;
	} catch (const Hdfs::HdfsRpcServerException &e) {
		return ClassifyServerClass(e.getErrClass());
	} catch (const Hdfs::HdfsEndOfStream &) {
		return HDFS_ERR_IO;
	} catch (const Hdfs::HdfsNetworkException &) {
		return HDFS_ERR_CONNECTION;
	} catch (const Hdfs::HdfsRpcException &) {
		return HDFS_ERR_CONNECTION;
	} catch (const Hdfs::HdfsTimeoutException &) {
		return HDFS_ERR_CONNECTION;
	} catch (const Hdfs::HdfsFailoverException &) {
		return HDFS_ERR_CONNECTION;
	} catch (const Hdfs::NameNodeStandbyException &) {
		return HDFS_ERR_CONNECTION;
	} catch (const Hdfs::SaslException &) {
		return HDFS_ERR_CONNECTION;
	} catch (const Hdfs::HdfsFileSystemClosed &) {
		return HDFS_ERR_CONNECTION;
	} catch (...) {
		return HDFS_ERR_IO;
	}
}

// Classify an exception and build its message, walking nested causes:
// libhdfs3 often wraps the specific failure in a generic HdfsIOException.
Failure Describe(const std::exception_ptr &ep) {
	Failure failure;
	bool classified = false;
	std::exception_ptr current = ep;
	for (int depth = 0; current && depth < 8; depth++) {
		int32_t code = ClassifyOne(current);
		if (!classified || failure.code == HDFS_ERR_IO) {
			failure.code = code;
			classified = true;
		}
		std::exception_ptr next;
		try {
			std::rethrow_exception(current);
		} catch (const std::exception &e) {
			auto line = FirstLine(e.what());
			if (!line.empty() && failure.message.find(line) == string::npos) {
				failure.message += failure.message.empty() ? line : ": " + line;
			}
			try {
				std::rethrow_if_nested(e);
			} catch (...) {
				next = std::current_exception();
			}
		} catch (...) {
			if (failure.message.empty()) {
				failure.message = "unknown error";
			}
		}
		current = next;
	}
	if (failure.message.empty()) {
		failure.message = "unknown error";
	}
	return failure;
}

void SetStatus(hdfs_status_t *status, int32_t code, const string &message) {
	if (!status) {
		return;
	}
	status->code = code;
	status->msg = strdup(message.c_str());
}

void SetError(hdfs_status_t *status, const string &context, const Failure &failure) {
	SetStatus(status, failure.code, context + ": " + failure.message);
}

// Run `fn`; on an exception, classify it into `status` and return false.
template <class FN>
bool Guard(hdfs_status_t *status, const string &context, FN &&fn) {
	try {
		fn();
		return true;
	} catch (...) {
		SetError(status, context, Describe(std::current_exception()));
		return false;
	}
}

// --- configuration ----------------------------------------------------------

bool FileExists(const string &path) {
	struct stat st;
	return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

string GetEnv(const char *name) {
	const char *value = std::getenv(name);
	return value ? string(value) : string();
}

// The Hadoop client config directory, resolved like the Hadoop CLI tools.
string ConfigDir(const char *explicit_dir) {
	if (explicit_dir && *explicit_dir) {
		return explicit_dir;
	}
	auto dir = GetEnv("HADOOP_CONF_DIR");
	if (!dir.empty()) {
		return dir;
	}
	auto home = GetEnv("HADOOP_HOME");
	if (!home.empty()) {
		return home + "/etc/hadoop";
	}
	return string();
}

// Set `key` only if the user's config doesn't.
void SetDefault(Hdfs::Config &conf, const char *key, const string &value) {
	if (!conf.getString(key, nullptr)) {
		conf.set(key, value);
	}
}

// Build the libhdfs3 config from core-site.xml / hdfs-site.xml.
Hdfs::Config LoadConfig(const char *explicit_dir) {
	Hdfs::Config conf;
	auto dir = ConfigDir(explicit_dir);
	if (!dir.empty()) {
		for (const char *file : {"core-site.xml", "hdfs-site.xml"}) {
			auto path = dir + "/" + file;
			if (FileExists(path)) {
				conf.update(path.c_str());
			}
		}
	}
	// libhdfs3 has its own setting names and much longer defaults than the
	// Hadoop Java client (e.g. 10 connect retries, hour-long timeouts). Use the
	// Hadoop setting when the cluster config has one, else a Hadoop-like default.
	struct Mapping {
		const char *libhdfs3_key;
		const char *hadoop_key;
		const char *fallback;
	};
	static const Mapping mappings[] = {
	    {"rpc.client.connect.timeout", "ipc.client.connect.timeout", "20000"},
	    {"rpc.client.connect.retry", "ipc.client.connect.max.retries", "1"},
	    {"rpc.client.timeout", "ipc.client.rpc-timeout.ms", "120000"},
	    {"rpc.client.read.timeout", "dfs.client.socket-timeout", "60000"},
	    {"input.connect.timeout", "ipc.client.connect.timeout", "20000"},
	    {"input.read.timeout", "dfs.client.socket-timeout", "60000"},
	    {"input.write.timeout", "dfs.client.socket-timeout", "60000"},
	    {"output.connect.timeout", "ipc.client.connect.timeout", "20000"},
	    {"output.read.timeout", "dfs.client.socket-timeout", "60000"},
	    {"output.write.timeout", "dfs.datanode.socket.write.timeout", "480000"},
	};
	for (auto &m : mappings) {
		const char *hadoop_value = conf.getString(m.hadoop_key, nullptr);
		// Hadoop's "0" means "no timeout"; keep libhdfs3's own default then.
		if (hadoop_value && string(hadoop_value) != "0") {
			SetDefault(conf, m.libhdfs3_key, hadoop_value);
		} else {
			SetDefault(conf, m.libhdfs3_key, m.fallback);
		}
	}
	SetDefault(conf, "dfs.client.read.shortcircuit", "false");
	// Idle DataNode connections kept for reuse (in all, across DataNodes).
	// Hadoop's default of 16 is too few for DuckDB, which reads many files on
	// many threads at once: the extra connections are closed after each read and
	// a new TCP (and, on secure clusters, SASL) handshake follows.
	SetDefault(conf, "dfs.client.socketcache.capacity", "64");
	SetDefault(conf, "dfs.client.log.severity", "FATAL");
	return conf;
}

// --- paths and entries --------------------------------------------------------

// The path of a listed child. libhdfs3 builds it as "<dir>/<name>", which
// doubles the slash for the root and ends in "/" when the listed path is a
// file (whose "child" is the file itself, with an empty name).
string ChildPath(const string &dir, const string &listed) {
	string name = listed.size() > dir.size() + 1 ? listed.substr(dir.size() + 1) : string();
	if (name.empty()) {
		return dir;
	}
	return dir == "/" ? "/" + name : dir + "/" + name;
}

string BaseName(const string &path) {
	auto slash = path.find_last_of('/');
	return slash == string::npos ? path : path.substr(slash + 1);
}

// One listing/glob result, independent of libhdfs3 types.
struct Entry {
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

Entry ToEntry(const Hdfs::FileStatus &st, const string &path) {
	Entry e;
	e.path = path;
	e.is_dir = st.isDirectory();
	e.length = st.getLength();
	e.mtime = static_cast<uint64_t>(st.getModificationTime());
	e.atime = static_cast<uint64_t>(st.getAccessTime());
	e.owner = st.getOwner();
	e.group = st.getGroup();
	e.permission = st.getPermission().toShort();
	// Replication and block size apply only to files; -1 means NULL in SQL.
	if (!e.is_dir) {
		e.replication = st.getReplication();
		e.block_size = st.getBlockSize();
	}
	return e;
}

void FillDirEntry(hdfs_dir_entry_t &out, const Entry &e) {
	out.path = strdup(e.path.c_str());
	out.is_dir = e.is_dir;
	out.length = e.length;
	out.mtime = e.mtime;
	out.atime = e.atime;
	out.owner = strdup(e.owner.c_str());
	out.group = strdup(e.group.c_str());
	out.permission = e.permission;
	out.replication = e.replication;
	out.block_size = e.block_size;
}

// List `dir`, calling `fn(child_path, status)` for each child. Listing a file
// yields the file itself once, as in the Hadoop client. (libhdfs3's iterator
// would return it forever.)
template <class FN>
void ListDirectory(Hdfs::FileSystem &fs, const string &dir, FN &&fn) {
	auto it = fs.listDirectory(dir.c_str());
	while (it.hasNext()) {
		auto st = it.getNext();
		auto path = ChildPath(dir, st.getPath());
		bool is_self = path == dir;
		fn(path, st);
		if (is_self) {
			return;
		}
	}
}

} // namespace

// --- streaming listing ----------------------------------------------------------

// A directory walk run by up to `max_parallelism` worker threads. Workers take
// directories from a LIFO stack (depth-first-ish, which keeps the stack small
// and the parent-to-child listing gap short), list them, and queue the entries
// for the consumer. The queue is bounded: a full queue pauses the walk until
// Next() drains it.
struct hdfs_list_stream_t {
	struct Task {
		string path;
		std::vector<hdfs_glob::Pos> states;
		bool is_root = false;
		// Stat the path instead of listing it (an all-literal glob).
		bool stat_only = false;
	};

	static constexpr size_t BUFFER = 8192;

	std::shared_ptr<Hdfs::FileSystem> fs;
	string display_path;
	bool glob = false;
	bool recursive = false;
	bool skip_permission_errors = false;
	bool include_hidden = false;
	size_t max_parallelism = 1;
	std::unique_ptr<hdfs_glob::GlobPlan> plan;

	std::mutex mu;
	std::condition_variable cv; // any state change
	std::vector<Task> pending;
	size_t in_flight = 0;
	size_t idle_workers = 0;
	std::deque<Entry> items;
	bool has_error = false;
	Failure error;
	bool cancelled = false;
	std::vector<std::thread> workers;

	~hdfs_list_stream_t() {
		{
			std::lock_guard<std::mutex> lock(mu);
			cancelled = true;
		}
		cv.notify_all();
		for (auto &t : workers) {
			t.join();
		}
	}

	bool Finished() const {
		return has_error || cancelled || (pending.empty() && in_flight == 0);
	}

	// Start with `first`. Caller doesn't hold `mu`.
	void Start(Task first) {
		std::lock_guard<std::mutex> lock(mu);
		pending.push_back(std::move(first));
		SpawnIfNeeded();
	}

	// Add a worker if there is queued work no idle worker can take. Caller holds `mu`.
	void SpawnIfNeeded() {
		if (idle_workers < pending.size() && workers.size() < max_parallelism) {
			workers.emplace_back([this] { Work(); });
		}
	}

	// Queue an entry, waiting while the buffer is full. Returns false once the
	// walk is stopping. Caller holds `lock`.
	bool Emit(std::unique_lock<std::mutex> &lock, Entry entry) {
		cv.wait(lock, [&] { return items.size() < BUFFER || cancelled || has_error; });
		if (cancelled || has_error) {
			return false;
		}
		items.push_back(std::move(entry));
		cv.notify_all();
		return true;
	}

	void Fail(std::unique_lock<std::mutex> &, Failure failure) {
		if (!has_error) {
			has_error = true;
			error = std::move(failure);
		}
		cv.notify_all();
	}

	void Work() {
		std::unique_lock<std::mutex> lock(mu);
		for (;;) {
			idle_workers++;
			cv.wait(lock, [&] { return !pending.empty() || Finished(); });
			idle_workers--;
			if (cancelled || has_error || pending.empty()) {
				cv.notify_all(); // let the consumer see completion
				return;
			}
			Task task = std::move(pending.back());
			pending.pop_back();
			in_flight++;
			lock.unlock();
			Run(task);
			lock.lock();
			in_flight--;
			cv.notify_all();
		}
	}

	// Whether a failed listing below the root is pruned rather than fatal.
	bool Prunable(const Task &task, const Failure &failure) const {
		if (glob && failure.code == HDFS_ERR_NOT_FOUND) {
			return true; // a glob matching nothing (or a raced delete) is no error
		}
		if (task.is_root) {
			return false;
		}
		return failure.code == HDFS_ERR_NOT_FOUND || (skip_permission_errors && failure.code == HDFS_ERR_PERMISSION);
	}

	// Run one task without holding `mu` during RPCs.
	void Run(const Task &task) {
		std::vector<std::pair<string, Hdfs::FileStatus>> children;
		try {
			if (task.stat_only) {
				children.emplace_back(task.path, fs->getFileStatus(task.path.c_str()));
			} else {
				ListDirectory(*fs, task.path,
				              [&](const string &path, const Hdfs::FileStatus &st) { children.emplace_back(path, st); });
			}
		} catch (...) {
			auto failure = Describe(std::current_exception());
			std::unique_lock<std::mutex> lock(mu);
			if (!Prunable(task, failure)) {
				Fail(lock, std::move(failure));
			}
			return;
		}

		std::unique_lock<std::mutex> lock(mu);
		if (task.stat_only) {
			Emit(lock, ToEntry(children[0].second, children[0].first));
			return;
		}
		for (auto &child : children) {
			const string &path = child.first;
			const auto &st = child.second;
			bool is_dir = st.isDirectory();
			if (glob) {
				if (path == task.path) {
					continue; // a file lists as itself; it has no children to match
				}
				auto step = plan->Match(task.states, BaseName(path), is_dir);
				if (is_dir && !step.next.empty()) {
					pending.push_back(Task {path, std::move(step.next), false, false});
					SpawnIfNeeded();
					cv.notify_all();
				}
				if (step.emit && !Emit(lock, ToEntry(st, path))) {
					return;
				}
			} else {
				// The listed path itself is exempt from hidden filtering.
				if (!include_hidden && path != task.path && hdfs_glob::IsHidden(BaseName(path))) {
					continue;
				}
				if (recursive && is_dir && path != task.path) {
					pending.push_back(Task {path, {}, false, false});
					SpawnIfNeeded();
					cv.notify_all();
				}
				if (!Emit(lock, ToEntry(st, path))) {
					return;
				}
			}
		}
	}

	// Hand out up to `max_entries` entries, blocking until some are available.
	// Returns an empty vector at the end of the walk; throws nothing — errors
	// come back through `failure`.
	bool Next(size_t max_entries, std::vector<Entry> &out, Failure &failure) {
		std::unique_lock<std::mutex> lock(mu);
		cv.wait(lock, [&] { return !items.empty() || Finished(); });
		if (!items.empty()) {
			while (!items.empty() && out.size() < max_entries) {
				out.push_back(std::move(items.front()));
				items.pop_front();
			}
			cv.notify_all(); // room for paused workers
			return true;
		}
		if (has_error) {
			failure = error;
			return false;
		}
		return true; // walk complete
	}
};

// --- readers and writers -----------------------------------------------------------

// The data encryption key of a file in an encryption zone, decrypted by the KMS.
static string FileKey(const hdfs_client_t &client, const Hdfs::FileEncryptionInfo &info, const string &path) {
	if (info.suite != hdfs_crypto::SUITE_AES_CTR_NOPADDING) {
		throw hdfs_crypto::CryptoError(path + " uses an unsupported encryption cipher suite (" +
		                               std::to_string(info.suite) + "); only AES/CTR/NoPadding is supported");
	}
	if (info.protocolVersion != hdfs_crypto::PROTOCOL_ENCRYPTION_ZONES) {
		throw hdfs_crypto::CryptoError(path + " uses an unsupported encryption protocol version (" +
		                               std::to_string(info.protocolVersion) + ")");
	}
	if (client.key_provider.empty()) {
		throw hdfs_crypto::CryptoError(path + " is in an HDFS encryption zone, but no key provider is configured "
		                                      "(set hadoop.security.key.provider.path in core-site.xml)");
	}
	return hdfs_crypto::DecryptEncryptedKey(hdfs_crypto::ParseKmsUri(client.key_provider), client.kerberos, client.user,
	                                        info.keyName, info.ezKeyVersionName, info.iv, info.key);
}

// An open file. libhdfs3 input streams aren't safe for concurrent use, so the
// reader keeps a pool: each concurrent read borrows its own stream.
struct hdfs_reader_t {
	hdfs_client_t *client = nullptr;
	int refs = 1; // open handles sharing this reader; guarded by client->readers_mu
	std::shared_ptr<Hdfs::FileSystem> fs;
	string path;
	int64_t length = 0;
	// Files in an encryption zone: the decrypted data key and the file IV.
	bool encrypted = false;
	string key;
	string iv;
	// Block locations fetched at open, shared out (as copies) to every pooled
	// stream so opening another stream costs no NameNode RPC.
	std::shared_ptr<Hdfs::Internal::LocatedBlocksImpl> blocks;
	std::mutex mu;
	std::vector<std::unique_ptr<Hdfs::InputStream>> idle;

	std::unique_ptr<Hdfs::InputStream> NewStream() {
		// Each stream extends its block list lazily, so it gets its own copy
		// (including the last block, which libhdfs3 updates in place).
		auto copy = std::make_shared<Hdfs::Internal::LocatedBlocksImpl>(*blocks);
		if (auto last = blocks->getLastBlock()) {
			copy->setLastBlock(std::make_shared<Hdfs::Internal::LocatedBlock>(*last));
		}
		auto stream = std::make_unique<Hdfs::InputStream>(copy);
		stream->open(*fs, path.c_str(), true);
		return stream;
	}

	std::unique_ptr<Hdfs::InputStream> Borrow() {
		{
			std::lock_guard<std::mutex> lock(mu);
			if (!idle.empty()) {
				auto stream = std::move(idle.back());
				idle.pop_back();
				return stream;
			}
		}
		return NewStream();
	}

	void Return(std::unique_ptr<Hdfs::InputStream> stream) {
		std::lock_guard<std::mutex> lock(mu);
		idle.push_back(std::move(stream));
	}
};

struct hdfs_writer_t {
	std::shared_ptr<Hdfs::FileSystem> fs;
	string path;
	std::unique_ptr<Hdfs::OutputStream> out;
	// Files in an encryption zone: the data key, the file IV, and how many
	// bytes have been written (the AES-CTR position).
	bool encrypted = false;
	string key;
	string iv;
	uint64_t position = 0;
	std::vector<uint8_t> scratch;
};

// --- C interface ---------------------------------------------------------------------

extern "C" {

void hdfs_bridge_free_string(char *s) {
	free(s);
}

hdfs_client_t *hdfs_bridge_connect(const char *url, const char *config_dir, const char *user, hdfs_status_t *status) {
	std::unique_ptr<hdfs_client_t> client;
	bool ok = Guard(status, "failed to connect to HDFS", [&] {
		Hdfs::Internal::RootLogger.setLogSeverity(Hdfs::Internal::FATAL);
		auto conf = LoadConfig(config_dir);
		string auth = conf.getString("hadoop.security.authentication", "simple");
		std::transform(auth.begin(), auth.end(), auth.begin(), ::tolower);
		if (auth != "simple" && auth != "kerberos") {
			throw Hdfs::InvalidParameter("hadoop.security.authentication=" + auth +
			                                 " is not supported (use simple or kerberos)",
			                             __FILE__, __LINE__, "");
		}
		bool kerberos = auth == "kerberos";
		string uri = url && *url ? string(url) : conf.getString("fs.defaultFS", "");
		if (uri.empty() || uri.rfind("hdfs://", 0) != 0) {
			throw Hdfs::InvalidParameter("no NameNode given: use hdfs://host:port/... or set fs.defaultFS "
			                             "to an hdfs:// URI in core-site.xml",
			                             __FILE__, __LINE__, "");
		}
		string username;
		if (kerberos) {
			// The identity is the principal in the Kerberos ticket cache
			// (KRB5CCNAME, or hadoop.security.kerberos.ticket.cache.path), as in
			// Hadoop: HADOOP_USER_NAME doesn't apply.
			if (!GetEnv("HADOOP_PROXY_USER").empty()) {
				throw Hdfs::InvalidParameter("HADOOP_PROXY_USER (impersonation) is not supported on Kerberos clusters",
				                             __FILE__, __LINE__, "");
			}
		} else {
			username = user && *user ? string(user) : GetEnv("HADOOP_USER_NAME");
			if (username.empty()) {
				username = GetEnv("HADOOP_PROXY_USER");
			}
		}
		client = std::make_unique<hdfs_client_t>();
		client->fs = std::make_shared<Hdfs::FileSystem>(conf);
		client->fs->connect(uri.c_str(), username.empty() ? nullptr : username.c_str(), nullptr);
		Hdfs::Internal::SessionConfig session(conf);
		client->replication = session.getDefaultReplica();
		client->block_size = session.getDefaultBlockSize();
		client->prefetch_size = session.getDefaultBlockSize() * session.getPrefetchSize();
		client->user = username;
		client->kerberos = kerberos;
		client->key_provider = conf.getString("hadoop.security.key.provider.path", "");
		if (client->key_provider.empty()) {
			client->key_provider = conf.getString("dfs.encryption.key.provider.uri", "");
		}
	});
	return ok ? client.release() : nullptr;
}

void hdfs_bridge_free_client(hdfs_client_t *client) {
	if (!client) {
		return;
	}
	try {
		client->fs->disconnect();
	} catch (...) { // NOLINT: best effort on teardown
	}
	delete client;
}

int32_t hdfs_bridge_get_file_info(hdfs_client_t *client, const char *path, hdfs_file_info_t *out,
                                  hdfs_status_t *status) {
	bool ok = Guard(status, string("stat '") + path + "' failed", [&] {
		auto st = client->fs->getFileStatus(path);
		out->length = st.getLength();
		out->is_dir = st.isDirectory();
		out->mtime = static_cast<uint64_t>(st.getModificationTime());
	});
	return ok ? 0 : -1;
}

hdfs_reader_t *hdfs_bridge_open(hdfs_client_t *client, const char *path, hdfs_status_t *status) {
	{
		std::lock_guard<std::mutex> lock(client->readers_mu);
		auto it = client->open_readers.find(path);
		if (it != client->open_readers.end()) {
			it->second->refs++;
			return it->second;
		}
	}
	std::unique_ptr<hdfs_reader_t> reader;
	bool ok = Guard(status, string("open '") + path + "' for reading failed", [&] {
		// One RPC: the block locations carry the length and any encryption info.
		// (A directory fails here with "Path is not a file".)
		auto blocks = std::make_shared<Hdfs::Internal::LocatedBlocksImpl>();
		client->fs->getBlockLocations(path, 0, client->prefetch_size, *blocks);
		reader = std::make_unique<hdfs_reader_t>();
		reader->client = client;
		reader->fs = client->fs;
		reader->path = path;
		reader->blocks = blocks;
		reader->length = blocks->getFileLength();
		if (blocks->isEncrypted()) {
			reader->encrypted = true;
			reader->key = FileKey(*client, blocks->getEncryptionInfo(), path);
			reader->iv = blocks->getEncryptionInfo().iv;
		}
		// Open one stream up front so open errors surface here, not on first read.
		reader->Return(reader->NewStream());
	});
	if (!ok) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(client->readers_mu);
	auto inserted = client->open_readers.emplace(path, reader.get());
	if (!inserted.second) {
		// Another thread opened the same file meanwhile: share its reader.
		// (Ours is dropped; its stream closes in the destructor.)
		inserted.first->second->refs++;
		return inserted.first->second;
	}
	return reader.release();
}

void hdfs_bridge_close_reader(hdfs_reader_t *reader) {
	if (!reader) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(reader->client->readers_mu);
		if (--reader->refs > 0) {
			return;
		}
		reader->client->open_readers.erase(reader->path);
	}
	for (auto &stream : reader->idle) {
		try {
			stream->close();
		} catch (...) { // NOLINT: best effort on teardown
		}
	}
	delete reader;
}

int64_t hdfs_bridge_file_size(hdfs_reader_t *reader) {
	return reader->length;
}

int64_t hdfs_bridge_read_range(hdfs_reader_t *reader, uint8_t *buf, int64_t len, uint64_t offset,
                               hdfs_status_t *status) {
	if (len < 0) {
		SetStatus(status, HDFS_ERR_INVALID_ARGUMENT, "negative read length");
		return -1;
	}
	std::unique_ptr<Hdfs::InputStream> stream;
	bool ok = Guard(status, "read at offset " + std::to_string(offset) + " failed", [&] {
		stream = reader->Borrow();
		int64_t done = 0;
		while (done < len) {
			auto chunk = static_cast<int32_t>(std::min<int64_t>(len - done, 64 << 20));
			int32_t n = stream->pread(reinterpret_cast<char *>(buf) + done, chunk, static_cast<int64_t>(offset) + done);
			if (n <= 0) {
				throw Hdfs::HdfsEndOfStream("unexpected end of file", __FILE__, __LINE__, "");
			}
			done += n;
		}
		if (reader->encrypted) {
			hdfs_crypto::AesCtrTransform(reader->key, reader->iv, offset, buf, static_cast<size_t>(len));
		}
	});
	if (ok) {
		reader->Return(std::move(stream));
		return len;
	}
	// A stream that failed mid-read is dropped rather than reused.
	return -1;
}

hdfs_writer_t *hdfs_bridge_create(hdfs_client_t *client, const char *path, bool overwrite, hdfs_status_t *status) {
	std::unique_ptr<hdfs_writer_t> writer;
	bool ok = Guard(status, string("create '") + path + "' for writing failed", [&] {
		int flags = Hdfs::Create | (overwrite ? Hdfs::Overwrite : 0);
		Hdfs::Permission permission(0644);
		// Missing parent directories are created, as in the Hadoop client.
		auto created = client->fs->createOrAppend(path, flags, permission, /*createParent=*/true, client->replication,
		                                          client->block_size);
		auto &file = *created.second;
		writer = std::make_unique<hdfs_writer_t>();
		writer->fs = client->fs;
		writer->path = path;
		if (file.isEncrypted()) {
			try {
				writer->key = FileKey(*client, file.getEncryptionInfo(), path);
			} catch (...) {
				// Don't leave an empty file behind that we can't write.
				try {
					client->fs->deletePath(path, false);
				} catch (...) { // NOLINT: the original error matters more
				}
				throw;
			}
			writer->encrypted = true;
			writer->iv = file.getEncryptionInfo().iv;
		}
		auto ec = file.getEcPolicy();
		writer->out = ec ? std::make_unique<Hdfs::OutputStream>(ec) : std::make_unique<Hdfs::OutputStream>();
		writer->out->open(*client->fs, path, created, flags, permission, true, client->replication, client->block_size,
		                  file.getFileId());
	});
	return ok ? writer.release() : nullptr;
}

int64_t hdfs_bridge_write(hdfs_writer_t *writer, const uint8_t *buf, int64_t len, hdfs_status_t *status) {
	if (len < 0) {
		SetStatus(status, HDFS_ERR_INVALID_ARGUMENT, "negative write length");
		return -1;
	}
	bool ok = Guard(status, "write failed", [&] {
		if (!writer->encrypted) {
			writer->out->append(reinterpret_cast<const char *>(buf), len);
			return;
		}
		writer->scratch.assign(buf, buf + len);
		hdfs_crypto::AesCtrTransform(writer->key, writer->iv, writer->position, writer->scratch.data(),
		                             writer->scratch.size());
		writer->out->append(reinterpret_cast<const char *>(writer->scratch.data()), len);
		writer->position += static_cast<uint64_t>(len);
	});
	return ok ? len : -1;
}

int32_t hdfs_bridge_close_writer(hdfs_writer_t *writer, hdfs_status_t *status) {
	if (!writer) {
		return 0;
	}
	std::unique_ptr<hdfs_writer_t> owned(writer);
	bool ok = Guard(status, "close writer failed", [&] { owned->out->close(); });
	return ok ? 0 : -1;
}

hdfs_list_stream_t *hdfs_bridge_list_stream_open(hdfs_client_t *client, const char *path, bool recursive,
                                                 int32_t max_parallelism, bool skip_permission_errors,
                                                 bool include_hidden) {
	auto stream = std::make_unique<hdfs_list_stream_t>();
	stream->fs = client->fs;
	stream->display_path = path;
	stream->recursive = recursive;
	stream->max_parallelism = static_cast<size_t>(std::max(max_parallelism, 1));
	stream->skip_permission_errors = skip_permission_errors;
	stream->include_hidden = include_hidden;
	stream->Start({path, {}, true, false});
	return stream.release();
}

hdfs_list_stream_t *hdfs_bridge_glob_stream_open(hdfs_client_t *client, const char *pattern, int32_t max_parallelism,
                                                 bool skip_permission_errors, bool include_hidden,
                                                 hdfs_status_t *status) {
	std::unique_ptr<hdfs_glob::GlobPlan> plan;
	if (!Guard(status, "invalid glob pattern", [&] {
		    plan = std::make_unique<hdfs_glob::GlobPlan>(hdfs_glob::GlobPlan::Parse(pattern, include_hidden));
	    })) {
		return nullptr;
	}
	auto stream = std::make_unique<hdfs_list_stream_t>();
	stream->fs = client->fs;
	stream->display_path = pattern;
	stream->glob = true;
	stream->max_parallelism = static_cast<size_t>(std::max(max_parallelism, 1));
	stream->skip_permission_errors = skip_permission_errors;
	stream->include_hidden = include_hidden;
	auto root = plan->Root();
	bool emit_root = plan->EmitRoot();
	auto initial = plan->Initial();
	stream->plan = std::move(plan);
	{
		std::lock_guard<std::mutex> lock(stream->mu);
		// An all-literal pattern (or brace alternative) matches the root itself.
		if (emit_root) {
			stream->pending.push_back({root, {}, true, true});
		}
		if (!initial.empty()) {
			stream->pending.push_back({root, initial, true, false});
		}
		stream->SpawnIfNeeded();
	}
	stream->cv.notify_all();
	return stream.release();
}

hdfs_dir_entry_t *hdfs_bridge_list_stream_next(hdfs_list_stream_t *stream, int32_t max_entries, int32_t *out_count,
                                               hdfs_status_t *status) {
	*out_count = 0;
	std::vector<Entry> batch;
	Failure failure;
	if (!stream->Next(static_cast<size_t>(std::max(max_entries, 1)), batch, failure)) {
		SetError(status, "list '" + stream->display_path + "' failed", failure);
		return nullptr;
	}
	if (batch.empty()) {
		return nullptr;
	}
	auto *entries = static_cast<hdfs_dir_entry_t *>(calloc(batch.size(), sizeof(hdfs_dir_entry_t)));
	for (size_t i = 0; i < batch.size(); i++) {
		FillDirEntry(entries[i], batch[i]);
	}
	*out_count = static_cast<int32_t>(batch.size());
	return entries;
}

void hdfs_bridge_list_stream_free(hdfs_list_stream_t *stream) {
	delete stream;
}

void hdfs_bridge_free_dir_entries(hdfs_dir_entry_t *entries, int32_t count) {
	if (!entries) {
		return;
	}
	for (int32_t i = 0; i < count; i++) {
		free(entries[i].path);
		free(entries[i].owner);
		free(entries[i].group);
	}
	free(entries);
}

hdfs_dir_entry_t *hdfs_bridge_stat(hdfs_client_t *client, const char *path, hdfs_status_t *status) {
	hdfs_dir_entry_t *entry = nullptr;
	Guard(status, string("stat '") + path + "' failed", [&] {
		auto st = client->fs->getFileStatus(path);
		entry = static_cast<hdfs_dir_entry_t *>(calloc(1, sizeof(hdfs_dir_entry_t)));
		FillDirEntry(*entry, ToEntry(st, path));
	});
	return entry;
}

int32_t hdfs_bridge_mkdirs(hdfs_client_t *client, const char *path, hdfs_status_t *status) {
	bool ok = Guard(status, string("mkdirs '") + path + "' failed",
	                [&] { client->fs->mkdirs(path, Hdfs::Permission(0755)); });
	return ok ? 0 : -1;
}

int32_t hdfs_bridge_delete(hdfs_client_t *client, const char *path, bool recursive, hdfs_status_t *status) {
	bool deleted = false;
	if (!Guard(status, string("delete '") + path + "' failed",
	           [&] { deleted = client->fs->deletePath(path, recursive); })) {
		return -1;
	}
	if (!deleted) {
		SetStatus(status, HDFS_ERR_NOT_FOUND, string("delete '") + path + "': path not found");
		return -1;
	}
	return 0;
}

int32_t hdfs_bridge_rename(hdfs_client_t *client, const char *src, const char *dst, bool overwrite,
                           hdfs_status_t *status) {
	// rename2 is atomic, and with `overwrite` replaces an existing file (or empty
	// directory) target; without it an existing target is an error.
	bool ok = Guard(status, string("rename '") + src + "' -> '" + dst + "' failed",
	                [&] { client->fs->rename2(src, dst, overwrite); });
	return ok ? 0 : -1;
}

} // extern "C"
