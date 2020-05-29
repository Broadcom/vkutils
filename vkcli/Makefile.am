ACLOCAL_AMFLAGS = -I m4 --install
sbin_PROGRAMS = vkcli

vkcli_SOURCES = $(srcdir)/../vksemver/pkg_semver.c vkcli.c
vkcli_CFLAGS = $(pcimem_CFLAGS) -I$(srcdir)/../vksemver  -I$(srcdir)/../vkshare -Isemver
vkcli_LDADD = $(pcimem_LIBS)
