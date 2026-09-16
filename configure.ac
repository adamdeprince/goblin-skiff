#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.61])
AC_INIT([mosh], [1.4.0], [mosh-devel@mit.edu])
AC_SUBST([GOBLIN_VERSION], [m4_normalize(m4_include([GOBLIN_VERSION]))])
AC_DEFINE_UNQUOTED([GOBLIN_VERSION], ["$GOBLIN_VERSION"], [Goblin Mosh release version.])
AM_INIT_AUTOMAKE([foreign std-options -Wall -Werror -Wno-portability])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_CONFIG_SRCDIR([src/frontend/mosh-client.cc])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([src/include/config.h])
AC_LANG([C++])
AX_CODE_COVERAGE

# Checks for programs.
AC_PROG_CC([cc gcc clang])
AC_PROG_CXX([c++ g++ clang++])
AC_PROG_RANLIB
AC_PATH_PROG([PROTOC], [protoc], [])
AS_IF([test x"$PROTOC" = x],
  [AC_MSG_ERROR([cannot find protoc, the Protocol Buffers compiler])])

# automake 1.12 seems to require this, but automake 1.11 doesn't recognize it
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Protobuf transitively requires at least C++14, get ahead of the
# curve and require at least C++17.
AX_CXX_COMPILE_STDCXX([17])

WARNING_CXXFLAGS=""
PICKY_CXXFLAGS=""
DISTCHECK_CXXFLAGS=""
AC_ARG_ENABLE([compile-warnings],
  [AS_HELP_STRING([--enable-compile-warnings@<:@=no/yes/maximum/error/distcheck@:>@],
     [Turn on compiler warnings])],
  [case "$enableval" in
     no)
       ;;
     '' | yes)
       WARNING_CXXFLAGS="-Wall"
       ;;
     maximum)
       WARNING_CXXFLAGS="-Wall"
       PICKY_CXXFLAGS="-Wextra -Wno-long-long -Weffc++ -Wmissing-declarations"
       ;;
     error)
       # remove -Wno-c++17-extensions once protocolbuffers/protobuf#9181 is
       # resolved
       # remove -Wno-unused-parameter once
       # protocolbuffers/protobuf#10357 is resolved       
       WARNING_CXXFLAGS="-Wall -Werror -Wno-c++17-extensions"
       PICKY_CXXFLAGS="-Wextra -Wno-long-long -Weffc++ -Wmissing-declarations -Wno-unused-parameter"
       ;;
     distcheck)
       WARNING_CXXFLAGS="-Wall -Werror"
       PICKY_CXXFLAGS="-Wextra -Wno-long-long -Weffc++ -Wmissing-declarations"
       AX_CHECK_COMPILE_FLAG([-Wno-error=unused-parameter],
         [PICKY_CXXFLAGS="$PICKY_CXXFLAGS -Wno-error=unused-parameter"], [], [-Werror])
       AX_CHECK_COMPILE_FLAG([-Wno-error=c++11-extensions],
         [PICKY_CXXFLAGS="$PICKY_CXXFLAGS -Wno-error=c++11-extensions"], [], [-Werror])
       AX_CHECK_COMPILE_FLAG([-Wno-error=deprecated-declarations],
         [PICKY_CXXFLAGS="$PICKY_CXXFLAGS -Wno-error=deprecated-declarations"], [], [-Werror])
       AX_CHECK_COMPILE_FLAG([-Wno-error=nested-anon-types],
         [PICKY_CXXFLAGS="$PICKY_CXXFLAGS -Wno-error=nested-anon-types"], [], [-Werror])
       ;;
     *)
       AC_MSG_ERROR([Unknown argument '$enableval' to --enable-compile-warnings])
       ;;
   esac],
  [WARNING_CXXFLAGS="-Wall"])
AC_SUBST([WARNING_CXXFLAGS])
AC_SUBST([PICKY_CXXFLAGS])

# Check for fuzzing support before the flag wrapper, because if
# requested and missing the functionality is entirely nonexistent.
MISC_CXXFLAGS=""
AC_ARG_ENABLE([fuzzing],
  [AS_HELP_STRING([--enable-fuzzing],
    [Enable compiler and linker options to enable fuzz testing @<:@no/yes/libfuzzer@:>@])],
  [fuzzing="$enableval"
   case "$enableval" in
    no)
      ;;
    '' | yes | libfuzzer)
      AX_CHECK_COMPILE_FLAG([-fsanitize=fuzzer],
        [FUZZING_CFLAGS="$FUZZING_CFLAGS -fsanitize=fuzzer"], [
        AC_MSG_ERROR([Fuzzing requested, but compiler support not present])], [-Werror])
      ;;
    *)
     AC_MSG_ERROR(["Unknown argument '$enableval' to --enable-fuzzing])
     ;;
  esac],
  [fuzzing="no"])
AC_SUBST([FUZZING_CFLAGS])
AC_SUBST([MISC_CXXFLAGS])
AM_CONDITIONAL([ENABLE_FUZZING], [test x"$fuzzing" != xno])

