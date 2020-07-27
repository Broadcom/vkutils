ACLOCAL_AMFLAGS = -I m4 --install
sbin_PROGRAMS = vkcli

vkcli_SOURCES = vkcli.c
vkcli_CFLAGS = $(pcimem_CFLAGS) -I$(srcdir)/../vkshare
vkcli_LDADD = $(pcimem_LIBS)
