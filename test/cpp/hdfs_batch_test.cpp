// Unit tests for batched-listing rejection handling (src/include/hdfs_batch.hpp).
// No cluster needed. Build and run with `make test_batch`.

#include "hdfs_batch.hpp"

#include <cstdio>

using namespace hdfs_batch;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

void TestUnsupported() {
	// A NameNode older than Hadoop 3.3.
	auto d = ClassifyRejection("org.apache.hadoop.ipc.RpcNoSuchMethodException",
	                           "Unknown method getBatchedListing called on "
	                           "org.apache.hadoop.hdfs.protocol.ClientProtocol protocol.");
	CHECK(d.rejection == Rejection::UNSUPPORTED);
	// Router-based federation.
	d = ClassifyRejection("java.lang.UnsupportedOperationException", "Not implemented");
	CHECK(d.rejection == Rejection::UNSUPPORTED);
	// The method name alone is enough, whatever the class.
	d = ClassifyRejection("java.io.IOException", "Unknown method getBatchedListing called on ...");
	CHECK(d.rejection == Rejection::UNSUPPORTED);
}

void TestTooManyPaths() {
	auto d = ClassifyRejection("java.lang.IllegalArgumentException", "Too many source paths (100 > 4)");
	CHECK(d.rejection == Rejection::TOO_MANY_PATHS);
	CHECK(d.limit == 4);
	// A limit of 1 is no batching at all.
	d = ClassifyRejection("java.lang.IllegalArgumentException", "Too many source paths (2 > 1)");
	CHECK(d.rejection == Rejection::UNSUPPORTED);
	// Unparseable: not treated as a batching rejection.
	d = ClassifyRejection("java.lang.IllegalArgumentException", "Too many source paths (lots)");
	CHECK(d.rejection == Rejection::NONE);
	// Another IllegalArgumentException (e.g. a stale resume key).
	d = ClassifyRejection("java.lang.IllegalArgumentException", "");
	CHECK(d.rejection == Rejection::NONE);
}

void TestOrdinaryFailures() {
	CHECK(ClassifyRejection("org.apache.hadoop.security.AccessControlException", "Permission denied").rejection ==
	      Rejection::NONE);
	CHECK(ClassifyRejection("java.io.FileNotFoundException", "Path /x does not exist").rejection == Rejection::NONE);
	CHECK(ClassifyRejection("org.apache.hadoop.ipc.StandbyException", "Operation category READ is not supported")
	          .rejection == Rejection::NONE);
	CHECK(ClassifyRejection("", "").rejection == Rejection::NONE);
}

} // namespace

int main() {
	TestUnsupported();
	TestTooManyPaths();
	TestOrdinaryFailures();
	if (failures) {
		std::fprintf(stderr, "%d batch test(s) failed\n", failures);
		return 1;
	}
	std::printf("All batch tests passed\n");
	return 0;
}
