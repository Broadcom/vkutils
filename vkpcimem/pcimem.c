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
#include <limits.h>
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
		if (!p_info) { \
			exit(-1); \
		} else { \
			fprintf(stderr, \
				"Id %04d:%02d\tErr@L: %d, F: %s (%d) [%s]\n", \
				p_info->d_id.nd, \
				p_info->d_id.bar, \
				__LINE__, \
				__FILE__, \
				errno, \
				strerror(errno)); \
			exit(1); \
		} \
	} while(0)

#define _STR_BASE(_str)	(_str[1] == 'x' ? 16 : 10)

/**
 * @brief str2ul strtoul wrapper function
 *
 * @param[in] str string to convert
 * @param[out] return_value value
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int str2ul(char *str, unsigned long *return_value)
{
	char *endptr = NULL;

	if (str == NULL || return_value == NULL)
		return -EINVAL;
	*return_value = strtoul(str, &endptr, _STR_BASE(str));
	if (endptr == str)
		return -EINVAL;
	if ((*return_value == LONG_MAX ||
	     *return_value == LONG_MIN) && errno == ERANGE)
		return -ERANGE;
	return STATUS_OK;
}

/**
 * @brief get_Id parses device name string and gets d_id info
 *
 * @param[in] device_name name of the device to init
 * @param[out] p_info->d_id.nd - node
 * @param[out] p_info->d_id.bar - bar
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int get_Id(char * const dev_name, struct map_info *p_info)
{
	char *num = NULL;
	int len, ret;
	unsigned long val;

	if (!dev_name || !p_info)
		return -EINVAL;

	/* get node info */
	len = strlen(dev_name);
	if (len <= 3)
		return -EINVAL;
	num = strstr(dev_name, ".");
	if (!num)
		return -EINVAL;
	num++;
	ret = str2ul(num, &val);
	if (ret < 0)
		return -EINVAL;
	p_info->d_id.nd = val;

	/* get bar info */
	p_info->d_id.bar = 0;
	num = &dev_name[len - 1];
	while (isdigit(*num))
		num--;
	ret = str2ul(num + 1, &val);
	if (ret < 0)
		return -EINVAL;
	p_info->d_id.bar = val;
	return STATUS_OK;
}

/**
 * @brief check_range memory range check
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - length of memory mapped region
 * @param[in] offset - offset from base to access
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int check_range(const struct map_info *p_info, off_t offset)
{
	void *virt_addr;
	void *end_map_addr;

	if (!p_info || offset < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	virt_addr = p_info->map_base + p_info->off_base + offset;
	end_map_addr = p_info->map_base + p_info->map_size;
	if (virt_addr >= end_map_addr) {
		PR_FN("ERR_RANGE: start / end:\n%p\n%p\n",
		      virt_addr,
		      end_map_addr);
		PRINT_ERROR;
		return -EINVAL;
	}
	return STATUS_OK;
}

/**
 * @brief pcimem_init init the pcimem library; opens the device as file
 *
 * @param[in] device_name name of the device to init
 * @param[out] p_info->map_size init the map_size to system PAGE_SIZE
 * @param[out] pfd - pointer to the device file descriptor
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_init(char * const device_name, struct map_info *p_info, int *pfd)
{
	int ret = -EINVAL;

	ret = get_Id(device_name, p_info);
	if (ret < 0)
		return ret;
	if (!pfd) {
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
 * @param[in] offset - offset from base to access
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_map_base(struct map_info *p_info,
		    const int fd,
		    const off_t offset,
		    const int type_width)
{
	off_t target_size;
	off_t base_offset;

	/* align the base address for mmap */
	base_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);

	/* size must be page aligned also */
	target_size = PAGE_RNDUP(p_info->map_size,
				 sysconf(_SC_PAGE_SIZE));

	FPR_FN("mmap( %10" PRId64 " 0x%x, 0x%x, %d, 0x%lx)\n",
	       target_size,
	       PROT_READ | PROT_WRITE,
	       MAP_SHARED,
	       fd,
	       base_offset);

	/* Map required size rounded up to be page aligned */
	p_info->map_base = mmap(0,
				target_size,
				PROT_READ | PROT_WRITE | MAP_NORESERVE,
				MAP_LOCKED | MAP_SHARED,
				fd,
				base_offset);
	if (p_info->map_base == (void *) -1) {
		PRINT_ERROR;
		return -EINVAL;
	}
	p_info->off_base = offset - base_offset;
	FPR_FN("PCI Memory mapped range:\n%p\n%p\n",
	       p_info->map_base,
	       p_info->map_base + p_info->map_size);
	return STATUS_OK;
}

/**
 * @brief pcimem_read reads within one mem page
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] offset - offset from mapped base
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_read(const struct map_info *p_info,
		const off_t offset,
		const int d_size,
		void *p_data,
		const int type_width)
{
	int res;
	void *virt_addr;

	res = check_range(p_info, offset);
	if (res < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	virt_addr = p_info->map_base + p_info->off_base + offset;
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
		PRINT_ERROR;
		return -EINVAL;
	}
	return STATUS_OK;
}

/**
 * @brief pcimem_write writes within one mem page
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] offset - offset from mapped base
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_write(const struct map_info *p_info,
		 const off_t offset,
		 const int d_size,
		 void *p_data,
		 const int type_width)
{
	int res;
	uint64_t read_result = -1;
	void *virt_addr;

	res = check_range(p_info, offset);
	if (res < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	virt_addr = p_info->map_base + p_info->off_base + offset;
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
		PRINT_ERROR;
		return -EINVAL;
	}
	if (read_result != *(uint64_t *)p_data) {
		PRINT_ERROR;
		return -EINVAL;
	}

	return STATUS_OK;
}

/**
 * @brief pcimem_blk_read reads from mem map space
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - size of region mapped in user space
 * @param[in] offset - offset from mapped base
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_blk_read(const struct map_info *p_info,
		    const off_t offset,
		    const int d_size,
		    void *p_data,
		    const int type_width)
{
	int res = -EINVAL;
	void *virt_addr;

	if (!p_info | !p_data) {
		PRINT_ERROR;
		return -EINVAL;
	}
	res = check_range(p_info, offset);
	if (res < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	res = check_range(p_info, offset + d_size - 1);
	if (res < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	virt_addr = p_info->map_base + p_info->off_base + offset;
	memcpy(p_data, virt_addr, d_size);
	res = STATUS_OK;
	return res;
}

/**
 * @brief pcimem_blk_write writes to mem map space
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - size of region mapped in user space
 * @param[in] offset - offset within page
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_blk_write(const struct map_info *p_info,
		     const off_t offset,
		     const int d_size,
		     void *p_data,
		     const int type_width)
{
	int res = -EINVAL;
	void *virt_addr;

	if (!p_info | !p_data) {
		PRINT_ERROR;
		return -EINVAL;
	}
	res = check_range(p_info, offset);
	if (res < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	res = check_range(p_info, offset + d_size - 1);
	if (res < 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	virt_addr = p_info->map_base + p_info->off_base + offset;
	memcpy(virt_addr, p_data, d_size);
	if (memcmp(p_data, virt_addr, d_size) != 0) {
		PRINT_ERROR;
		return -EINVAL;
	}
	res = STATUS_OK;
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
