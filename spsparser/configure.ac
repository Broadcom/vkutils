#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_INIT(sps_pps_parser, 0.1)

AC_CONFIG_SRCDIR(./sps_pps_parser.c)
AC_CONFIG_MACRO_DIRS([m4])

AC_CONFIG_AUX_DIR(build-aux)

AM_CFLAGS='Wall -Werror -Wstrict-prototypes -std=gnu89 -Wdeclaration-after-statement'
AC_SUBST(AM_CFLAGS)

AC_LANG_WERROR
AM_INIT_AUTOMAKE([-Wall -Werror subdir-objects foreign])

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_LD
AC_PROG_INSTALL
AC_PROG_LIBTOOL

AC_CHECK_FUNCS(getopt_long, , AC_MSG_WARN(getopt_long not found. Long options will not work.) )

AC_CONFIG_FILES([Makefile])

AC_OUTPUT
