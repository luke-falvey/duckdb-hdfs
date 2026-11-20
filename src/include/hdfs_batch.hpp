//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_batch.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <cstdio>

namespace duckdb {

//! Batched directory listing (the NameNode's getBatchedListing, Hadoop 3.3+): how many directories to send per
//! call, and what to do when the NameNode rejects a call.
struct HdfsBatchedListing {
	//! Paths per call until the NameNode says otherwise: Hadoop's default dfs.batched.ls.limit.
	static constexpr idx_t DEFAULT_LIMIT = 100;

	enum class Rejection {
		//! Not a rejection of batching: an ordinary failure (connection, ...).
		NONE,
		//! The NameNode can't do batched listings (older than 3.3, or a Router): list one directory per call.
		UNSUPPORTED,
		//! Too many paths in one call: retry with `limit` paths per call.
		TOO_MANY_PATHS,
	};

	struct Decision {
		Rejection rejection = Rejection::NONE;
		idx_t limit = 0;
	};

	//! Decide from a failed call's server exception class and message.
	static Decision ClassifyRejection(const string &error_class, const string &message) {
		Decision decision;
		if (error_class.find("RpcNoSuchMethodException") != string::npos ||
		    error_class.find("UnsupportedOperationException") != string::npos ||
		    message.find("Unknown method getBatchedListing") != string::npos) {
			decision.rejection = Rejection::UNSUPPORTED;
			return decision;
		}
		// FSNamesystem: "Too many source paths (%d > %d)", the second number being dfs.batched.ls.limit.
		auto at = message.find("Too many source paths (");
		if (error_class.find("IllegalArgumentException") == string::npos || at == string::npos) {
			return decision;
		}
		unsigned long sent = 0;  // NOLINT(google-runtime-int): matches the %lu conversion
		unsigned long limit = 0; // NOLINT(google-runtime-int)
		if (std::sscanf(message.c_str() + at, "Too many source paths (%lu > %lu)", &sent, &limit) == 2) {
			decision.rejection = limit >= 2 ? Rejection::TOO_MANY_PATHS : Rejection::UNSUPPORTED;
			decision.limit = static_cast<idx_t>(limit);
		}
		return decision;
	}
};

} // namespace duckdb
