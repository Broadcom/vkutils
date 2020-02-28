/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2020 Broadcom.
 */

#include <ctype.h>
#include <sys/types.h>

typedef struct _map_info {
	void *map_base;
	int map_size;
} map_info;

int pcimem_init(const char *device_name, map_info *p_info, int *pfd);

int pcimem_map_base(map_info *p_info,
		    const int fd,
		    const off_t target,
		    const int type_width);

u_int64_t pcimem_read(const map_info *p_info,
		      const off_t target,
		      const int type_width);

u_int64_t pcimem_write(const map_info *p_info,
		       const off_t target,
		       const u_int64_t write_val,
		       const int type_width);

int pcimem_deinit(map_info *p_info, int *pfd);
