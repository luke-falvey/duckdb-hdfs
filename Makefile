PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hdfs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# C++ unit tests that need no cluster: the glob planner (brace expansion, roots,
# hidden names, `**`), the metadata time conversion, the encryption-zone
# helpers (against NIST vectors) and batched-listing rejection handling. They
# link the DuckDB and vcpkg libraries of the release build, so run `make` first.
# Globs against a real directory tree are tested with SQL on a cluster
# (test/sql/hdfs_glob.test).
BUILD_DIR = build/release
VCPKG_INSTALLED_DIR ?= $(firstword $(filter-out %/vcpkg,$(wildcard $(BUILD_DIR)/vcpkg_installed/*)))
UNIT_PKGS = libcurl libssl libcrypto
UNIT_CXXFLAGS = -std=c++17 -O1 -Wall -Wextra -Isrc/include -Iduckdb/src/include
DUCKDB_STATIC_LIBS = $(BUILD_DIR)/src/libduckdb_static.a $(wildcard $(BUILD_DIR)/third_party/*/libduckdb_*.a)

test_unit: test_glob test_functions test_crypto test_batch

test_glob:
	mkdir -p build/unit_test
	$(CXX) $(UNIT_CXXFLAGS) -Iduckdb/third_party/utf8proc/include src/hdfs_glob.cpp test/cpp/hdfs_glob_test.cpp \
		-Wl,--start-group $(DUCKDB_STATIC_LIBS) -Wl,--end-group -lpthread -ldl -o build/unit_test/hdfs_glob_test
	./build/unit_test/hdfs_glob_test

test_functions:
	mkdir -p build/unit_test
	$(CXX) $(UNIT_CXXFLAGS) -Iduckdb/third_party/fmt/include test/cpp/hdfs_functions_test.cpp \
		-Wl,--start-group $(DUCKDB_STATIC_LIBS) -Wl,--end-group -lpthread -ldl -o build/unit_test/hdfs_functions_test
	./build/unit_test/hdfs_functions_test

test_crypto:
	@test -n "$(VCPKG_INSTALLED_DIR)" || { echo "No vcpkg packages under $(BUILD_DIR); run make first"; exit 1; }
	mkdir -p build/unit_test
	$(CXX) $(UNIT_CXXFLAGS) src/hdfs_crypto.cpp test/cpp/hdfs_crypto_test.cpp \
		-Wl,--start-group $(DUCKDB_STATIC_LIBS) -Wl,--end-group \
		$$(PKG_CONFIG_PATH=$(VCPKG_INSTALLED_DIR)/lib/pkgconfig pkg-config --static --cflags --libs $(UNIT_PKGS)) \
		-lpthread -ldl -o build/unit_test/hdfs_crypto_test
	./build/unit_test/hdfs_crypto_test

test_batch:
	mkdir -p build/unit_test
	$(CXX) $(UNIT_CXXFLAGS) test/cpp/hdfs_batch_test.cpp -o build/unit_test/hdfs_batch_test
	./build/unit_test/hdfs_batch_test

.PHONY: test_unit test_glob test_functions test_crypto test_batch