AC_ARG_ENABLE([asan],
  [AS_HELP_STRING([--enable-asan],
    [Enable compiler and linker options to enable AddressSanitizer @<:@no@:>@])],
  [asan="$enableval"],
  [asan="no"])

AS_IF([test x"$asan" != x"no"], [
  AX_CHECK_COMPILE_FLAG([-fsanitize=address,leak],
  [MISC_CXXFLAGS="$MISC_CXXFLAGS -fsanitize=address,leak"], [
  AC_MSG_ERROR([ASAN requested, but compiler support not present])], [-Werror])
])
AC_SUBST([MISC_CXXFLAGS])

# We want to check for compiler flag support, but there is no way to make
# clang's "argument unused" warning fatal.  So we invoke the compiler through a
# wrapper script that greps for this message.

saved_CXX="$CXX"
saved_LD="$LD"
flag_wrap="$srcdir/scripts/wrap-compiler-for-flag-check"
CXX="$flag_wrap $CXX"
LD="$flag_wrap $LD"

AC_DEFUN([check_cxx_flag],
  [AX_CHECK_COMPILE_FLAG([$1], [$2], [$3], [-Werror $4])])
AC_DEFUN([check_link_flag],
  [AX_CHECK_LINK_FLAG([$1], [$2], [$3], [-Werror $4])])

AC_ARG_ENABLE([hardening],
  [AS_HELP_STRING([--enable-hardening],
    [Enable compiler and linker options to frustrate memory corruption exploits @<:@yes@:>@])],
  [hardening="$enableval"],
  [hardening="yes"])

HARDEN_CFLAGS=""
HARDEN_LDFLAGS=""
AS_IF([test x"$hardening" != x"no"], [
  check_cxx_flag([-fno-strict-overflow], [HARDEN_CFLAGS="$HARDEN_CFLAGS -fno-strict-overflow"])

  # This one will likely succeed, even on platforms where it does nothing.
  check_cxx_flag([-D_FORTIFY_SOURCE=2], [HARDEN_CFLAGS="$HARDEN_CFLAGS -D_FORTIFY_SOURCE=2"])

  check_link_flag([-fstack-protector-all],
   [HARDEN_CFLAGS="$HARDEN_CFLAGS -fstack-protector-all"
    check_cxx_flag([-Wstack-protector], [HARDEN_CFLAGS="$HARDEN_CFLAGS -Wstack-protector"],
      [], [-fstack-protector-all])
    check_cxx_flag([--param ssp-buffer-size=1], [HARDEN_CFLAGS="$HARDEN_CFLAGS --param ssp-buffer-size=1"],
      [], [-fstack-protector-all])])

  check_cxx_flag([-fPIE],
   [check_link_flag([-fPIE -pie],
     [HARDEN_CFLAGS="$HARDEN_CFLAGS -fPIE"
      HARDEN_LDFLAGS="$HARDEN_LDFLAGS -pie"],
     [check_link_flag([-fPIE -Wl,-pie],
       [HARDEN_CFLAGS="$HARDEN_CFLAGS -fPIE"
        HARDEN_LDFLAGS="$HARDEN_LDFLAGS -Wl,-pie"])])])

  check_link_flag([-Wl,-z,relro],
   [HARDEN_LDFLAGS="$HARDEN_LDFLAGS -Wl,-z,relro"
    check_link_flag([-Wl,-z,now], [HARDEN_LDFLAGS="$HARDEN_LDFLAGS -Wl,-z,now"])])])
AC_SUBST([HARDEN_CFLAGS])
AC_SUBST([HARDEN_LDFLAGS])

# Also check for a few non-hardening-related flags.
AX_CHECK_COMPILE_FLAG([-fno-default-inline],
  [MISC_CXXFLAGS="$MISC_CXXFLAGS -fno-default-inline"], [], [-Werror])
AX_CHECK_COMPILE_FLAG([-pipe],
  [MISC_CXXFLAGS="$MISC_CXXFLAGS -pipe"], [], [-Werror])
AC_SUBST([MISC_CXXFLAGS])

# End of flag tests.
CXX="$saved_CXX"
LD="$saved_LD"

AC_ARG_ENABLE([client],
  [AS_HELP_STRING([--enable-client], [Build the goblin-mosh-client program @<:@yes@:>@])],
  [build_client="$enableval"],
  [build_client="yes"])
AM_CONDITIONAL([BUILD_CLIENT], [test x"$build_client" != xno])

AC_ARG_ENABLE([server],
  [AS_HELP_STRING([--enable-server], [Build the goblin-mosh-server program @<:@yes@:>@])],
  [build_server="$enableval"],
  [build_server="yes"])
AM_CONDITIONAL([BUILD_SERVER], [test x"$build_server" != xno])

AC_ARG_ENABLE([examples],
  [AS_HELP_STRING([--enable-examples], [Build the miscellaneous programs in src/examples @<:@no@:>@])],
  [build_examples="$enableval"],
  [build_examples="no"])
AM_CONDITIONAL([BUILD_EXAMPLES], [test x"$build_examples" != xno])

AC_ARG_ENABLE([ufw],
  [AS_HELP_STRING([--enable-ufw], [Install firewall profile for ufw (Uncomplicated Firewall) @<:@no@:>@])],
  [install_ufw="$enableval"],
  [install_ufw="no"])
