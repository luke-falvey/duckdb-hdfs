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

//! The permission bits HDFS has (rwx for user, group and other, plus the sticky bit 01000) of a status's mode.
inline uint16_t HdfsPermissionMode(uint16_t mode) {
	return mode & 01777;
}

//! The permission bits as `hdfs dfs -ls` shows them: "rwxr-xr-x", with the sticky bit as 't' in the last place
//! ('T' if others can't execute), e.g. "rwxrwxrwt" for /tmp.
inline string HdfsPermissionString(uint16_t mode) {
	static const char *const RWX[] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
	auto result = string(RWX[(mode >> 6) & 0x7]) + RWX[(mode >> 3) & 0x7] + RWX[mode & 0x7];
	if (mode & 01000) {
		result.back() = (mode & 01) ? 't' : 'T';
	}
	return result;
}

} // namespace duckdb
