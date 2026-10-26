# Same as vcpkg's krb5 port (Linux/macOS path), but built from the official
# release tarball, which ships a generated configure script: no autoreconf, so
# it builds with any autoconf version (vcpkg's port fails with autoconf 2.73)
# and on CI images without extra autotools packages.
vcpkg_download_distfile(ARCHIVE
    URLS "https://kerberos.org/dist/krb5/1.22/krb5-${VERSION}.tar.gz"
    FILENAME "krb5-${VERSION}.tar.gz"
    SHA512 3237cacfb2019285107991a3211e0d74944c605942ab38a8b4b372703b8f02f5779fa2de80c4e201bd59703d557f37ac346bdc5ea14b986b0a0db23eb422fc6f
)

vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    PATCHES
        static-deps.diff
        define-des-zeroblock.diff
)

vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}/src"
    OPTIONS
        --disable-nls
        --with-tls-impl=no
        --without-keyutils
        --without-libedit
        --without-readline
        "CFLAGS=-fcommon \$CFLAGS"
)
vcpkg_make_install()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin/krb5-config" "${CURRENT_INSTALLED_DIR}" [[$(cd "$(dirname "$0")/../../.."; pwd -P)]])
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/${PORT}/bin/compile_et" "${CURRENT_INSTALLED_DIR}" [[$(cd "$(dirname "$0")/../../.."; pwd -P)]])
if(NOT VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug/bin/krb5-config" "${CURRENT_INSTALLED_DIR}" [[$(cd "$(dirname "$0")/../../../.."; pwd -P)]])
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/${PORT}/debug/bin/compile_et" "${CURRENT_INSTALLED_DIR}" [[$(cd "$(dirname "$0")/../../../.."; pwd -P)]])
endif()

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/debug/var"
    "${CURRENT_PACKAGES_DIR}/share/krb5/cat1"
    "${CURRENT_PACKAGES_DIR}/share/krb5/cat5"
    "${CURRENT_PACKAGES_DIR}/share/krb5/cat7"
    "${CURRENT_PACKAGES_DIR}/share/krb5/cat8"
    "${CURRENT_PACKAGES_DIR}/var"
    "${CURRENT_PACKAGES_DIR}/lib/krb5/"
    "${CURRENT_PACKAGES_DIR}/debug/lib/krb5/"
)
if(VCPKG_BUILD_TYPE)
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/NOTICE")