AM_CONDITIONAL([INSTALL_UFW], [test x"$install_ufw" != xno])

AC_ARG_ENABLE([completion],
  [AS_HELP_STRING([--enable-completion], [Install bash_completion rule @<:@no@:>@])],
  [install_completion="$enableval"],
  [install_completion="no"])
AM_CONDITIONAL([INSTALL_COMPLETION], [test x"$install_completion" != xno])

AC_ARG_ENABLE([syslog],
  [AS_HELP_STRING([--enable-syslog], [Enable connection logging in mosh-server @<:@no@:>@])],
  [enable_syslog="$enableval"],
  [enable_syslog="no"])
AS_IF([test x"$enable_syslog" != xno],
  [AC_CHECK_HEADERS([syslog.h],
    [AC_DEFINE([HAVE_SYSLOG], [1], [Define if syslog is available.])],
    [AS_IF([test x"$enable_syslog" = xcheck],
      [AC_MSG_WARN([Unable to find syslog.h.])],
      [AC_MSG_ERROR([--enable-syslog was given but syslog.h was not found.])])])])

# Checks for libraries.
AC_ARG_ENABLE([static-libraries],
  [AS_HELP_STRING([--enable-static-libraries], [Enable all static linking options below @<:@no@:>@])])

AC_ARG_ENABLE([static-libstdc++],
  [AS_HELP_STRING([--enable-static-libstdc++], [Link libstdc++ statically @<:@no@:>@])],
  [], [enable_static_libstdc__="$enable_static_libraries"])
AS_IF([test "$enable_static_libstdc__" = yes],
  [LDFLAGS="$LDFLAGS -static-libstdc++"])

AC_ARG_ENABLE([static-libgcc],
  [AS_HELP_STRING([--enable-static-libgcc], [Link libgcc statically @<:@no@:>@])],
  [], [enable_static_libgcc="$enable_static_libraries"])
AS_IF([test "$enable_static_libgcc" = yes],
  [LDFLAGS="$LDFLAGS -static-libgcc"])

AC_ARG_WITH([utempter],
  [AS_HELP_STRING([--with-utempter], [write utmp entries using libutempter @<:@check@:>@])],
  [with_utempter="$withval"],
  [with_utempter="check"])
AC_ARG_ENABLE([static-utempter],
  [AS_HELP_STRING([--enable-static-utempter], [Link utempter statically @<:@no@:>@])],
  [], [enable_static_utempter="$enable_static_libraries"])
AS_IF([test x"$with_utempter" != xno],
  [AC_CHECK_LIB([utempter], [utempter_remove_record],
    [UTEMPTER_LIBS=-lutempter
     AS_IF([test "$enable_static_utempter" = yes],
       [UTEMPTER_LIBS="-Wl,-Bstatic $UTEMPTER_LIBS -Wl,-Bdynamic"])
     LIBS="$UTEMPTER_LIBS $LIBS"
     AC_DEFINE([HAVE_UTEMPTER], [1], [Define if libutempter is available.])],
    [AS_IF([test x"$with_utempter" = xcheck],
      [AC_MSG_WARN([Unable to find libutempter; utmp entries will not be made.])],
      [AC_MSG_ERROR([--with-utempter was given but libutempter was not found.])])])])

AC_ARG_ENABLE([static-zlib],
  [AS_HELP_STRING([--enable-static-zlib], [Link zlib statically @<:@no@:>@])],
  [], [enable_static_zlib="$enable_static_libraries"])
AC_CHECK_LIB([z], [compress],
  [ZLIB_LIBS=-lz
   AS_IF([test "$enable_static_zlib" = yes],
     [ZLIB_LIBS="-Wl,-Bstatic $ZLIB_LIBS -Wl,-Bdynamic"])
   LIBS="$ZLIB_LIBS $LIBS"],
  [AC_MSG_ERROR([Unable to find zlib.])])

# Kitty graphics use WebP or lossless palette/DjVu in terminal state. PNG is
# required to accept the compact image format mandated by the Kitty protocol.
PKG_CHECK_MODULES([WEBP], [libwebp], [],
  [AC_MSG_ERROR([Unable to find libwebp, required for Kitty graphics state.])])
PKG_CHECK_MODULES([PNG], [libpng], [],
  [AC_MSG_ERROR([Unable to find libpng, required for Kitty PNG input.])])
PKG_CHECK_MODULES([DJVU], [ddjvuapi >= 3.5.28], [],
  [AC_MSG_ERROR([Unable to find DjVuLibre, required for palette image transport.])])
# DjVuLibre exposes a stable decoding API; its JB2 encoder is the cjb2 tool.
AC_PATH_PROG([CJB2], [cjb2], [])
AS_IF([test x"$CJB2" = x],
  [AC_MSG_ERROR([Unable to find cjb2. Install the DjVuLibre command-line tools.])])
AC_DEFINE_UNQUOTED([CJB2_PATH], ["$CJB2"], [Path to the DjVuLibre lossless encoder.])
CPPFLAGS="$CPPFLAGS $WEBP_CFLAGS $PNG_CFLAGS $DJVU_CFLAGS"
LIBS="$WEBP_LIBS $PNG_LIBS $DJVU_LIBS $LIBS"

