/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2020 Broadcom.
 */

#ifndef PCIMEM_API_H
#define PCIMEM_API_H

#include <ctype.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct _map_info {
	void *map_base;
	int map_size;
} map_info;

int pcimem_init(const char *device_name,
		map_info *p_info,
		int *pfd);

int pcimem_map_base(map_info *p_info,
		    const int fd,
		    const off_t target,
		    const int type_width);

uint64_t pcimem_read(const map_info *p_info,
		     const off_t target,
		     const int type_width);

uint64_t pcimem_write(const map_info *p_info,
		      const off_t target,
		      const uint64_t write_val,
		      const int type_width);

int pcimem_deinit(map_info *p_info,
		  int *pfd);

#endif /* PCIMEM_API_H */
