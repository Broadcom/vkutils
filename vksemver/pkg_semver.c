// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020 Broadcom
 */

#include "version.h"

__attribute__ ((__used__, __section__(".verinfo")))
char verinfo_strings[] =
"name=" PACKAGE_NAME "\0"
"version=" PKG_VERSION_MAJOR "." PKG_VERSION_MINOR "." PKG_VERSION_PATCH "\0";

__attribute__ ((__used__, __section__(".gnu.linkonce.this_module")))
struct module {
	char __pad0[0x18];
	char name[sizeof(PACKAGE_NAME)];
} __attribute__ ((__packed__))
__this_module = {
	.name = PACKAGE_NAME,
};