AC_ARG_WITH([zstd],
  [AS_HELP_STRING([--with-zstd], [Enable zstd compression for moshcp and transport state @<:@check@:>@])],
  [with_zstd="$withval"],
  [with_zstd="check"])
have_zstd=no
AS_IF([test x"$with_zstd" != xno], [
  PKG_CHECK_MODULES([ZSTD], [libzstd],
    [have_zstd=yes],
    [AC_CHECK_HEADERS([zstd.h],
      [AC_CHECK_LIB([zstd], [ZSTD_compress],
        [ZSTD_LIBS=-lzstd
         have_zstd=yes],
        [:])],
      [:])])
  AS_IF([test x"$with_zstd" != xcheck && test x"$have_zstd" != xyes],
    [AC_MSG_ERROR([--with-zstd was given, but libzstd could not be found])])
])
AS_IF([test x"$have_zstd" = xyes],
  [AC_DEFINE([HAVE_ZSTD], [1], [Define if libzstd is available.])])
AM_CONDITIONAL([HAVE_ZSTD], [test x"$have_zstd" = xyes])
AC_SUBST([ZSTD_CFLAGS])
AC_SUBST([ZSTD_LIBS])

AC_ARG_WITH([librsync],
  [AS_HELP_STRING([--with-librsync], [Enable file-menu delta transfers with librsync and OpenSSL 3 @<:@check@:>@])],
  [with_librsync="$withval"], [with_librsync=check])
AS_CASE([$with_librsync], [yes|no|check], [], [AC_MSG_ERROR([--with-librsync accepts yes or no])])
have_file_sync=no
AS_IF([test x"$with_librsync" != xno], [
  PKG_CHECK_MODULES([FILECRYPTO], [libcrypto >= 3.0], [
    AC_CHECK_HEADERS([librsync.h], [
      AC_CHECK_LIB([rsync], [rs_sig_args],
        [LIBRSYNC_LIBS=-lrsync
         have_file_sync=yes], [:])
    ], [:])
  ], [:])
  AS_IF([test x"$with_librsync" = xyes && test x"$have_file_sync" != xyes],
    [AC_MSG_ERROR([--with-librsync requires librsync 2.3+ and OpenSSL 3 development files])])
])
AS_IF([test x"$have_file_sync" = xyes],
  [AC_DEFINE([HAVE_FILE_SYNC], [1], [Define if librsync file-menu transfers are available.])])
AC_SUBST([LIBRSYNC_LIBS])

RAPTORQ_CPPFLAGS=""
RAPTORQ_CXXFLAGS=""
AC_ARG_WITH([libraptorq],
  [AS_HELP_STRING([--with-libraptorq@<:@=DIR@:>@],
    [Opt in to RaptorQ FEC for custom builds only; review patent terms @<:@no@:>@])],
  [with_libraptorq="$withval"],
  [with_libraptorq="no"])
AS_IF([test x"$with_libraptorq" = xcheck || test x"$with_libraptorq" = x],
  [AC_MSG_ERROR([RaptorQ auto-detection is disabled; use --with-libraptorq or --with-libraptorq=DIR for an explicit custom build])])
have_libraptorq=no
AS_IF([test x"$with_libraptorq" != xno], [
  AC_MSG_WARN([RaptorQ is for custom builds only, not prebuilt distribution artifacts. Review its patent terms in THIRD_PARTY.md.])
  AS_IF([test x"$with_libraptorq" != xyes], [
    RAPTORQ_CPPFLAGS="-I$with_libraptorq/src -I$with_libraptorq/external/eigen3"
  ])

  old_CPPFLAGS="$CPPFLAGS"
  old_CXXFLAGS="$CXXFLAGS"
  CPPFLAGS="$CPPFLAGS $RAPTORQ_CPPFLAGS"
  AC_MSG_CHECKING([for libRaptorQ RFC6330 header])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#if !defined(RQ_LITTLE_ENDIAN) && !defined(RQ_BIG_ENDIAN)
# if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define RQ_BIG_ENDIAN
# else
#  define RQ_LITTLE_ENDIAN
# endif
#endif
#include <RaptorQ/RFC6330_v1_hdr.hpp>
    ]], [[]])],
    [have_libraptorq=yes
     AC_MSG_RESULT([yes])],
    [CXXFLAGS="$CXXFLAGS -fpermissive"
     AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#if !defined(RQ_LITTLE_ENDIAN) && !defined(RQ_BIG_ENDIAN)
# if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define RQ_BIG_ENDIAN
# else
#  define RQ_LITTLE_ENDIAN
# endif
#endif
#include <RaptorQ/RFC6330_v1_hdr.hpp>
      ]], [[]])],
      [have_libraptorq=yes
       RAPTORQ_CXXFLAGS="-fpermissive"
       AC_MSG_RESULT([yes, with -fpermissive])],
      [AC_MSG_RESULT([no])])])
  CPPFLAGS="$old_CPPFLAGS"
  CXXFLAGS="$old_CXXFLAGS"

  AS_IF([test x"$have_libraptorq" != xyes],
    [AC_MSG_ERROR([--with-libraptorq was given, but RaptorQ/RFC6330_v1_hdr.hpp could not be compiled])])
])
AS_IF([test x"$have_libraptorq" = xyes],
  [AC_DEFINE([HAVE_LIBRAPTORQ], [1], [Define if libRaptorQ RFC6330 headers are available.])])
