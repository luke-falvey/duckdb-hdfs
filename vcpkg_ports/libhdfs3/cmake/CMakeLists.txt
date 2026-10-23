# Builds ClickHouse/libhdfs3 as a static library for the duckdb-hdfs extension.
# Modelled on ClickHouse's contrib/libhdfs3-cmake: Kerberos and ISA-L are off,
# and the SSE4.2 assembly CRC is not used (no yasm needed), so the same build
# works on x86-64 and arm64.
cmake_minimum_required(VERSION 3.16)
project(libhdfs3 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT HDFS3_SOURCE_ROOT)
  message(FATAL_ERROR "HDFS3_SOURCE_ROOT must point at the libhdfs3 checkout")
endif()
set(SRC "${HDFS3_SOURCE_ROOT}/src")

find_package(Protobuf CONFIG REQUIRED)
find_package(LibXml2 CONFIG REQUIRED)
find_package(boost_uuid CONFIG REQUIRED)
find_package(boost_crc CONFIG REQUIRED)
find_package(boost_core CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSASL REQUIRED IMPORTED_TARGET libgsasl)

# platform.h
if(APPLE)
  set(OS_MACOSX 1)
else()
  set(OS_LINUX 1)
endif()
set(HAVE_DLADDR 1)
set(HAVE_STEADY_CLOCK 1)
set(HAVE_NESTED_EXCEPTION 1)
set(HAVE_STD_CHRONO 1)
set(HAVE_STD_ATOMIC 1)
set(WITH_KERBEROS 0)
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
  "#include <string.h>
   int main() { char b[8]; int r = strerror_r(0, b, sizeof(b)); return r; }"
  STRERROR_R_RETURN_INT)
set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${SRC}/platform.h.in" "${GENERATED}/platform.h")

# Protobuf sources, generated flat into GENERATED (the sources include them as
# e.g. "RpcHeader.pb.h").
set(PROTO_FILES
    ClientDatanodeProtocol.proto
    ClientNamenodeProtocol.proto
    IpcConnectionContext.proto
    ProtobufRpcEngine.proto
    RpcHeader.proto
    Security.proto
    datatransfer.proto
    hdfs.proto)
list(TRANSFORM PROTO_FILES PREPEND "${SRC}/proto/")

file(GLOB_RECURSE HDFS3_SOURCES "${SRC}/client/*.cpp" "${SRC}/common/*.cpp" "${SRC}/network/*.cpp"
     "${SRC}/rpc/*.cpp" "${SRC}/server/*.cpp")
list(FILTER HDFS3_SOURCES EXCLUDE REGEX "/common/perf_checksum\\.cpp$")
list(FILTER HDFS3_SOURCES EXCLUDE REGEX "/common/IntelAsmCrc32c\\.cpp$")

add_library(hdfs3 STATIC ${HDFS3_SOURCES} ${PROTO_FILES})
set_target_properties(hdfs3 PROPERTIES OUTPUT_NAME hdfs3)
protobuf_generate(
  TARGET hdfs3
  LANGUAGE cpp
  IMPORT_DIRS "${SRC}/proto"
  PROTOC_OUT_DIR "${GENERATED}")

target_compile_definitions(
  hdfs3 PRIVATE __STDC_FORMAT_MACROS _GNU_SOURCE _GLIBCXX_USE_NANOSLEEP HAVE_NANOSLEEP
                BOOST_UUID_RANDOM_PROVIDER_FORCE_POSIX=1)
if(STRERROR_R_RETURN_INT)
  target_compile_definitions(hdfs3 PRIVATE STRERROR_R_RETURN_INT)
endif()
target_compile_options(hdfs3 PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/absl_log_undef.h"
                                     -fno-strict-aliasing -w)
target_include_directories(
  hdfs3
  PRIVATE "${SRC}" "${SRC}/common" "${SRC}/client" "${GENERATED}"
  INTERFACE $<INSTALL_INTERFACE:include/libhdfs3> $<INSTALL_INTERFACE:include/libhdfs3/client>
            $<INSTALL_INTERFACE:include/libhdfs3/common>)
target_link_libraries(
  hdfs3
  PUBLIC protobuf::libprotobuf LibXml2::LibXml2 PkgConfig::GSASL
  # Header-only, so only needed while building this library.
  PRIVATE $<BUILD_INTERFACE:Boost::uuid> $<BUILD_INTERFACE:Boost::crc> $<BUILD_INTERFACE:Boost::core>)
if(NOT APPLE)
  target_link_libraries(hdfs3 PUBLIC pthread)
endif()

include(GNUInstallDirs)
install(
  TARGETS hdfs3
  EXPORT libhdfs3-targets
  ARCHIVE DESTINATION lib)
install(
  DIRECTORY "${SRC}/"
  DESTINATION include/libhdfs3
  FILES_MATCHING
  PATTERN "*.h")
install(
  DIRECTORY "${GENERATED}/"
  DESTINATION include/libhdfs3
  FILES_MATCHING
  PATTERN "*.h")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/absl_log_undef.h" DESTINATION include/libhdfs3)
install(
  EXPORT libhdfs3-targets
  NAMESPACE libhdfs3::
  DESTINATION share/libhdfs3)
include(CMakePackageConfigHelpers)
configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/libhdfs3-config.cmake.in"
                              "${CMAKE_CURRENT_BINARY_DIR}/libhdfs3-config.cmake" INSTALL_DESTINATION share/libhdfs3)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libhdfs3-config.cmake" DESTINATION share/libhdfs3)
