lib_LTLIBRARIES = libvcon.la

include_HEADERS = vcon_api.h

libvcon_la_SOURCES = vcon_api.c

ACLOCAL_AMFLAGS = -I m4

sbin_PROGRAMS = vkcon vkcmd

vkcon_DEPENDENCIES = libvcon.la
vkcon_SOURCES = vkcon.c
vkcon_LDFLAGS = -pthread -lvcon -lpcimem

vkcmd_DEPENDENCIES = libvcon.la
vkcmd_SOURCES = vkcmd.c
vkcmd_LDFLAGS = -pthread -lvcon