AM_CONDITIONAL([HAVE_LIBRAPTORQ], [test x"$have_libraptorq" = xyes])
AC_SUBST([RAPTORQ_CPPFLAGS])
AC_SUBST([RAPTORQ_CXXFLAGS])

AC_SEARCH_LIBS([socket], [socket network])
AC_SEARCH_LIBS([inet_addr], [nsl])

AC_SEARCH_LIBS([clock_gettime], [rt])

# Checks for header files.
AC_CHECK_HEADERS(m4_normalize([
  fcntl.h
  langinfo.h
  limits.h
  locale.h
  netdb.h
  netinet/in.h
  stddef.h
  stdint.h
  inttypes.h
  stdlib.h
  string.h
  sys/ioctl.h
  sys/random.h
  sys/resource.h
  sys/socket.h
  sys/stat.h
  sys/time.h
  termios.h
  unistd.h
  wchar.h
  wctype.h
  ]), [], [AC_MSG_ERROR([Missing required header file.])])

AC_CHECK_HEADERS([pty.h util.h libutil.h paths.h])
AC_CHECK_HEADERS([endian.h sys/endian.h])
AC_CHECK_HEADERS([utmpx.h])
AC_CHECK_HEADERS([termio.h])
AC_CHECK_HEADERS([sys/uio.h])
AC_CHECK_HEADERS([memory tr1/memory])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_TYPE_INT64_T
AC_TYPE_PID_T
AC_C_RESTRICT
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T
AC_TYPE_UINTPTR_T

# Checks for library functions.
AC_CHECK_FUNCS(m4_normalize([
  getentropy
  getrandom
  gettimeofday
  posix_memalign
  cfmakeraw
  pselect
  pledge
  ]))

# Start by trying to find the needed tinfo parts by pkg-config
PKG_CHECK_MODULES([TINFO], [tinfo],
  [AC_DEFINE([HAVE_CURSES_H], [1], [Define to 1 if <curses.h> is present])],
  [PKG_CHECK_MODULES([TINFO], [ncurses],
    [AC_DEFINE([HAVE_CURSES_H], [1], [Define to 1 if <curses.h> is present])],
    [AX_CHECK_LIBRARY([TINFO], [curses.h], [tinfo],
      [AC_DEFINE([HAVE_CURSES_H], [1], [Define to 1 if <curses.h> is present])
        AC_SUBST([TINFO_CFLAGS], ["$TINFO_CPPFLAGS"])
        AC_SUBST([TINFO_LIBS], ["$TINFO_LDFLAGS -ltinfo"])],)])])

# Then try to find it in a specific install dir
AC_ARG_WITH(curses, [AS_HELP_STRING([--with-curses=DIR], [Where curses is installed])],
    [if test $withval != yes; then
        cv_curses=$withval
    fi
    if test x$cv_curses != x/usr; then
        CURSES_LDFLAGS="-L${cv_curses}/lib"
        CURSES_CPPFLAGS="-I${cv_curses}/include"
    fi])

if test "x$ax_cv_have_TINFO" = xno ; then
    # save away old LDFLAGS/CPPFLAGS and check for curses with cv_curses
    old_LDFLAGS="$LDFLAGS"
    old_CPPFLAGS="$CPPFLAGS"
    LDFLAGS="$LDFLAGS $CURSES_LDFLAGS"
    CPPFLAGS="$CPPFLAGS $CURSES_CPPFLAGS"

    AX_WITH_CURSES
    # restore old LDFLAGS/CPPFLAGS
    LDFLAGS="$old_LDFLAGS"
    CPPFLAGS="$old_CPPFLAGS"

    if test "x$ax_cv_curses_which" = xno ; then
        AC_MSG_ERROR([requires either tinfo, NcursesW or Ncurses library])
    else
        AC_SUBST([TINFO_LIBS], ["$CURSES_LDFLAGS $CURSES_LIB"])
        AC_SUBST([TINFO_CFLAGS], ["$CURSES_CPPFLAGS"])
    fi
fi

AC_ARG_ENABLE([static-curses],
  [AS_HELP_STRING([--enable-static-curses], [Link curses statically @<:@no@:>@])],
  [], [enable_static_curses="$enable_static_libraries"])
AS_IF([test "$enable_static_curses" = yes],
  [TINFO_LIBS="-Wl,-Bstatic $TINFO_LIBS -Wl,-Bdynamic"])

dnl Default to OpenSSL, or OS X crypto library if found
AC_CHECK_HEADERS([CommonCrypto/CommonCrypto.h],
  [default_crypto_library="apple-common-crypto"],
  [default_crypto_library="openssl"]
)

