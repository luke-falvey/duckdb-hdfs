#pragma once

// Batched directory listing (the NameNode's getBatchedListing, Hadoop 3.3+):
// how many directories to send per call, and what to do when the NameNode
// rejects a call. Kept free of libhdfs3 so it can be unit-tested
// (test/cpp/hdfs_batch_test.cpp).

#include <cstddef>
#include <cstdio>
#include <string>

namespace hdfs_batch {

// Paths per call until the NameNode says otherwise: Hadoop's default
// dfs.batched.ls.limit.
constexpr size_t DEFAULT_LIMIT = 100;

enum class Rejection {
	// Not a rejection of batching: an ordinary failure (connection, ...).
	NONE,
	// The NameNode can't do batched listings (older than 3.3, or a Router):
	// list one directory per call from now on.
	UNSUPPORTED,
	// Too many paths in one call: retry with `limit` paths per call.
	TOO_MANY_PATHS,
};

struct Decision {
	Rejection rejection = Rejection::NONE;
	size_t limit = 0;
};

// Decide from a failed call's server exception class and message.
inline Decision ClassifyRejection(const std::string &error_class, const std::string &message) {
	Decision decision;
	if (error_class.find("RpcNoSuchMethodException") != std::string::npos ||
	    error_class.find("UnsupportedOperationException") != std::string::npos ||
	    message.find("Unknown method getBatchedListing") != std::string::npos) {
		decision.rejection = Rejection::UNSUPPORTED;
		return decision;
	}
	// FSNamesystem: "Too many source paths (%d > %d)", the second number
	// being the NameNode's dfs.batched.ls.limit.
	auto at = message.find("Too many source paths (");
	if (error_class.find("IllegalArgumentException") != std::string::npos && at != std::string::npos) {
		unsigned long sent = 0;
		unsigned long limit = 0;
		if (std::sscanf(message.c_str() + at, "Too many source paths (%lu > %lu)", &sent, &limit) == 2) {
			decision.rejection = limit >= 2 ? Rejection::TOO_MANY_PATHS : Rejection::UNSUPPORTED;
			decision.limit = static_cast<size_t>(limit);
		}
	}
	return decision;
}

} // namespace hdfs_batch
