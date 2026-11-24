// Unit tests for the helpers behind hdfs_ls / hdfs_stat (src/include/hdfs_functions.hpp). No cluster needed. Build
// and run with `make test_functions`.

#include "hdfs_functions.hpp"

#include <cstdio>
#include <string>

using namespace duckdb;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

//! Whether converting `ms` fails with an IOException that names the path.
bool ThrowsIO(int64_t ms) {
	try {
		HdfsTimestampFromEpochMs(ms, "hdfs://nn/d/f");
	} catch (const IOException &error) {
		return std::string(error.what()).find("hdfs://nn/d/f") != std::string::npos;
	}
	return false;
}

void EpochConversion() {
	CHECK(HdfsTimestampFromEpochMs(0, "u").value == 0);
	CHECK(HdfsTimestampFromEpochMs(1700000000123, "u").value == 1700000000123000);
	// Pre-1970 times are negative, and valid.
	CHECK(HdfsTimestampFromEpochMs(-2, "u").value == -2000);
	CHECK(HdfsTimestampFromEpochMs(NumericLimits<int64_t>::Maximum() / 1000 - 1, "u").IsFinite());
	// Out of range: overflows microseconds, or lands on DuckDB's infinity sentinels.
	CHECK(ThrowsIO(NumericLimits<int64_t>::Maximum()));
	CHECK(ThrowsIO(NumericLimits<int64_t>::Minimum()));
	CHECK(ThrowsIO(NumericLimits<int64_t>::Maximum() / 1000 + 1));
}

} // namespace

void Permissions() {
	CHECK(HdfsPermissionString(0755) == "rwxr-xr-x");
	CHECK(HdfsPermissionString(0640) == "rw-r-----");
	CHECK(HdfsPermissionString(0) == "---------");
	// The sticky bit, as Hadoop's FsPermission shows it.
	CHECK(HdfsPermissionString(01777) == "rwxrwxrwt");
	CHECK(HdfsPermissionString(01776) == "rwxrwxrwT");
	CHECK(HdfsPermissionMode(01777) == 01777);
	CHECK(HdfsPermissionMode(0755) == 0755);
	// Bits HDFS doesn't define are not reported.
	CHECK(HdfsPermissionMode(07777) == 01777);
	CHECK(HdfsPermissionString(0xF3ED) == "rwxr-xr-t");
}

int main() {
	EpochConversion();
	Permissions();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("All function helper tests passed\n");
	return 0;
}