dnl Allow user to select over the default.
AC_ARG_WITH(
  [crypto-library],
  [AS_HELP_STRING([--with-crypto-library=library], [build with the given crypto library, TYPE=openssl|nettle|apple-common-crypto @<:@default=openssl@:>@])],
  [
    case "${withval}" in
      openssl|openssl-with-internal-ocb|openssl-with-openssl-ocb|nettle|apple-common-crypto) ;;
      *) AC_MSG_ERROR([bad value ${withval} for --with-crypto-library]) ;;
    esac
  ],
  [with_crypto_library="$default_crypto_library"]
)

dnl Checks for chosen crypto library
PKG_CHECK_MODULES([OpenSSL], [openssl],
  [have_openssl=yes
    AC_CHECK_LIB([crypto], [AES_encrypt], [have_deprecated_openssl_aes=yes])
    AC_CHECK_LIB([crypto], [EVP_aes_128_ocb], [have_evp_aes_ocb=yes])],
  [:])
PKG_CHECK_MODULES([Nettle], [nettle], [have_nettle=yes], [:])
AS_CASE([$with_crypto_library],
  [openssl*],
    [AS_IF([test "x$have_openssl" != xyes],
       [AC_MSG_ERROR([OpenSSL crypto library not found])])
     AC_DEFINE([USE_OPENSSL_AES], [1], [Use OpenSSL library])
     AC_SUBST([CRYPTO_CFLAGS], ["$OpenSSL_CFLAGS"])
     AC_SUBST([CRYPTO_LIBS], ["$OpenSSL_LIBS -lcrypto"])])
case "${with_crypto_library}" in
  openssl|openssl-with-internal-ocb)
    AS_IF([test "x$have_deprecated_openssl_aes" != xyes],
      [AC_MSG_ERROR([found OpenSSL without AES support])])
    AM_CONDITIONAL([USE_AES_OCB_FROM_OPENSSL], [false])
    human_readable_cryptography_description='internal OCB, OpenSSL AES'
    ;;
  openssl-with-openssl-ocb)
    AS_IF([test "x$have_evp_aes_ocb" != xyes],
      [AC_MSG_ERROR([found OpenSSL without AES-OCB support])])
    AM_CONDITIONAL([USE_AES_OCB_FROM_OPENSSL], [true])
    human_readable_cryptography_description='OpenSSL OCB, OpenSSL AES'
    ;;
  nettle)
    AS_IF([test "x$have_nettle" != xyes],
      [AC_MSG_ERROR([Nettle crypto library not found])])
    AC_DEFINE([USE_NETTLE_AES], [1], [Use Nettle library])
    AC_SUBST([CRYPTO_CFLAGS], ["$Nettle_CFLAGS"])
    AC_SUBST([CRYPTO_LIBS], ["$Nettle_LIBS"])
    AM_CONDITIONAL([USE_AES_OCB_FROM_OPENSSL], [false])
    human_readable_cryptography_description='internal OCB, Nettle AES'
    ;;
  apple-common-crypto)
    AS_IF([test "x$ac_cv_header_CommonCrypto_CommonCrypto_h" != xyes],
      [AC_MSG_ERROR([Apple Common Crypto header not found])])
    AC_DEFINE([USE_APPLE_COMMON_CRYPTO_AES], [1], [Use Apple Common Crypto library])
    AM_CONDITIONAL([USE_AES_OCB_FROM_OPENSSL], [false])
    human_readable_cryptography_description='internal OCB, Apple Common Crypto AES'
    ;;
esac

dnl The optional FIPS mode is independent of the library used to implement
dnl legacy OCB.  For example, macOS can retain CommonCrypto for normal mosh
dnl sessions while an explicitly requested FIPS session uses OpenSSL 3.
AC_ARG_WITH(
  [fips-crypto],
  [AS_HELP_STRING([--with-fips-crypto],
    [build --fips-crypto support with OpenSSL 3 (requires a configured FIPS provider at runtime) @<:@check@:>@])],
  [with_fips_crypto="$withval"],
  [with_fips_crypto="check"]
)

AS_CASE([$with_fips_crypto],
  [yes|no|check], [],
  [AC_MSG_ERROR([bad value ${with_fips_crypto} for --with-fips-crypto (use yes, no, or check)])])

