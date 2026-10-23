PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=hdfs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Unit tests that need no DuckDB build or cluster: the glob engine, and the
# encryption-zone helpers (which use the OpenSSL/libcurl that vcpkg installs
# for the main build, so run `make` first).
VCPKG_INSTALLED_DIR ?= $(firstword $(filter-out %/vcpkg,$(wildcard build/release/vcpkg_installed/*)))
UNIT_PKGS = libcurl libssl libcrypto

test_unit: test_glob test_crypto

test_glob:
	mkdir -p build/unit_test
	$(CXX) -std=c++17 -O1 -Wall -Wextra -Isrc/include src/hdfs_glob.cpp test/cpp/hdfs_glob_test.cpp -o build/unit_test/hdfs_glob_test
	./build/unit_test/hdfs_glob_test

test_crypto:
	@test -n "$(VCPKG_INSTALLED_DIR)" || { echo "No vcpkg packages under build/release; run make first"; exit 1; }
	mkdir -p build/unit_test
	$(CXX) -std=c++17 -O1 -Wall -Wextra -Isrc/include src/hdfs_crypto.cpp test/cpp/hdfs_crypto_test.cpp \
		$$(PKG_CONFIG_PATH=$(VCPKG_INSTALLED_DIR)/lib/pkgconfig pkg-config --static --cflags --libs $(UNIT_PKGS)) \
		-o build/unit_test/hdfs_crypto_test
	./build/unit_test/hdfs_crypto_test

.PHONY: test_unit test_glob test_crypto
