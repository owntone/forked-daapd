AC_INIT([librespot-c], [0.1])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])

AC_PROG_CC
AM_PROG_AR
AC_PROG_RANLIB

AM_CPPFLAGS="-Wall"
AC_SUBST([AM_CPPFLAGS])

AC_CHECK_HEADERS_ONCE([sys/utsname.h])

AC_CHECK_HEADERS([endian.h sys/endian.h libkern/OSByteOrder.h], [found_endian_headers=yes; break;])
AS_IF([test "x$found_endian_headers" != "xyes"], [AC_MSG_ERROR([[Missing functions to swap byte order]])])

AC_SEARCH_LIBS([pthread_exit], [pthread], [], [AC_MSG_ERROR([[pthreads library is required]])])

PKG_CHECK_MODULES([LIBEVENT], [libevent])
PKG_CHECK_MODULES([JSON_C], [json-c])
PKG_CHECK_MODULES([LIBGCRYPT], [libgcrypt], [], [
	AM_PATH_LIBGCRYPT([], [], [AC_MSG_ERROR([[libgcrypt is required]])])
])
PKG_CHECK_MODULES([LIBCURL], [libcurl])
PKG_CHECK_MODULES([LIBPROTOBUF_C], [libprotobuf-c])

AC_CONFIG_FILES([Makefile tests/Makefile])
AC_OUTPUT
