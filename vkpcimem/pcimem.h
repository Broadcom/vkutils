/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2020 Broadcom.
 */

#ifndef PCIMEM_API_H
#define PCIMEM_API_H

#include <ctype.h>
#include <stdint.h>
#include <sys/types.h>

#define STATUS_OK	0

/* transaction width - future use - all is 32 bit for now */
enum bit_align {
	ALIGN_8_BIT = 1,
	ALIGN_16_BIT = 2,
	ALIGN_32_BIT = 4,
	ALIGN_64_BIT = 8
};

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

int pcimem_read(const map_info *p_info,
		const off_t target,
		const int d_size,
		void *p_data,
		const int type_width);

int pcimem_write(const map_info *p_info,
		 const off_t target,
		 const int d_size,
		 void *p_data,
		 const int type_width);

int pcimem_deinit(map_info *p_info,
		  int *pfd);

#endif /* PCIMEM_API_H */
