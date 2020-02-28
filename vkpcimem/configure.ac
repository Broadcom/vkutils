#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_INIT([pcimem], [1.0])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

# Checks for programs.
AM_PROG_AR
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AM_CFLAGS="-Wall -Werror"
# Checks for libraries.

# Set shared libraries

# Checks for header files.
# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

PCIMEM_VERSION=1:0:0

AC_SUBST(PCIMEM_VERSION)
AC_SUBST(LIBS_PUBLIC)
AC_SUBST(LIBS_PRIVATE)
AC_SUBST([AM_CFLAGS])

#Initialize Libtool
LT_INIT([shared])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
