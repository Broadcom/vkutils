lib_LTLIBRARIES = libvcon.la

include_HEADERS = vcon_api.h

libvcon_la_SOURCES = vcon_api.c

sbin_PROGRAMS = vkcon

vkcon_DEPENDENCIES = libvcon.la
vkcon_SOURCES = vkcon.c
vkcon_LDFLAGS = -pthread -lvcon
