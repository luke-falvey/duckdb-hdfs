# ClickHouse's maintained fork of libhdfs3. Its own build system is the old
# HAWQ one (forces -msse4.2, needs yasm on x86), so we build it with a small
# CMakeLists.txt of our own, modelled on ClickHouse's contrib/libhdfs3-cmake.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ClickHouse/libhdfs3
    REF 28a9868bc15749d899669e117e0c6eb9f5350923
    SHA512 453ed3216896e451241e70839de83e5ff36f041448bcbd55e1ca02d14c84ef1c259f52a55d6d85dcf392a227486021a18b9ed1bd5d39071f92dca36491ce7ce4
    HEAD_REF master
    PATCHES
        protobuf-uint8.patch
        encryption-info.patch
        config-update-merges.patch
        rename2.patch
        kerberos-no-stderr.patch
        wire-encryption.patch
        fewer-round-trips.patch
        peer-cache.patch
        batched-listing.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/cmake/CMakeLists.txt" DESTINATION "${SOURCE_PATH}/vcpkg-build")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/cmake/libhdfs3-config.cmake.in" DESTINATION "${SOURCE_PATH}/vcpkg-build")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/absl_log_undef.h" DESTINATION "${SOURCE_PATH}/vcpkg-build")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/vcpkg-build"
    OPTIONS
        "-DHDFS3_SOURCE_ROOT=${SOURCE_PATH}"
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME libhdfs3 CONFIG_PATH share/libhdfs3)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