have_fips_crypto=no
AS_IF([test "x$with_fips_crypto" != xno], [
  PKG_CHECK_MODULES([FIPS_OPENSSL], [libcrypto >= 3.0], [
    saved_CPPFLAGS="$CPPFLAGS"
    saved_LIBS="$LIBS"
    CPPFLAGS="$CPPFLAGS $FIPS_OPENSSL_CFLAGS"
    LIBS="$FIPS_OPENSSL_LIBS $LIBS"
    AC_LINK_IFELSE(
      [AC_LANG_PROGRAM(
        [[#include <openssl/evp.h>
          #include <openssl/kdf.h>
          #include <openssl/provider.h>
          #include <openssl/rand.h>]],
        [[OSSL_LIB_CTX *ctx = OSSL_LIB_CTX_new();
          EVP_CIPHER *cipher = EVP_CIPHER_fetch(ctx, "AES-128-GCM", "fips=yes");
          EVP_KDF *kdf = EVP_KDF_fetch(ctx, "HKDF", "fips=yes");
          EVP_CIPHER_free(cipher);
          EVP_KDF_free(kdf);
          OSSL_LIB_CTX_free(ctx);]])],
      [have_fips_crypto=yes],
      [have_fips_crypto=no])
    CPPFLAGS="$saved_CPPFLAGS"
    LIBS="$saved_LIBS"
  ], [:])
])

AS_IF([test "x$with_fips_crypto" = xyes && test "x$have_fips_crypto" != xyes],
  [AC_MSG_ERROR([--with-fips-crypto requires OpenSSL 3 libcrypto development files])])

AS_IF([test "x$have_fips_crypto" = xyes], [
  AC_DEFINE([HAVE_OPENSSL_FIPS_CRYPTO], [1], [Build support for the OpenSSL 3 FIPS provider.])
  CRYPTO_CFLAGS="$CRYPTO_CFLAGS $FIPS_OPENSSL_CFLAGS"
  CRYPTO_LIBS="$CRYPTO_LIBS $FIPS_OPENSSL_LIBS"
])

AC_ARG_ENABLE([static-crypto],
  [AS_HELP_STRING([--enable-static-crypto], [Link crypto library statically @<:@no@:>@])],
  [], [enable_static_crypto="$enable_static_libraries"])
AS_IF([test "$enable_static_crypto" = yes],
  [CRYPTO_LIBS="-Wl,-Bstatic $CRYPTO_LIBS -Wl,-Bdynamic"])

AC_CHECK_DECL([forkpty],
  [AC_DEFINE([FORKPTY_IN_LIBUTIL], [1],
     [Define if libutil.h necessary for forkpty().])],
  , [[#include <sys/types.h>
#include <libutil.h>]])

AC_SEARCH_LIBS([forkpty], [util bsd], [
  AC_DEFINE([HAVE_FORKPTY],, [Define if you have forkpty().])
])

AC_MSG_CHECKING([whether FD_ISSET() argument is const])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <sys/select.h>
class T {
public:
  fd_set fds;
  bool Fun( void ) const { return FD_ISSET( 0, &fds ); } };]],
[[T x = T(); return x.Fun();]])],
  [AC_DEFINE([FD_ISSET_IS_CONST], [1],
     [Define if FD_ISSET() fd_set argument is const.])
   AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])])

AC_MSG_CHECKING([whether std::shared_ptr is available])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <memory>
class T {
public:
  std::shared_ptr<int> Fun( void ) { return std::shared_ptr<int>( new int ( 0 ) ); } };]],
[[T x; return !!x.Fun();]])],
  [AC_DEFINE([HAVE_STD_SHARED_PTR], [1],
     [Define if std::shared_ptr is available.])
   AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])])

AC_MSG_CHECKING([whether std::tr1::shared_ptr is available])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include <tr1/memory>
class T {
public:
  std::tr1::shared_ptr<int> Fun( void ) { return std::tr1::shared_ptr<int>( new int ( 0 ) ); } };]],
[[T x; return !!x.Fun();]])],
  [AC_DEFINE([HAVE_STD_TR1_SHARED_PTR], [1],
     [Define if std::tr1::shared_ptr is available.])
   AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])])

AC_MSG_CHECKING([whether clock_gettime() is supported])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <time.h>
struct timespec ts;
]], [[return clock_gettime(CLOCK_MONOTONIC, &ts);]])],
  [AC_DEFINE([HAVE_CLOCK_GETTIME], [1],
     [Define if clock_gettime() is available.])
   AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])])

