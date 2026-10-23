PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hdfs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Unit tests for the glob engine; needs no DuckDB build or cluster.
test_glob:
	mkdir -p build/glob_test
	$(CXX) -std=c++17 -O1 -Wall -Wextra -Isrc/include src/hdfs_glob.cpp test/cpp/hdfs_glob_test.cpp -o build/glob_test/hdfs_glob_test
	./build/glob_test/hdfs_glob_test

.PHONY: test_glob
