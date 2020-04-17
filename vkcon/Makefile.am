lib_LTLIBRARIES = libvcon.la

include_HEADERS = vcon_api.h vcon_chan_intf.h

libvcon_la_SOURCES = vcon_api.c
libvcon_la_CFLAGS = $(pcimem_CFLAGS)

ACLOCAL_AMFLAGS = -I m4 --install

sbin_PROGRAMS = vkcon vkcmd vlogdump

vkcon_DEPENDENCIES = libvcon.la
vkcon_SOURCES = vkcon.c
vkcon_CFLAGS = $(pcimem_CFLAGS)
vkcon_LDADD = $(pcimem_LIBS)
vkcon_LDFLAGS = -pthread -lvcon

vkcmd_DEPENDENCIES = libvcon.la
vkcmd_SOURCES = vkcmd.c
vkcmd_CFLAGS = $(pcimem_CFLAGS)
vkcmd_LDADD = $(pcimem_LIBS)
vkcmd_LDFLAGS = -pthread -lvcon

vlogdump_DEPENDENCIES = libvcon.la
vlogdump_SOURCES = vcon_logdump.c