AC_CHECK_DECL([mach_absolute_time],
  [AC_DEFINE([HAVE_MACH_ABSOLUTE_TIME], [1],
     [Define if mach_absolute_time is available.])],
  , [[#include <mach/mach_time.h>]])


AC_CHECK_DECLS([__builtin_ctz])

AC_CHECK_DECLS([ffs], [], [],
  [[#include <strings.h>]])

AC_CHECK_DECLS([be64toh, betoh64, bswap64, __builtin_bswap64], [], [],
  [[#if defined(HAVE_ENDIAN_H)
#include <endian.h>
#elif defined(HAVE_SYS_ENDIAN_H)
#include <sys/types.h>
#include <sys/endian.h>
#endif]])

AS_IF([test x"$ac_cv_have_decl_be64toh" != xyes &&
       test x"$ac_cv_have_decl_betoh64" != xyes],
  [AC_CHECK_DECL([OSSwapHostToBigInt64],
     [AC_DEFINE([HAVE_OSX_SWAP], [1],
        [Define if OSSwapHostToBigInt64 and friends exist.])],
     [AC_MSG_WARN([Unable to find byte swapping functions; using built-in routines.])],
     [[#include <libkern/OSByteOrder.h>]])])

AC_CHECK_DECL([IP_MTU_DISCOVER],
  [AC_DEFINE([HAVE_IP_MTU_DISCOVER], [1],
     [Define if IP_MTU_DISCOVER is a valid sockopt.])],
  , [[#include <netinet/in.h>]])

AC_CHECK_DECL([IP_RECVTOS],
  [AC_DEFINE([HAVE_IP_RECVTOS], [1],
     [Define if IP_RECVTOS is a valid sockopt.])],
  , [[#include <netinet/in.h>]])

AC_CHECK_DECL([__STDC_ISO_10646__],
  [],
  [AC_MSG_WARN([C library doesn't advertise wchar_t is Unicode (OS X works anyway with workaround).])],
  [[#include <wchar.h>]])

AC_CHECK_DECL([IUTF8],
  [AC_DEFINE([HAVE_IUTF8], [1],
     [Define if IUTF8 is a defined termios mode.])],
  [AC_MSG_WARN([No IUTF8 termios mode; character-erase of multibyte character sequence probably does not work properly in canonical mode on this platform.])],
  [[#include <termios.h>]])

# Checks for protobuf
PKG_CHECK_MODULES([protobuf], [protobuf])

# On Debian Sid 2016-03-16ish at least, "pkgconfig --libs protobuf"
# gives us the bogus "-lprotobuf -pthread -lpthread"; remove
# "-lpthread" which misconfigures compile, causing a segfault in
# goblin-mosh-server.
AS_IF([echo "$protobuf_LIBS" | grep -q -- -pthread],
  [protobuf_LIBS="`echo $protobuf_LIBS | sed 's/-lpthread//g'`"])

AC_ARG_ENABLE([static-protobuf],
  [AS_HELP_STRING([--enable-static-protobuf], [Link protobuf statically @<:@no@:>@])],
  [], [enable_static_protobuf="$enable_static_libraries"])
AS_IF([test "$enable_static_protobuf" = yes],
  [protobuf_LIBS="-Wl,-Bstatic $protobuf_LIBS -Wl,-Bdynamic"],
  [AS_IF([test "$enable_static_libstdc__" = yes],
     [AC_MSG_ERROR([--enable-static-libstdc++ requires --enable-static-protobuf])])
   AS_IF([test "$enable_static_libgcc" = yes],
     [AC_MSG_ERROR([--enable-static-libgcc requires --enable-static-protobuf])])])

AC_MSG_CHECKING([whether protoc matches protobuf])
cat > conftest.proto <<'EOF'
syntax = "proto2";
option optimize_for = LITE_RUNTIME;
EOF
AS_IF([$PROTOC --cpp_out=. conftest.proto],
  [old_CPPFLAGS="$CPPFLAGS"
   CPPFLAGS="$CPPFLAGS $protobuf_CFLAGS"
   AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[#include "conftest.pb.h"]], [[]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     cat conftest.err
     AC_MSG_FAILURE([Could not build output generated by protoc ($PROTOC).
Check that protoc matches the installed protobuf headers and libraries.])])
   CPPFLAGS="$old_CPPFLAGS"],
  [AC_MSG_RESULT([no])
   AC_MSG_ERROR([Could not run protoc ($PROTOC).])])

# Bash completion needs to ask where it goes if >= 2.0 is installed.
AS_IF([test "$install_completion" != no],
  [PKG_CHECK_MODULES([BASH_COMPLETION], [bash-completion >= 2.0],
     [if test "$prefix" = "NONE"; then
        completions="`pkg-config --variable=completionsdir bash-completion`"
      else
        completions="`pkg-config --define-variable=prefix=$prefix --variable=completionsdir bash-completion`"
      fi],
     [completions="${sysconfdir}/bash_completion.d"])
   AC_SUBST([completions])])

AC_CONFIG_FILES([
  Makefile
  src/Makefile
  src/crypto/Makefile
  src/fec/Makefile
  src/moshcp/Makefile
  src/frontend/Makefile
  src/fuzz/Makefile
  src/include/Makefile
  src/network/Makefile
  src/protobufs/Makefile
  src/statesync/Makefile
  src/terminal/Makefile
  src/util/Makefile
  scripts/Makefile
  src/examples/Makefile
  src/tests/Makefile
  man/Makefile
  conf/Makefile
])
AC_OUTPUT

AC_MSG_NOTICE([ === Configuration results ===])
AC_MSG_NOTICE([Version:             $PACKAGE_VERSION])
AC_MSG_NOTICE([c++ compiler:        $CXX])
AC_MSG_NOTICE([Warning CXXFLAGS:    $WARNING_CXXFLAGS])
AC_MSG_NOTICE([Picky CXXFLAGS:      $PICKY_CXXFLAGS])
AC_MSG_NOTICE([Harden CFLAGS:       $HARDEN_CFLAGS])
AC_MSG_NOTICE([Cryptography:        $human_readable_cryptography_description])
AC_MSG_NOTICE([FIPS crypto mode:    $have_fips_crypto (OpenSSL FIPS provider required at runtime)])
AC_MSG_NOTICE([FEC Reed-Solomon:    yes])
AC_MSG_NOTICE([FEC RFC6330:         $have_libraptorq])
AC_MSG_NOTICE([moshcp zstd:         $have_zstd])
AC_MSG_NOTICE([File-menu librsync:  $have_file_sync])
AC_MSG_NOTICE([Kitty WebP state:    yes])
AC_MSG_NOTICE([Palette DjVu state: yes (cjb2)])
AC_MSG_NOTICE([ =============================])
