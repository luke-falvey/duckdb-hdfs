#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class HdfsFileSystem;

//! Register hdfs_ls, hdfs_stat and hdfs_exists. `hdfs` is owned by the database's VirtualFileSystem and outlives
//! the functions.
void RegisterHdfsFunctions(ExtensionLoader &loader, HdfsFileSystem &hdfs);

//! An HDFS time (milliseconds since the epoch, UTC; may be negative) as a timestamp. A value DuckDB can't represent
//! is an IOException naming `url`: bad data from the NameNode, not an internal error.
inline timestamp_t HdfsTimestampFromEpochMs(int64_t ms, const string &url) {
	int64_t micros;
	if (!TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(ms, Interval::MICROS_PER_MSEC, micros) ||
	    !timestamp_t(micros).IsFinite()) {
		throw IOException("HDFS reported an out-of-range time for '%s': %d ms since the epoch", url, ms);
	}
	return timestamp_t(micros);
}

} // namespace duckdb
