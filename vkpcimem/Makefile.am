ACLOCAL_AMFLAGS = -I m4 --install

include_HEADERS = pcimem.h
lib_LTLIBRARIES = libpcimem.la
libpcimem_la_CFLAGS = -Isemver
libpcimem_la_LDFLAGS = -version-info @PCIMEM_VERSION@ -no-undefined
libpcimem_la_SOURCES = ../vksemver/pkg_semver.c pcimem.c

pkgconfiglibdir = $(libdir)/pkgconfig
pkgconfiglib_DATA = pcimem.pc
