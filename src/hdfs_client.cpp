// HDFS client (hdfs_client.hpp) on the native C++ libhdfs3 client. This is
// the only translation unit that includes libhdfs3 headers.

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

#include "duckdb/common/error_data.hpp"
#include "hdfs_batch.hpp"
#include "hdfs_client.hpp"
#include "hdfs_crypto.hpp"
#include "hdfs_glob.hpp"
#include "hdfs_logging.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

namespace duckdb {

namespace {

// --- errors ---------------------------------------------------------------

// A classified failure, carried from where it happens to an HdfsError.
struct Failure {
	HdfsErrorType type = HdfsErrorType::IO;
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
HdfsErrorType ClassifyServerClass(const string &cls) {
	if (cls.find("AccessControlException") != string::npos || cls.find("SecurityException") != string::npos) {
		return HdfsErrorType::PERMISSION;
	}
	if (cls.find("FileNotFoundException") != string::npos) {
		return HdfsErrorType::NOT_FOUND;
	}
	if (cls.find("FileAlreadyExistsException") != string::npos || cls.find("AlreadyBeingCreated") != string::npos) {
		return HdfsErrorType::ALREADY_EXISTS;
	}
	if (cls.find("StandbyException") != string::npos || cls.find("RetriableException") != string::npos) {
		return HdfsErrorType::CONNECTION;
	}
	return HdfsErrorType::IO;
}

// Classify one exception (not its nested causes).
HdfsErrorType ClassifyOne(const std::exception_ptr &ep) {
	try {
		std::rethrow_exception(ep);
	} catch (const HdfsError &e) {
		return e.type;
	} catch (const InvalidInputException &) {
		return HdfsErrorType::INVALID_INPUT;
	} catch (const Hdfs::AccessControlException &) {
		return HdfsErrorType::PERMISSION;
	} catch (const Hdfs::FileNotFoundException &) {
		return HdfsErrorType::NOT_FOUND;
	} catch (const Hdfs::FileAlreadyExistsException &) {
		return HdfsErrorType::ALREADY_EXISTS;
	} catch (const Hdfs::AlreadyBeingCreatedException &) {
		return HdfsErrorType::ALREADY_EXISTS;
	} catch (const Hdfs::InvalidPath &) {
		return HdfsErrorType::INVALID_INPUT;
	} catch (const Hdfs::InvalidParameter &) {
		return HdfsErrorType::INVALID_INPUT;
	} catch (const Hdfs::HdfsRpcServerException &e) {
		return ClassifyServerClass(e.getErrClass());
	} catch (const Hdfs::HdfsEndOfStream &) {
		return HdfsErrorType::IO;
	} catch (const Hdfs::HdfsNetworkException &) {
		return HdfsErrorType::CONNECTION;
	} catch (const Hdfs::HdfsRpcException &) {
		return HdfsErrorType::CONNECTION;
	} catch (const Hdfs::HdfsTimeoutException &) {
		return HdfsErrorType::CONNECTION;
	} catch (const Hdfs::HdfsFailoverException &) {
		return HdfsErrorType::CONNECTION;
	} catch (const Hdfs::NameNodeStandbyException &) {
		return HdfsErrorType::CONNECTION;
	} catch (const Hdfs::SaslException &) {
		return HdfsErrorType::CONNECTION;
	} catch (const Hdfs::HdfsFileSystemClosed &) {
		return HdfsErrorType::CONNECTION;
	} catch (...) {
		return HdfsErrorType::IO;
	}
}

// Classify an exception and build its message, walking nested causes:
// libhdfs3 often wraps the specific failure in a generic HdfsIOException.
Failure Describe(const std::exception_ptr &ep) {
	Failure failure;
	bool classified = false;
	std::exception_ptr current = ep;
	for (int depth = 0; current && depth < 8; depth++) {
		auto type = ClassifyOne(current);
		if (!classified || failure.type == HdfsErrorType::IO) {
			failure.type = type;
			classified = true;
		}
		std::exception_ptr next;
		try {
			std::rethrow_exception(current);
		} catch (const std::exception &e) {
			// DuckDB exceptions carry their message as JSON in what().
			auto line = FirstLine(ErrorData(e).RawMessage().c_str());
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

// Run `fn`; turn any failure into an HdfsError whose message starts with `context` (if any).
template <class FN>
auto Guard(const string &context, FN &&fn) -> decltype(fn()) {
	try {
		return fn();
	} catch (const HdfsError &) {
		throw;
	} catch (...) {
		auto failure = Describe(std::current_exception());
		throw HdfsError(failure.type, context.empty() ? failure.message : context + ": " + failure.message);
	}
}

// --- RPC logging ----------------------------------------------------------------

// The logger this thread's NameNode RPCs go to (HdfsClient::SetThreadLogger).
weak_ptr<Logger> &ThreadLogger() {
	thread_local weak_ptr<Logger> logger;
	return logger;
}

// Logs one NameNode RPC (HdfsLogType) when it goes out of scope: whether it
// threw, and how long it ran (not counting paused stretches). Costs nothing
// beyond a weak_ptr lock unless the HDFS log type is enabled.
class RpcLog {
public:
	RpcLog(const char *op_p, const string &path_p) : op(op_p) {
		logger = ThreadLogger().lock();
		if (logger && !logger->ShouldLog(HdfsLogType::NAME, HdfsLogType::LEVEL)) {
			logger.reset();
		}
		if (logger) {
			path = path_p;
			exceptions = std::uncaught_exceptions();
			start = std::chrono::steady_clock::now();
		}
	}

	~RpcLog() {
		if (!logger) {
			return;
		}
		try {
			Pause();
			bool ok = std::uncaught_exceptions() == exceptions;
			auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
			logger->WriteLog(HdfsLogType::NAME, HdfsLogType::LEVEL,
			                 HdfsLogType::ConstructLogMessage(op, path, static_cast<int64_t>(us), ok));
		} catch (...) { // NOLINT: logging must not fail the call
		}
	}

	RpcLog(const RpcLog &) = delete;
	RpcLog &operator=(const RpcLog &) = delete;

	// Stop timing (e.g. while the caller processes a page of results).
	void Pause() {
		if (logger && running) {
			elapsed += std::chrono::steady_clock::now() - start;
			running = false;
		}
	}

	void Resume() {
		if (logger && !running) {
			start = std::chrono::steady_clock::now();
			running = true;
		}
	}

private:
	const char *op;
	shared_ptr<Logger> logger;
	string path;
	int exceptions = 0;
	bool running = true;
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::duration elapsed {0};
};

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

HdfsFileStatus ToStatus(const Hdfs::FileStatus &st, const string &path) {
	HdfsFileStatus e;
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
	e.file_id = st.getFileId();
	return e;
}

// List `dir`, calling `fn(child_path, status)` for each child until it returns
// false. Listing a file yields the file itself once, as in the Hadoop client.
// (libhdfs3's iterator would return it forever.) The iterator fetches a page
// (dfs.ls.limit entries, 1000 by default) per getListing call, so a listing
// stopped early makes no further calls. Logs one entry for the whole listing,
// timing only the calls into the iterator. Returns false if `fn` stopped it.
template <class FN>
bool ListDirectory(Hdfs::FileSystem &fs, const string &dir, FN &&fn) {
	RpcLog log("listDirectory", dir);
	auto it = fs.listDirectory(dir.c_str());
	while (it.hasNext()) {
		auto st = it.getNext();
		auto path = ChildPath(dir, st.getPath());
		bool is_self = path == dir;
		log.Pause();
		if (!fn(path, st)) {
			return false;
		}
		log.Resume();
		if (is_self) {
			break;
		}
	}
	return true;
}

// --- streaming listing ----------------------------------------------------------

// A directory walk run by up to `max_parallelism` worker threads. Workers take
// directories from a LIFO stack (depth-first-ish, which keeps the stack small
// and the parent-to-child listing gap short), list them, and queue the entries
// for the consumer a page at a time. The queue is bounded: a full queue pauses
// the walk until Next() drains it. Once the walk stops (the consumer is done or
// cancelled, or an error), workers stop between pages and between directories.
// Any exception in a worker (including bad_alloc, or a failure to start another
// worker thread) fails the walk and is rethrown by Next() on the consumer.
class HdfsWalkImpl : public HdfsWalk {
public:
	struct Task {
		string path;
		vector<HdfsGlobPlan::State> states;
		bool is_root = false;
		// Stat the path instead of listing it (an all-literal glob).
		bool stat_only = false;
	};

	// Entries queued for the consumer before the walk pauses.
	static constexpr size_t BUFFER = 8192;
	// Entries of a plain listing handed to Process at once (the NameNode's default page size).
	static constexpr size_t LISTING_CHUNK = 1000;

	std::shared_ptr<Hdfs::FileSystem> fs;
	string display_path;
	bool glob = false;
	bool recursive = false;
	bool skip_permission_errors = false;
	bool include_hidden = false;
	bool batched_listing = true;
	size_t max_parallelism = 1;
	unique_ptr<HdfsGlobPlan> plan;
	std::shared_ptr<std::atomic<size_t>> batch_limit; // see HdfsClientImpl
	// Where the workers log their RPCs: the logger of the thread that started the walk.
	shared_ptr<Logger> logger;

	std::mutex mu;
	std::condition_variable cv; // any state change
	std::vector<Task> pending;
	size_t in_flight = 0;
	size_t idle_workers = 0;
	std::deque<HdfsFileStatus> items;
	bool has_error = false;
	Failure error;
	bool cancelled = false;
	// cancelled || has_error, readable without `mu` (checked between listing pages).
	std::atomic<bool> stopping {false};
	std::vector<std::thread> workers;

	~HdfsWalkImpl() override {
		{
			std::lock_guard<std::mutex> lock(mu);
			cancelled = true;
			stopping = true;
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

	// Directories one worker takes at a time (one batched listing call).
	size_t BatchSize() const {
		auto limit = batched_listing && batch_limit ? batch_limit->load() : 0;
		return limit > 1 ? limit : 1;
	}

	// Add a worker if there is queued work no idle worker can take. Caller holds `mu`.
	void SpawnIfNeeded() {
		// The destructor joins `workers` once the walk stops; adding a worker then would race with it.
		if (cancelled || has_error) {
			return;
		}
		size_t batch = BatchSize();
		size_t units = (pending.size() + batch - 1) / batch;
		if (idle_workers < units && workers.size() < max_parallelism) {
			// `workers` has room for max_parallelism threads (see NewWalk), so only starting the thread can fail.
			try {
				workers.emplace_back([this] { Work(); });
			} catch (const std::exception &e) {
				// Out of threads: the running workers take the queued work. With none, the walk fails.
				if (workers.empty()) {
					FailLocked(Failure {HdfsErrorType::IO, string("could not start a listing thread: ") + e.what()});
				}
			}
		}
	}

	// Queue an entry, waiting while the buffer is full. Returns false once the
	// walk is stopping. Caller holds `lock`.
	bool Emit(std::unique_lock<std::mutex> &lock, HdfsFileStatus entry) {
		cv.wait(lock, [&] { return items.size() < BUFFER || cancelled || has_error; });
		if (cancelled || has_error) {
			return false;
		}
		items.push_back(std::move(entry));
		cv.notify_all();
		return true;
	}

	// Stop the walk with `failure` (the first one wins). Caller holds `mu`.
	void FailLocked(Failure failure) {
		if (!has_error) {
			has_error = true;
			stopping = true;
			error = std::move(failure);
		}
		cv.notify_all();
	}

	void Fail(std::unique_lock<std::mutex> &, Failure failure) {
		FailLocked(std::move(failure));
	}

	// Describe(), for where a second exception (e.g. bad_alloc again) must not escape.
	static Failure DescribeNoThrow(const std::exception_ptr &ep) noexcept {
		try {
			return Describe(ep);
		} catch (...) {
			Failure failure;
			failure.message = "out of memory"; // fits the small-string buffer: no allocation
			return failure;
		}
	}

	// A worker thread's body. Nothing may escape it (that would terminate the process).
	void Work() noexcept {
		try {
			HdfsClient::SetThreadLogger(logger);
			WorkLoop();
		} catch (...) {
			auto failure = DescribeNoThrow(std::current_exception());
			std::lock_guard<std::mutex> lock(mu);
			FailLocked(std::move(failure));
		}
	}

	void WorkLoop() {
		std::unique_lock<std::mutex> lock(mu);
		for (;;) {
			idle_workers++;
			cv.wait(lock, [&] { return !pending.empty() || Finished(); });
			idle_workers--;
			if (cancelled || has_error || pending.empty()) {
				cv.notify_all(); // let the consumer see completion
				return;
			}
			// Take a batch of directories to list in one call; a stat runs alone.
			std::vector<Task> batch;
			size_t size = BatchSize();
			do {
				batch.push_back(std::move(pending.back()));
				pending.pop_back();
			} while (!batch[0].stat_only && batch.size() < size && !pending.empty() && !pending.back().stat_only);
			in_flight++;
			lock.unlock();
			bool failed = false;
			Failure failure;
			try {
				if (batch.size() == 1) {
					Run(batch[0]);
				} else {
					RunBatch(batch);
				}
			} catch (...) {
				failed = true;
				failure = DescribeNoThrow(std::current_exception());
			}
			lock.lock();
			in_flight--;
			if (failed) {
				FailLocked(std::move(failure));
			}
			cv.notify_all();
		}
	}

	// Whether a failed listing below the root is pruned rather than fatal.
	bool Prunable(const Task &task, const Failure &failure) const {
		if (glob && failure.type == HdfsErrorType::NOT_FOUND) {
			return true; // a glob matching nothing (or a raced delete) is no error
		}
		if (task.is_root) {
			return false;
		}
		return failure.type == HdfsErrorType::NOT_FOUND ||
		       (skip_permission_errors && failure.type == HdfsErrorType::PERMISSION);
	}

	using Children = std::vector<std::pair<string, Hdfs::FileStatus>>;

	// A task's listing failed: prune it or fail the walk. Caller doesn't hold `mu`.
	void HandleFailure(const Task &task, const Failure &failure) {
		std::unique_lock<std::mutex> lock(mu);
		if (!Prunable(task, failure)) {
			Fail(lock, failure);
		}
	}

	// Run one task without holding `mu` during RPCs. A directory's entries are
	// processed a chunk at a time while it is being listed, so a huge directory
	// is never held in memory whole, and its listing stops once the walk does.
	void Run(const Task &task) {
		Children children;
		if (task.stat_only) {
			try {
				RpcLog log("getFileInfo", task.path);
				children.emplace_back(task.path, fs->getFileStatus(task.path.c_str()));
			} catch (...) {
				HandleFailure(task, Describe(std::current_exception()));
				return;
			}
			Process(task, children);
			return;
		}
		try {
			bool completed = ListDirectory(*fs, task.path, [&](const string &path, const Hdfs::FileStatus &st) {
				if (stopping) {
					return false;
				}
				children.emplace_back(path, st);
				if (children.size() < LISTING_CHUNK) {
					return true;
				}
				bool more = Process(task, children);
				children.clear();
				return more;
			});
			if (!completed) {
				return; // the walk is stopping
			}
		} catch (...) {
			HandleFailure(task, Describe(std::current_exception()));
			return;
		}
		Process(task, children);
	}

	// The NameNode rejected a batched call: learn its limit (or that it can't
	// batch). The limit only ever goes down, so a requeued batch is smaller.
	void LowerBatchLimit(const HdfsBatchedListing::Decision &decision) {
		size_t target = decision.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED ? 0 : decision.limit;
		auto current = batch_limit->load();
		while (current > target && !batch_limit->compare_exchange_weak(current, target)) {
		}
	}

	// List several directories in batched listing calls (one call unless the
	// listings are too large for one response), processing each response as
	// it arrives.
	void RunBatch(std::vector<Task> &tasks) {
		std::vector<string> paths;
		paths.reserve(tasks.size());
		for (auto &task : tasks) {
			paths.push_back(task.path);
		}
		const string log_path = paths[0] + " (+" + std::to_string(paths.size() - 1) + " more)";
		string key;
		bool more = true;
		bool first_call = true;
		std::vector<Hdfs::BatchedListing> listings;
		Children children;
		while (more) {
			if (stopping) {
				return;
			}
			listings.clear();
			string previous_key = key;
			try {
				RpcLog log("getBatchedListing", log_path);
				more = fs->getBatchedListing(paths, key, listings);
			} catch (const Hdfs::HdfsRpcServerException &e) {
				auto decision = HdfsBatchedListing::ClassifyRejection(e.getErrClass(), e.getErrMsg(), tasks.size());
				// A rejection comes on the first call; later, entries have already been handed out.
				if (first_call && decision.rejection != HdfsBatchedListing::Rejection::NONE) {
					// Hand the directories back, to be listed in smaller batches or one per call.
					LowerBatchLimit(decision);
					Requeue(tasks);
					return;
				}
				FailAll(tasks, Describe(std::current_exception()));
				return;
			} catch (...) {
				FailAll(tasks, Describe(std::current_exception()));
				return;
			}
			first_call = false;
			if (more && key == previous_key) {
				// A NameNode that never advances would be asked for the same page forever.
				FailAll(tasks, Failure {HdfsErrorType::IO, "getBatchedListing did not advance (the NameNode returned "
				                                           "the same resume key)"});
				return;
			}
			for (auto &listing : listings) {
				auto &task = tasks[listing.index]; // libhdfs3 checks the index
				if (listing.error) {
					HandleFailure(task, Describe(listing.error));
					continue;
				}
				children.clear();
				for (auto &st : listing.entries) {
					children.emplace_back(ChildPath(task.path, st.getPath()), st);
				}
				if (!Process(task, children)) {
					return; // the walk is stopping
				}
			}
		}
	}

	// A whole batched call failed (e.g. lost connection): each task fails as
	// a single listing would have.
	void FailAll(const std::vector<Task> &tasks, const Failure &failure) {
		for (auto &task : tasks) {
			HandleFailure(task, failure);
		}
	}

	void Requeue(std::vector<Task> &tasks) {
		std::lock_guard<std::mutex> lock(mu);
		for (auto &task : tasks) {
			pending.push_back(std::move(task));
		}
		SpawnIfNeeded();
		cv.notify_all();
	}

	// Match and queue listed children of `task` (all of them, or one page).
	// Returns false once the walk is stopping. Caller doesn't hold `mu`.
	bool Process(const Task &task, Children &children) {
		std::unique_lock<std::mutex> lock(mu);
		if (cancelled || has_error) {
			return false;
		}
		if (task.stat_only) {
			return Emit(lock, ToStatus(children[0].second, children[0].first));
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
				if (step.emit && !Emit(lock, ToStatus(st, path))) {
					return false;
				}
			} else {
				// The listed path itself is exempt from hidden filtering.
				if (!include_hidden && path != task.path && HdfsGlobPlan::IsHidden(BaseName(path))) {
					continue;
				}
				if (recursive && is_dir && path != task.path) {
					pending.push_back(Task {path, {}, false, false});
					SpawnIfNeeded();
					cv.notify_all();
				}
				if (!Emit(lock, ToStatus(st, path))) {
					return false;
				}
			}
		}
		return true;
	}

	bool Next(vector<HdfsFileStatus> &out, idx_t max_entries, int32_t timeout_ms, bool &pending) override {
		out.clear();
		pending = false;
		std::unique_lock<std::mutex> lock(mu);
		auto ready = [&] {
			return !items.empty() || Finished();
		};
		if (timeout_ms < 0) {
			cv.wait(lock, ready);
		} else if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
			pending = true;
			return true;
		}
		if (!items.empty()) {
			while (!items.empty() && out.size() < max_entries) {
				out.push_back(std::move(items.front()));
				items.pop_front();
			}
			cv.notify_all(); // room for paused workers
			return true;
		}
		if (has_error) {
			throw HdfsError(error.type, "list '" + display_path + "' failed: " + error.message);
		}
		return false; // walk complete
	}
};
// --- readers and writers -----------------------------------------------------------

// Client-wide state the readers, writers and walks need.
struct ClientState {
	std::shared_ptr<Hdfs::FileSystem> fs;
	int replication = 0;
	int64_t block_size = 0;
	// Bytes of block locations fetched when opening a file (as libhdfs3's own
	// open does: dfs.prefetchsize blocks).
	int64_t prefetch_size = 0;
	// For encryption zones: the user sent to the KMS, and the KMS provider URI
	// from hadoop.security.key.provider.path / dfs.encryption.key.provider.uri
	// (empty if not configured).
	string user;
	string key_provider;
	// Kerberos cluster: the KMS is called with SPNEGO instead of user.name.
	bool kerberos = false;
	// This client's decrypted data encryption keys; wiped when the client goes.
	std::shared_ptr<HdfsKeyCache> key_cache = std::make_shared<HdfsKeyCache>();
	// Directories per batched listing call (getBatchedListing); 0 lists one
	// directory per call. Lowered when the NameNode rejects a call, and shared
	// by this client's walks so the lesson is learned once.
	std::shared_ptr<std::atomic<size_t>> batch_limit = std::make_shared<std::atomic<size_t>>(0);
};

// The data encryption key of a file in an encryption zone, decrypted by the KMS.
string FileKey(const ClientState &client, const Hdfs::FileEncryptionInfo &info, const string &path) {
	if (info.suite != HdfsCrypto::SUITE_AES_CTR_NOPADDING) {
		throw IOException(path + " uses an unsupported encryption cipher suite (" + std::to_string(info.suite) +
		                  "); only AES/CTR/NoPadding is supported");
	}
	if (info.protocolVersion != HdfsCrypto::PROTOCOL_ENCRYPTION_ZONES) {
		throw IOException(path + " uses an unsupported encryption protocol version (" +
		                  std::to_string(info.protocolVersion) + ")");
	}
	if (client.key_provider.empty()) {
		throw IOException(path + " is in an HDFS encryption zone, but no key provider is configured "
		                         "(set hadoop.security.key.provider.path in core-site.xml)");
	}
	return HdfsCrypto::DecryptEncryptedKey(*client.key_cache, HdfsCrypto::ParseKmsUri(client.key_provider),
	                                       client.kerberos, client.user, info.keyName, info.ezKeyVersionName, info.iv,
	                                       info.key);
}

// An open file. libhdfs3 input streams aren't safe for concurrent use, so the
// reader keeps a pool: each concurrent read borrows its own stream.
class HdfsReaderImpl : public HdfsReader {
public:
	// `version`: the file's status (from a listing or a stat) just before opening; its mtime and file id identify
	// the version read. The length always comes from the block locations.
	HdfsReaderImpl(const ClientState &client, string path_p, const HdfsFileStatus &version)
	    : fs(client.fs), path(std::move(path_p)), mtime(version.mtime), file_id(version.file_id) {
		// One RPC: the block locations carry the length and any encryption info.
		// (A directory fails here with "Path is not a file".)
		blocks = std::make_shared<Hdfs::Internal::LocatedBlocksImpl>();
		{
			RpcLog log("getBlockLocations", path);
			fs->getBlockLocations(path.c_str(), 0, client.prefetch_size, *blocks);
		}
		length = blocks->getFileLength();
		if (blocks->isEncrypted()) {
			encrypted = true;
			key = FileKey(client, blocks->getEncryptionInfo(), path);
			iv = blocks->getEncryptionInfo().iv;
		}
		// Open one stream up front so open errors surface here, not on first read.
		Return(NewStream());
	}

	~HdfsReaderImpl() override {
		HdfsCrypto::Wipe(key);
		for (auto &stream : idle) {
			try {
				stream->close();
			} catch (...) { // NOLINT: best effort on teardown
			}
		}
	}

	int64_t Length() const override {
		return length;
	}

	uint64_t ModificationTime() const override {
		return mtime;
	}

	// Replace the version a reader was opened with (before it is shared): the file changed after `version` was
	// taken, so a fresh stat names it.
	void SetVersion(const HdfsFileStatus &version) {
		mtime = version.mtime;
		file_id = version.file_id;
	}

	// Whether this reader reads the file version `status` describes. A file id of 0 is unknown and not compared.
	bool Reads(const HdfsFileStatus &status) const {
		return !status.is_dir && status.length == length && status.mtime == mtime &&
		       (status.file_id == 0 || file_id == 0 || status.file_id == file_id);
	}

	bool SameVersion(const HdfsReaderImpl &other) const {
		return other.length == length && other.mtime == mtime &&
		       (other.file_id == 0 || file_id == 0 || other.file_id == file_id);
	}

	void Read(data_ptr_t buffer, idx_t nr_bytes, idx_t offset) override {
		// A stream that fails mid-read is dropped rather than reused.
		auto stream = Guard("read at offset " + std::to_string(offset) + " failed", [&] {
			auto borrowed = Borrow();
			idx_t done = 0;
			while (done < nr_bytes) {
				// libhdfs3 reads at most INT32_MAX bytes per call.
				constexpr idx_t MAX_CHUNK = 64 << 20;
				auto chunk = static_cast<int32_t>(MinValue<idx_t>(nr_bytes - done, MAX_CHUNK));
				int32_t n = borrowed->pread(reinterpret_cast<char *>(buffer) + done, chunk,
				                            static_cast<int64_t>(offset + done));
				if (n <= 0) {
					throw Hdfs::HdfsEndOfStream("unexpected end of file", __FILE__, __LINE__, "");
				}
				done += static_cast<idx_t>(n);
			}
			if (encrypted) {
				HdfsCrypto::AesCtrTransform(key, iv, offset, buffer, nr_bytes);
			}
			return borrowed;
		});
		Return(std::move(stream));
	}

private:
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

	std::shared_ptr<Hdfs::FileSystem> fs;
	string path;
	int64_t length = 0;
	uint64_t mtime = 0;
	int64_t file_id = 0;
	// Files in an encryption zone: the decrypted data key and the file IV.
	bool encrypted = false;
	string key;
	string iv;
	// Block locations fetched at open, shared out (as copies) to every pooled
	// stream so opening another stream costs no NameNode RPC.
	std::shared_ptr<Hdfs::Internal::LocatedBlocksImpl> blocks;
	std::mutex mu;
	std::vector<std::unique_ptr<Hdfs::InputStream>> idle;
};

class HdfsWriterImpl : public HdfsWriter {
public:
	HdfsWriterImpl(const ClientState &client, const string &path_p, bool overwrite) : fs(client.fs), path(path_p) {
		int flags = Hdfs::Create | (overwrite ? Hdfs::Overwrite : 0);
		Hdfs::Permission permission(0644);
		// Missing parent directories are created, as in the Hadoop client.
		auto created = [&] {
			RpcLog log("create", path);
			return fs->createOrAppend(path.c_str(), flags, permission, /*createParent=*/true, client.replication,
			                          client.block_size);
		}();
		auto &file = *created.second;
		if (file.isEncrypted()) {
			try {
				key = FileKey(client, file.getEncryptionInfo(), path);
			} catch (...) {
				// Don't leave an empty file behind that we can't write.
				try {
					RpcLog log("delete", path);
					fs->deletePath(path.c_str(), false);
				} catch (...) { // NOLINT: the original error matters more
				}
				throw;
			}
			encrypted = true;
			iv = file.getEncryptionInfo().iv;
		}
		auto ec = file.getEcPolicy();
		out = ec ? std::make_unique<Hdfs::OutputStream>(ec) : std::make_unique<Hdfs::OutputStream>();
		out->open(*fs, path.c_str(), created, flags, permission, true, client.replication, client.block_size,
		          file.getFileId());
	}

	void Write(const_data_ptr_t buffer, idx_t nr_bytes) override {
		Guard("write failed", [&] {
			if (!encrypted) {
				out->append(reinterpret_cast<const char *>(buffer), static_cast<int64_t>(nr_bytes));
				return;
			}
			scratch.assign(buffer, buffer + nr_bytes);
			HdfsCrypto::AesCtrTransform(key, iv, position, scratch.data(), scratch.size());
			out->append(reinterpret_cast<const char *>(scratch.data()), static_cast<int64_t>(nr_bytes));
			position += nr_bytes;
		});
	}

	~HdfsWriterImpl() override {
		// Never completed: abandon the file rather than let the stream's destructor complete it.
		try {
			Abort();
		} catch (...) { // NOLINT: best effort on teardown
		}
		HdfsCrypto::Wipe(key);
	}

	void Sync() override {
		Guard("sync failed", [&] { out->sync(); });
	}

	void Close() override {
		Guard("close writer failed", [&] {
			// A stream whose close fails is reset by libhdfs3; the file stays ours to Abort().
			out->close();
			out.reset();
			completed = true;
		});
	}

	void Abort() override {
		if (completed || aborted) {
			return;
		}
		aborted = true;
		// Delete first: closing (or destroying) an open stream completes the file, publishing it with partial
		// contents. Once the file is gone, the stream's close fails fast instead.
		try {
			RpcLog log("delete", path);
			fs->deletePath(path.c_str(), false);
		} catch (...) {
			auto failure = Describe(std::current_exception());
			// The NameNode is unreachable, or refused: releasing the stream now would try to complete the file
			// (and block on the same NameNode). Leave the stream unclosed instead: the file stays under construction,
			// not complete, and its lease lapses once this client disconnects.
			(void)out.release(); // NOLINT: deliberately leaked, see above
			throw HdfsError(failure.type, "delete incomplete file '" + path + "' failed: " + failure.message);
		}
		try {
			out.reset();
		} catch (...) { // NOLINT: the file is already gone
		}
	}

private:
	std::shared_ptr<Hdfs::FileSystem> fs;
	string path;
	std::unique_ptr<Hdfs::OutputStream> out;
	// Close() completed the file / Abort() ran: neither may run again.
	bool completed = false;
	bool aborted = false;
	// Files in an encryption zone: the data key, the file IV, and how many
	// bytes have been written (the AES-CTR position).
	bool encrypted = false;
	string key;
	string iv;
	uint64_t position = 0;
	std::vector<uint8_t> scratch;
};

// --- client ----------------------------------------------------------------------------

class HdfsClientImpl : public HdfsClient {
public:
	explicit HdfsClientImpl(ClientState state_p) : state(std::move(state_p)) {
	}

	~HdfsClientImpl() override {
		try {
			state.fs->disconnect();
		} catch (...) { // NOLINT: best effort on teardown
		}
	}

	HdfsFileStatus Stat(const string &path) override {
		return Guard("stat '" + path + "' failed", [&] {
			RpcLog log("getFileInfo", path);
			return ToStatus(state.fs->getFileStatus(path.c_str()), path);
		});
	}

	shared_ptr<HdfsReader> Open(const string &path, optional_ptr<const HdfsFileStatus> expected) override {
		// Without a listing's status, one stat names the version to open (and to share).
		auto version = expected ? *expected : Stat(path);
		{
			std::lock_guard<std::mutex> lock(readers_mu);
			auto it = open_readers.find(path);
			if (it != open_readers.end()) {
				auto reader = it->second.lock();
				if (reader && reader->Reads(version)) {
					return reader;
				}
			}
		}
		auto reader = Guard("open '" + path + "' for reading failed",
		                    [&] { return make_shared_ptr<HdfsReaderImpl>(state, path, version); });
		// Block locations carry no mtime or file id. A changed length shows that the file changed after `version` was
		// taken: name the version the block locations belong to. (A same-size replacement in the moment between the
		// listing and this call goes unnoticed.)
		if (reader->Length() != version.length) {
			reader->SetVersion(Stat(path));
		}
		std::lock_guard<std::mutex> lock(readers_mu);
		auto &entry = open_readers[path];
		auto current = entry.lock();
		if (current && current->SameVersion(*reader)) {
			// Another thread opened the same version meanwhile: share its reader.
			return current;
		}
		entry = reader;
		// Drop entries of readers that have since been closed.
		for (auto it = open_readers.begin(); it != open_readers.end();) {
			it = it->second.expired() ? open_readers.erase(it) : std::next(it);
		}
		return std::move(reader);
	}

	unique_ptr<HdfsWriter> Create(const string &path, bool overwrite) override {
		ForgetReaders(path);
		return Guard("create '" + path + "' for writing failed",
		             [&] { return make_uniq<HdfsWriterImpl>(state, path, overwrite); });
	}

	void CreateDirectories(const string &path) override {
		Guard("mkdirs '" + path + "' failed", [&] {
			RpcLog log("mkdirs", path);
			state.fs->mkdirs(path.c_str(), Hdfs::Permission(0755));
		});
	}

	void CreateDirectory(const string &path) override {
		Guard("mkdir '" + path + "' failed", [&] {
			RpcLog log("mkdirs", path);
			state.fs->mkdir(path.c_str(), Hdfs::Permission(0755));
		});
	}

	bool Delete(const string &path, bool recursive) override {
		ForgetReaders(path);
		return Guard("delete '" + path + "' failed", [&] {
			RpcLog log("delete", path);
			return state.fs->deletePath(path.c_str(), recursive);
		});
	}

	void Rename(const string &source, const string &target, bool overwrite) override {
		// rename2 is atomic, and with `overwrite` replaces an existing file (or empty
		// directory) target; without it an existing target is an error.
		ForgetReaders(source);
		ForgetReaders(target);
		Guard("rename '" + source + "' -> '" + target + "' failed", [&] {
			RpcLog log("rename2", source);
			state.fs->rename2(source.c_str(), target.c_str(), overwrite);
		});
	}

	unique_ptr<HdfsWalk> List(const string &path, bool recursive, const HdfsWalkOptions &options) override {
		auto walk = NewWalk(path, options);
		walk->recursive = recursive;
		walk->Start({path, {}, true, false});
		return std::move(walk);
	}

	unique_ptr<HdfsWalk> Glob(const string &pattern, const HdfsWalkOptions &options) override {
		auto plan = Guard("invalid glob pattern", [&] {
			return make_uniq<HdfsGlobPlan>(HdfsGlobPlan::Parse(pattern, options.include_hidden));
		});
		auto walk = NewWalk(pattern, options);
		walk->glob = true;
		auto root = plan->Root();
		bool emit_root = plan->EmitRoot();
		auto initial = plan->Initial();
		walk->plan = std::move(plan);
		{
			std::lock_guard<std::mutex> lock(walk->mu);
			// An all-literal pattern (or brace alternative) matches the root itself.
			if (emit_root) {
				walk->pending.push_back({root, {}, true, true});
			}
			if (!initial.empty()) {
				walk->pending.push_back({root, initial, true, false});
			}
			walk->SpawnIfNeeded();
		}
		walk->cv.notify_all();
		return std::move(walk);
	}

private:
	unique_ptr<HdfsWalkImpl> NewWalk(const string &path, const HdfsWalkOptions &options) {
		auto walk = make_uniq<HdfsWalkImpl>();
		walk->fs = state.fs;
		walk->batch_limit = state.batch_limit;
		walk->display_path = path;
		walk->max_parallelism =
		    MinValue<idx_t>(MaxValue<idx_t>(options.max_parallelism, 1), HdfsWalkOptions::MAX_PARALLELISM);
		// Room for every worker, so that starting one can only fail at thread creation (see SpawnIfNeeded).
		walk->workers.reserve(walk->max_parallelism);
		walk->logger = ThreadLogger().lock();
		walk->skip_permission_errors = options.skip_permission_errors;
		walk->include_hidden = options.include_hidden;
		walk->batched_listing = options.batched_listing;
		return walk;
	}

	// Stop sharing open readers of `path` and anything below it: the files there
	// are about to change. Readers already handed out keep working.
	void ForgetReaders(const string &path) {
		if (path.empty()) {
			return;
		}
		std::lock_guard<std::mutex> lock(readers_mu);
		auto prefix = path.back() == '/' ? path : path + "/";
		for (auto it = open_readers.begin(); it != open_readers.end();) {
			bool below = it->first == path || it->first.compare(0, prefix.size(), prefix) == 0;
			it = below ? open_readers.erase(it) : std::next(it);
		}
	}

	ClientState state;
	// Files open for reading, by path. DuckDB often opens a file again while a
	// handle on it is still open (Parquet: one handle for the metadata, one
	// for the scan); the second open shares the reader instead of asking the
	// NameNode again.
	std::mutex readers_mu;
	unordered_map<string, weak_ptr<HdfsReaderImpl>> open_readers;
};

// Set libhdfs3's (process-wide) log severity from dfs.client.log.severity:
// FATAL by default (see LoadConfig), INFO or DEBUG1 to debug retries and
// failovers. The most recent connection's config wins.
void SetLogSeverity(const Hdfs::Config &conf) {
	using Severity = decltype(Hdfs::Internal::FATAL);
	// libhdfs3's LogSeverity values, in order.
	static const char *const names[] = {"FATAL", "ERROR", "WARNING", "INFO", "DEBUG1", "DEBUG2", "DEBUG3"};
	string value = conf.getString("dfs.client.log.severity", "FATAL");
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (value == names[i]) {
			Hdfs::Internal::RootLogger.setLogSeverity(static_cast<Severity>(i));
			return;
		}
	}
}

[[noreturn]] void ConfigError(const string &message) {
	throw HdfsError(HdfsErrorType::INVALID_CONFIGURATION, message);
}

} // namespace

void HdfsClient::SetThreadLogger(const shared_ptr<Logger> &logger) {
	ThreadLogger() = logger;
}

shared_ptr<HdfsClient> HdfsClient::Connect(const string &authority, const HdfsConnectionConfig &config) {
	// The caller names the connection in its error message.
	return Guard("", [&]() -> shared_ptr<HdfsClient> {
		// libhdfs3 has one process-wide logger that writes to stderr (there is no per-client sink to route into
		// DuckDB's log), and logs retries and failovers at INFO: keep it quiet by default, until the config is read
		// (SetLogSeverity). NameNode RPCs are logged to DuckDB's log instead (HdfsLogType).
		Hdfs::Internal::RootLogger.setLogSeverity(Hdfs::Internal::FATAL);
		if (!config.config_dir.empty() && !FileExists(config.config_dir + "/core-site.xml") &&
		    !FileExists(config.config_dir + "/hdfs-site.xml")) {
			ConfigError("config_dir '" + config.config_dir + "' has no core-site.xml or hdfs-site.xml");
		}
		auto conf = LoadConfig(config.config_dir.empty() ? nullptr : config.config_dir.c_str());
		SetLogSeverity(conf);
		string auth = conf.getString("hadoop.security.authentication", "simple");
		std::transform(auth.begin(), auth.end(), auth.begin(), [](unsigned char c) { return std::tolower(c); });
		if (auth != "simple" && auth != "kerberos") {
			ConfigError("hadoop.security.authentication=" + auth + " is not supported (use simple or kerberos)");
		}
		bool kerberos = auth == "kerberos";
		string uri = !authority.empty() ? authority : conf.getString("fs.defaultFS", "");
		if (uri.empty() || uri.rfind("hdfs://", 0) != 0) {
			ConfigError("no NameNode given: use hdfs://host:port/... or set fs.defaultFS to an hdfs:// URI in "
			            "core-site.xml");
		}
		string username;
		if (kerberos) {
			// The identity is the principal in the Kerberos ticket cache
			// (KRB5CCNAME, or hadoop.security.kerberos.ticket.cache.path), as in
			// Hadoop: HADOOP_USER_NAME doesn't apply.
			if (!GetEnv("HADOOP_PROXY_USER").empty()) {
				ConfigError("HADOOP_PROXY_USER (impersonation) is not supported on Kerberos clusters");
			}
			if (!config.user.empty()) {
				ConfigError("a user can't be set on a Kerberos cluster: the identity is the Kerberos ticket's "
				            "principal");
			}
		} else {
			username = !config.user.empty() ? config.user : GetEnv("HADOOP_USER_NAME");
			if (username.empty()) {
				username = GetEnv("HADOOP_PROXY_USER");
			}
		}
		ClientState state;
		state.fs = std::make_shared<Hdfs::FileSystem>(conf);
		state.fs->connect(uri.c_str(), username.empty() ? nullptr : username.c_str(), nullptr);
		Hdfs::Internal::SessionConfig session(conf);
		state.replication = session.getDefaultReplica();
		state.block_size = session.getDefaultBlockSize();
		state.prefetch_size = session.getDefaultBlockSize() * session.getPrefetchSize();
		// Batched listings (Hadoop 3.3+). Older NameNodes and Routers reject the
		// first call, and the client then lists one directory per call. The
		// hdfs-site.xml key is deprecated in favour of hdfs_batched_listing.
		if (conf.getBool("duckdb.hdfs.batched-listing.enabled", true)) {
			state.batch_limit->store(HdfsBatchedListing::DEFAULT_LIMIT);
		}
		state.user = kerberos || !username.empty() ? username : HdfsCrypto::CurrentOsUser();
		state.kerberos = kerberos;
		state.key_provider = conf.getString("hadoop.security.key.provider.path", "");
		if (state.key_provider.empty()) {
			state.key_provider = conf.getString("dfs.encryption.key.provider.uri", "");
		}
		return make_shared_ptr<HdfsClientImpl>(std::move(state));
	});
}

} // namespace duckdb
