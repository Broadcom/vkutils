// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Broadcom.
 *
 * Derived from:
 * pcimem.c: Simple program to read/write to a memory mapped device
 * from userspace
 *
 * Copyright (C) 2010, Bill Farrow (bfarrow@beyondelectronics.us)
 *
 * Based on the devmem2.c code
 * Copyright (C) 2000, Jan-Derk Bakker (J.D.Bakker@its.tudelft.nl)
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "pcimem.h"

#define PRINT_ERROR \
	do { \
		fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n", \
		__LINE__, __FILE__, errno, strerror(errno)); exit(1); \
	} while(0)

/**
 * @brief pcimem_init init the pcimem library; opens the device as file
 *
 * @param[in] device_name name of the device to init
 * @param[out] p_info->map_size init the map_size to system PAGE_SIZE
 * @param[out] pfd - pointer to the device file descriptor
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_init(const char *device_name, struct map_info *p_info, int *pfd)
{
	int ret = -EINVAL;

	if (!p_info || !pfd) {
		PRINT_ERROR;
		return ret;
	}
	ret = open(device_name, O_RDWR | O_SYNC);
	if (ret < 0) {
		PRINT_ERROR;
		return -errno;
	}
	PR_FN("%s opened.\nPage size is %ld\n",
	      device_name,
	      sysconf(_SC_PAGE_SIZE));
	fflush(stdout);
	p_info->map_size = sysconf(_SC_PAGE_SIZE);
	*pfd = ret;
	return STATUS_OK;
}

/**
 * @brief perform memory map for the previous open device
 *
 * @param[in] p_info->map_size used in map call
 * @param[out] p_info->map_base - base address mapped in user space
 * @param[in] fd - the device file descriptor
 * @param[in] target - offset from base to access
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_map_base(struct map_info *p_info,
		    const int fd,
		    const off_t target,
		    const int type_width)
{
	off_t target_base;
	target_base = target & ~(sysconf(_SC_PAGE_SIZE)-1);

	if (target + type_width - target_base > p_info->map_size)
		p_info->map_size = target + type_width - target_base;

	/* Map one page */
	printf("mmap( %10" PRId64 " 0x%x, 0x%x, %d, 0x%x)\n",
	       p_info->map_size,
	       PROT_READ | PROT_WRITE,
	       MAP_SHARED,
	       fd,
	       (int)target);

	p_info->map_base = mmap(0,
				p_info->map_size,
				PROT_READ | PROT_WRITE | MAP_NORESERVE,
				MAP_LOCKED | MAP_SHARED,
				fd,
				target_base);
	if (p_info->map_base == (void *) -1) {
		PRINT_ERROR;
		return -EINVAL;
	}
	printf("PCI Memory mapped to address 0x%08lx.\n",
		(unsigned long)p_info->map_base);
	fflush(stdout);
	return STATUS_OK;
}

/**
 * @brief pcimem_read reads within one mem page
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] target - offset within page
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_read(const struct map_info *p_info,
		const off_t target,
		const int d_size,
		void *p_data,
		const int type_width)
{
	void *virt_addr;

	if (target > sysconf(_SC_PAGE_SIZE))
		return -EINVAL;
	virt_addr = p_info->map_base + target;
	switch (type_width) {
	case ALIGN_8_BIT:
		*(uint8_t *)p_data = *((uint8_t *)virt_addr);
		break;
	case ALIGN_16_BIT:
		*(uint16_t *)p_data = *((uint16_t *)virt_addr);
		break;
	case ALIGN_32_BIT:
		*(uint32_t *)p_data = *((uint32_t *)virt_addr);
		break;
	case ALIGN_64_BIT:
		*(uint64_t *)p_data = *((uint64_t *)virt_addr);
		break;
	default:
		return -EINVAL;
	}
	return STATUS_OK;
}

/**
 * @brief pcimem_write writes within one mem page
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] target - offset within page
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_write(const struct map_info *p_info,
		 const off_t target,
		 const int d_size,
		 void *p_data,
		 const int type_width)
{
	void *virt_addr;
	uint64_t read_result = -1;

	if (target > sysconf(_SC_PAGE_SIZE))
		return -EINVAL;
	virt_addr = p_info->map_base + target;
	switch (type_width) {
	case ALIGN_8_BIT:
		*((uint8_t *)virt_addr) = *(uint8_t *)p_data;
		read_result = *((uint8_t *)virt_addr);
		break;
	case ALIGN_16_BIT:
		*((uint16_t *)virt_addr) = *(uint16_t *)p_data;
		read_result = *((uint16_t *)virt_addr);
		break;
	case ALIGN_32_BIT:
		*((uint32_t *)virt_addr) = *(uint32_t *)p_data;
		read_result = *((uint32_t *)virt_addr);
		break;
	case ALIGN_64_BIT:
		*((uint64_t *)virt_addr) = *(uint64_t *)p_data;
		read_result = *((uint64_t *)virt_addr);
		break;
	default:
		return -EINVAL;
	}
	if (read_result != *(uint64_t *)p_data)
		return -EINVAL;

	return STATUS_OK;
}

/**
 * @brief pcimem_blk_read reads from mem map space
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - size of region mapped in user space
 * @param[in] target - offset within page
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_blk_read(const struct map_info *p_info,
		    const off_t target,
		    const int d_size,
		    void *p_data,
		    const int type_width)
{
	void *end_mapped_addr;
	int res = -EINVAL;
	void *virt_addr;

	if (target > sysconf(_SC_PAGE_SIZE))
		return -EINVAL;
	if (p_info != NULL && p_data != NULL) {
		end_mapped_addr = p_info->map_base + p_info->map_size;
		virt_addr = p_info->map_base + target;
		if (virt_addr == NULL ||
		    virt_addr + d_size > end_mapped_addr)
			return -EINVAL;
		memcpy(p_data, virt_addr, d_size);
		res = STATUS_OK;
	}
	return res;
}

/**
 * @brief pcimem_blk_write writes to mem map space
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - size of region mapped in user space
 * @param[in] target - offset within page
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_blk_write(const struct map_info *p_info,
		     const off_t target,
		     const int d_size,
		     void *p_data,
		     const int type_width)
{
	void *end_mapped_addr;
	int res = -EINVAL;
	void *virt_addr;

	if (target > sysconf(_SC_PAGE_SIZE))
		return -EINVAL;
	if (p_info != NULL && p_data != NULL) {
		end_mapped_addr = p_info->map_base + p_info->map_size;
		virt_addr = p_info->map_base + target;
		if (virt_addr == NULL ||
		    virt_addr + d_size > end_mapped_addr)
			return -EINVAL;
		memcpy(virt_addr, p_data, d_size);
		if (memcmp(p_data, virt_addr, d_size) != 0)
			return -EINVAL;
		res = STATUS_OK;
	}
	return res;
}

/**
 * @brief pcimem_deinit deinit the pcimem library;
 * unmap, make map_base pointer NULL, closes the file descriptor
 *
 * @param[in/out] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - size of region mapped in user space
 * @param[in/out] pfd - pointer to the device file descriptor
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_deinit(struct map_info *p_info, int *pfd)
{
	int ret = -EINVAL;

	if (!p_info || !p_info->map_base || !pfd) {
		PRINT_ERROR;
		return ret;
	}
	ret = munmap(p_info->map_base, p_info->map_size);
	if (ret < 0) {
		PRINT_ERROR;
		return -errno;
	}
	p_info->map_base = NULL;
	p_info->map_size = 0;
	ret = close(*pfd);
	ret = (ret < 0) ? -errno : STATUS_OK;
	return ret;
}
