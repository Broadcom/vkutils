// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Broadcom.
 *
 * Derived from:
 * pcimem.c: Simple program to read/write to a pci device from userspace
 *
 * Copyright (C) 2010, Bill Farrow (bfarrow@beyondelectronics.us)
 *
 * Based on the devmem2.c code
 * Copyright (C) 2000, Jan-Derk Bakker (J.D.Bakker@its.tudelft.nl)
 *
 */

#include <errno.h>
#include <fcntl.h>
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
 * @return 0 on success, error code otherwise
 */
int pcimem_init(const char *device_name, map_info *p_info, int *pfd)
{
	*pfd = open(device_name, O_RDWR | O_SYNC);
	if (*pfd == -1) {
		PRINT_ERROR;
		return -EINVAL;
	}
	printf("%s opened.\n", device_name);
	printf("Page size is %ld\n", sysconf(_SC_PAGE_SIZE));
	fflush(stdout);
	p_info->map_size = sysconf(_SC_PAGE_SIZE);

	return 0;
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
 * @return 0 on success, error code otherwise
 */
int pcimem_map_base(map_info *p_info,
		    const int fd,
		    const off_t target,
		    const int type_width)
{
	off_t target_base;
	target_base = target & ~(sysconf(_SC_PAGE_SIZE)-1);

	if (target + type_width - target_base > p_info->map_size)
		p_info->map_size = target + type_width - target_base;

	/* Map one page */
	printf("mmap(%d, %d, 0x%x, 0x%x, %d, 0x%x)\n",
		0,
		p_info->map_size,
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		fd,
		(int)target);

	p_info->map_base =
		mmap(0,
			p_info->map_size,
			PROT_READ | PROT_WRITE,
			MAP_SHARED,
			fd,
			target_base);
	if (p_info->map_base == (void *) -1) {
		PRINT_ERROR;
		return -EINVAL;
	}
	printf("PCI Memory mapped to address 0x%08lx.\n",
		(unsigned long)p_info->map_base);
	fflush(stdout);
	return 0;
}

/**
 * @breif pcimem_read reads from mem map space
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] target - offset from base to access
 * @param[in] type_width - access width in bytes
 *
 * @return the value read from offset
 */
int pcimem_read(const map_info *p_info,
		const off_t target,
		const int d_size,
		void *p_data,
		const int type_width)
{
	void *virt_addr;
	off_t target_base = target & ~(sysconf(_SC_PAGE_SIZE)-1);

	virt_addr = p_info->map_base + target - target_base;
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
 * @breif pcimem_write writes to mem map space
 *
 * @param[in] p_info->map_base - base address mapped in user space
 * @param[in] target - offset from base to access
 * @param[in] type_width - access width in bytes
 *
 * @return the value read back from the same location written
 */
int pcimem_write(const map_info *p_info,
		 const off_t target,
		 const int d_size,
		 void *p_data,
		 const int type_width)
{
	void *virt_addr;
	uint64_t read_result = -1;
	off_t target_base = target & ~(sysconf(_SC_PAGE_SIZE)-1);

	virt_addr = p_info->map_base + target - target_base;
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
 * @brief pcimem_deinit deinit the pcimem library;
 * unmap, make map_base pointer NULL, closes the file descriptor
 *
 * @param[in/out] p_info->map_base - base address mapped in user space
 * @param[in/out] pfd - pointer to the device file descriptor
 *
 * @return 0 on success, error code otherwise
 */
int pcimem_deinit(map_info *p_info, int *pfd)
{
	if (munmap(p_info->map_base, p_info->map_size) == -1) {
		PRINT_ERROR;
		return -EINVAL;
	}
	p_info->map_base = NULL;
	close(*pfd);
	return 0;
}
