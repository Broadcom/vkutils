#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_INIT([vkcli], [1.0])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([-Wall -Werror subdir-objects foreign])

# Checks for programs.
AM_PROG_AR
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AM_CFLAGS="-Wall -Werror -O2 -Wmissing-prototypes -Wstrict-prototypes -std=gnu89 -Wdeclaration-after-statement"
PKG_CHECK_MODULES([pcimem], [pcimem])
AC_SUBST([AM_CFLAGS])
# Checks for libraries.
AC_SEARCH_LIBS([pcimem_init], [pcimem], [], [
  AC_MSG_ERROR([unable to find pcimem_init()])
])

# Set shared libraries

# Initialize Libtool
LT_INIT([shared])

# Checks for header files.
# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
