#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_INIT([elf2vk], [1.0])
AC_LANG_WERROR
AM_INIT_AUTOMAKE([-Wall -Werror subdir-objects foreign])

# Checks for programs.
AM_PROG_AR
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AM_CFLAGS="-Wall -Werror -O2 -Wmissing-prototypes -Wstrict-prototypes -std=gnu89 -Wdeclaration-after-statement"
AC_SUBST([AM_CFLAGS])

# Checks for libraries.
LIBS="-lelf"

# Set shared libraries

# Checks for header files.
# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
