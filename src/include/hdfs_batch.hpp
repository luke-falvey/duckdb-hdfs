//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_batch.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <cstring>

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

	//! Decide from a failed call's server exception class and message. `sent` is the number of paths the rejected
	//! call carried: a limit is only accepted if it is smaller, so retrying with it always makes progress (a limit
	//! that isn't means "list one directory per call").
	static Decision ClassifyRejection(const string &error_class, const string &message, idx_t sent) {
		Decision decision;
		if (error_class.find("RpcNoSuchMethodException") != string::npos ||
		    error_class.find("UnsupportedOperationException") != string::npos ||
		    message.find("Unknown method getBatchedListing") != string::npos) {
			decision.rejection = Rejection::UNSUPPORTED;
			return decision;
		}
		// FSNamesystem: "Too many source paths (%d > %d)", the second number being dfs.batched.ls.limit.
		static constexpr const char *PREFIX = "Too many source paths (";
		auto at = message.find(PREFIX);
		if (error_class.find("IllegalArgumentException") == string::npos || at == string::npos) {
			return decision;
		}
		const char *pos = message.c_str() + at + std::strlen(PREFIX);
		int64_t reported_sent;
		int64_t limit;
		if (!ParseInteger(pos, reported_sent) || std::strncmp(pos, " > ", 3) != 0) {
			return decision;
		}
		pos += 3;
		if (!ParseInteger(pos, limit) || *pos != ')') {
			return decision;
		}
		if (limit >= 2 && static_cast<uint64_t>(limit) < sent) {
			decision.rejection = Rejection::TOO_MANY_PATHS;
			decision.limit = static_cast<idx_t>(limit);
		} else {
			// A limit of 1 is no batching at all; a negative one, or one that doesn't shrink the batch, can't be
			// trusted (a retry with it would be rejected again, forever).
			decision.rejection = Rejection::UNSUPPORTED;
		}
		return decision;
	}

private:
	//! Parse an optionally negative decimal integer at `pos` (at most 18 digits), advancing past it.
	static bool ParseInteger(const char *&pos, int64_t &result) {
		bool negative = *pos == '-';
		const char *digit = negative ? pos + 1 : pos;
		int64_t value = 0;
		int count = 0;
		for (; *digit >= '0' && *digit <= '9'; digit++) {
			if (++count > 18) {
				return false;
			}
			value = value * 10 + (*digit - '0');
		}
		if (count == 0) {
			return false;
		}
		result = negative ? -value : value;
		pos = digit;
		return true;
	}
};

} // namespace duckdb
