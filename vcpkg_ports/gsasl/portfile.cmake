# Same as vcpkg's gsasl port, minus the configure.ac patch (which only adds an
# option to skip the gsasl command-line tool) and the autoreconf step it needs.
# The release tarball ships a generated configure script, so this builds on CI
# images without autoconf-archive.
vcpkg_download_distfile(ARCHIVE
    URLS
        "https://ftpmirror.gnu.org/gnu/gsasl/gsasl-${VERSION}.tar.gz"
        "https://ftp.gnu.org/gnu/gsasl/gsasl-${VERSION}.tar.gz"
    FILENAME "gsasl-${VERSION}.tar.gz"
    SHA512 62fb4a9383392e4816a036f3e8f408c5161a10723e59f0a8f6df5f72101e0b644787f3b07a71c772628fc4f4050960c842c7500736edacd24313ef654e703bc9
)

vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        --disable-nls
        --disable-gssapi
        --disable-valgrind-tests
)

vcpkg_make_install()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/bin"
    "${CURRENT_PACKAGES_DIR}/debug/bin"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/info"
    "${CURRENT_PACKAGES_DIR}/share/man"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING.LESSER")
