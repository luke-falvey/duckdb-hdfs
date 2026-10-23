# Same as vcpkg's gsasl port, minus the configure.ac patch (which only adds an
# option to skip the gsasl command-line tool) and the autoreconf step it needs,
# and with the GSSAPI mechanism (Kerberos) enabled, using MIT krb5.
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

# GSSAPI (Kerberos) via the krb5 port, found through its krb5-config. krb5 is
# static, so configure's link tests also need its private dependencies;
# otherwise they fail, configure decides functions like gss_oid_equal are
# missing, and gsasl's replacements then clash with krb5's declarations.
set(ENV{KRB5_CONFIG} "${CURRENT_INSTALLED_DIR}/tools/krb5/bin/krb5-config")
vcpkg_find_acquire_program(PKGCONFIG)
set(ENV{PKG_CONFIG_PATH} "${CURRENT_INSTALLED_DIR}/lib/pkgconfig")
execute_process(
    COMMAND "${PKGCONFIG}" --static --libs mit-krb5-gssapi
    OUTPUT_VARIABLE KRB5_STATIC_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        --disable-nls
        --with-gssapi-impl=mit
        --disable-valgrind-tests
        "LIBS=${KRB5_STATIC_LIBS} \$LIBS"
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
