// Unit tests for batched-listing rejection handling (src/include/hdfs_batch.hpp).
// No cluster needed. Build and run with `make test_batch`.

#include "hdfs_batch.hpp"

#include <cstdio>

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

void TestUnsupported() {
	// A NameNode older than Hadoop 3.3.
	auto d = HdfsBatchedListing::ClassifyRejection("org.apache.hadoop.ipc.RpcNoSuchMethodException",
	                                               "Unknown method getBatchedListing called on "
	                                               "org.apache.hadoop.hdfs.protocol.ClientProtocol protocol.",
	                                               100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	// Router-based federation.
	d = HdfsBatchedListing::ClassifyRejection("java.lang.UnsupportedOperationException", "Not implemented", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	// The method name alone is enough, whatever the class.
	d = HdfsBatchedListing::ClassifyRejection("java.io.IOException", "Unknown method getBatchedListing called on ...",
	                                          100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
}

void TestTooManyPaths() {
	auto d = HdfsBatchedListing::ClassifyRejection("java.lang.IllegalArgumentException",
	                                               "Too many source paths (100 > 4)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::TOO_MANY_PATHS);
	CHECK(d.limit == 4);
	// A limit of 1 is no batching at all.
	d = HdfsBatchedListing::ClassifyRejection("java.lang.IllegalArgumentException", "Too many source paths (2 > 1)",
	                                          100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	// Unparseable: not treated as a batching rejection.
	d = HdfsBatchedListing::ClassifyRejection("java.lang.IllegalArgumentException", "Too many source paths (lots)",
	                                          100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::NONE);
	// Another IllegalArgumentException (e.g. a stale resume key).
	d = HdfsBatchedListing::ClassifyRejection("java.lang.IllegalArgumentException", "", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::NONE);
}

// A limit that wouldn't shrink the batch would requeue the same call forever; a nonsensical one isn't trusted.
void TestUntrustedLimits() {
	const string cls = "java.lang.IllegalArgumentException";
	auto d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 99)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::TOO_MANY_PATHS);
	CHECK(d.limit == 99);
	// Equal to, or larger than, what was sent: no progress.
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 100)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 500)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	// The count in the message is ignored: what matters is how many paths this call sent.
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 8)", 8);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 8)", 9);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::TOO_MANY_PATHS);
	CHECK(d.limit == 8);
	// Negative (which %lu would have wrapped to a huge number) and zero.
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > -4)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 0)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::UNSUPPORTED);
	// Overlong or malformed numbers: not understood, so an ordinary failure.
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 99999999999999999999)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::NONE);
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > 4x)", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::NONE);
	d = HdfsBatchedListing::ClassifyRejection(cls, "Too many source paths (100 > )", 100);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::NONE);
	// The message may carry more text around the numbers.
	d = HdfsBatchedListing::ClassifyRejection(
	    cls, "java.lang.IllegalArgumentException: Too many source paths (50 > 10) ok", 50);
	CHECK(d.rejection == HdfsBatchedListing::Rejection::TOO_MANY_PATHS);
	CHECK(d.limit == 10);
}

void TestOrdinaryFailures() {
	CHECK(HdfsBatchedListing::ClassifyRejection("org.apache.hadoop.security.AccessControlException",
	                                            "Permission denied", 100)
	          .rejection == HdfsBatchedListing::Rejection::NONE);
	CHECK(HdfsBatchedListing::ClassifyRejection("java.io.FileNotFoundException", "Path /x does not exist", 100)
	          .rejection == HdfsBatchedListing::Rejection::NONE);
	CHECK(HdfsBatchedListing::ClassifyRejection("org.apache.hadoop.ipc.StandbyException",
	                                            "Operation category READ is not supported", 100)
	          .rejection == HdfsBatchedListing::Rejection::NONE);
	CHECK(HdfsBatchedListing::ClassifyRejection("", "", 100).rejection == HdfsBatchedListing::Rejection::NONE);
}

} // namespace

int main() {
	TestUnsupported();
	TestTooManyPaths();
	TestUntrustedLimits();
	TestOrdinaryFailures();
	if (failures) {
		std::fprintf(stderr, "%d batch test(s) failed\n", failures);
		return 1;
	}
	std::printf("All batch tests passed\n");
	return 0;
}
