lib_LTLIBRARIES = libvcon.la

include_HEADERS = vcon_api.h

libvcon_la_SOURCES = ../vksemver/pkg_semver.c vcon_api.c
libvcon_la_CFLAGS = $(pcimem_CFLAGS) -I$(srcdir)/../vkshare -I$(srcdir)/../vksemver -Isemver

ACLOCAL_AMFLAGS = -I m4 --install

sbin_PROGRAMS = vkcon vkcmd vlogdump

vkcon_DEPENDENCIES = libvcon.la
vkcon_SOURCES = ../vksemver/pkg_semver.c vkcon.c
vkcon_CFLAGS = $(pcimem_CFLAGS) -I$(srcdir)/../vkshare -I$(srcdir)/../vksemver -Isemver -DMULTITHREAD
vkcon_LDADD = $(pcimem_LIBS)
vkcon_LDFLAGS = -pthread -lvcon

vkcmd_DEPENDENCIES = libvcon.la
vkcmd_SOURCES = ../vksemver/pkg_semver.c vkcmd.c
vkcmd_CFLAGS = $(pcimem_CFLAGS) -I$(srcdir)/../vkshare -I$(srcdir)/../vksemver -Isemver
vkcmd_LDADD = $(pcimem_LIBS)
vkcmd_LDFLAGS = -pthread -lvcon

vlogdump_SOURCES = ../vksemver/pkg_semver.c vcon_logdump.c
vlogdump_CFLAGS = $(pcimem_CFLAGS) -I$(srcdir)/../vkshare -I$(srcdir)/../vksemver -Isemver

pkgconfigbindir = $(bindir)/pkgconfig
pkgconfigbin_DATA = vkcon.pc
