#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

m4_include([../vksemver/n4.m4])
n4_define_substrings_as(

    m4_include([PKG_SEMVER]),

    [\([0-9]+\)\s*\.\s*\([0-9]+\)\s*\.\s*\([0-9]+\)],

    [PKG_TAG], PKG_VERSION_MAJOR, PKG_VERSION_MINOR, [PKG_VERSION_PATCH]

)

AC_INIT([vkcon], PKG_VERSION_MAJOR[.]PKG_VERSION_MINOR)
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([gnu subdir-objects -Wall -Werror foreign])

# Checks for programs.
AM_PROG_AR
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AM_CFLAGS="-Wall -Werror -O2 -Wmissing-prototypes -Wstrict-prototypes -std=gnu89 -Wdeclaration-after-statement"
AC_SUBST([AM_CFLAGS])
PKG_CHECK_MODULES([pcimem], [pcimem])

# Checks for libraries.
AC_SEARCH_LIBS([pcimem_init], [pcimem], [], [
  AC_MSG_ERROR([unable to find pcimem_init()])
])

# Initialize Libtool
LT_INIT([shared])

VKCON_VERSION=PKG_VERSION_MAJOR:PKG_VERSION_MINOR
BRCM_VER_MAJOR=PKG_VERSION_MAJOR
BRCM_VER_MINOR=PKG_VERSION_MINOR
BRCM_VER_PATCH=PKG_VERSION_PATCH

AC_SUBST(VKCON_VERSION)
AC_SUBST(BRCM_VER_MAJOR)
AC_SUBST(BRCM_VER_MINOR)
AC_SUBST(BRCM_VER_PATCH)
# Checks for header files.
# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_CONFIG_FILES([Makefile vkcon.pc semver/version.h:../vksemver/version.h.in])
AC_OUTPUT
