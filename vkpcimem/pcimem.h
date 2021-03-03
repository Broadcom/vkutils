/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2020 Broadcom.
 */

#ifndef PCIMEM_API_H
#define PCIMEM_API_H

#include <ctype.h>
#include <stdint.h>
#include <sys/types.h>

#define STATUS_OK               0

#define PAGE_RNDUP(x, s)        (((x) + (s) - 1) & ~((s) - 1))

/* transaction width - future use - all is 32 bit for now */
enum bit_align {
	ALIGN_8_BIT = 1,
	ALIGN_16_BIT = 2,
	ALIGN_32_BIT = 4,
	ALIGN_64_BIT = 8
};

struct id_info {
	uint16_t nd;
	uint16_t bar;
};

struct map_info {
	struct id_info d_id;
	int fd;
	void *map_base;
	off_t map_size;
};

int pcimem_init(char * const device_name,
		struct map_info *p_info);

int pcimem_map_base(struct map_info *p_info,
		    const off_t target,
		    const int type_width);

int pcimem_blk_read(struct map_info const *p_info,
		    const off_t target,
		    const int d_size,
		    void *p_data,
		    const int type_width);

int pcimem_blk_write(struct map_info const *p_info,
		     const off_t target,
		     const int d_size,
		     void *p_data,
		     const int type_width);

int pcimem_read(struct map_info const *p_info,
		const off_t target,
		const int d_size,
		void *p_data,
		const int type_width);

int pcimem_write(struct map_info const *p_info,
		 const off_t target,
		 const int d_size,
		 void *p_data,
		 const int type_width);

int pcimem_deinit(struct map_info *p_info);

#endif /* PCIMEM_API_H */
