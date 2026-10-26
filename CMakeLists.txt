cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME hdfs)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# The native HDFS client (ClickHouse's libhdfs3), built by the overlay port in
# vcpkg_ports/libhdfs3. Only src/hdfs_bridge.cpp includes its headers.
find_package(libhdfs3 CONFIG REQUIRED)
# Encryption zones: AES-CTR (OpenSSL) and the KMS REST client (libcurl).
find_package(OpenSSL REQUIRED)
find_package(CURL CONFIG REQUIRED)

set(EXTENSION_SOURCES
    src/hdfs_extension.cpp src/hdfs_filesystem.cpp src/hdfs_functions.cpp
    src/hdfs_bridge.cpp src/hdfs_crypto.cpp src/hdfs_glob.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The build_*_extension helpers use the plain target_link_libraries signature on
# these targets, so we must match it (no PUBLIC/PRIVATE keyword).
set(HDFS_LIBS libhdfs3::hdfs3 CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${EXTENSION_NAME} ${HDFS_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${HDFS_LIBS})

# Drop the local symbol table from the loadable extension (about a quarter of
# its size: the statically linked DuckDB and libraries carry ~60,000 local
# symbols). Only the entry point is exported, so nothing needs them at run time;
# Debug builds keep them for debuggers.
if(APPLE)
  target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
                      $<$<NOT:$<CONFIG:Debug>>:-Wl,-x>)
elseif(NOT WIN32)
  target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
                      $<$<NOT:$<CONFIG:Debug>>:-Wl,--strip-all>)
endif()

# Install the static extension archive. We intentionally omit EXPORT
# "${DUCKDB_EXPORT_SET}" (used by core extensions for find_package(DuckDB)
# consumers): a community extension distributes the loadable
# `.duckdb_extension`, so the export set is unneeded.
install(
  TARGETS ${EXTENSION_NAME}
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
