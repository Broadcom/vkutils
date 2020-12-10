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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "pcimem.h"

/* internal defines */
#define NODE_NOT_FOUND			0xFF

/* internal structures */
struct map_list {
	int fd;
	off_t base;
	off_t size;
	void *p_mapping;
	struct id_info d_id;
	struct map_list *next;
};

static inline void pr_err(const struct id_info *p_id, int lineno,
			  const char *func, int err, const char *fmt, ...)
{
	va_list vl;

	fprintf(stderr,
		"Id %04d:%02d\tErr@L: %d, F: %s (%d) [%s] ",
		(p_id) ? p_id->nd : -1,
		(p_id) ? p_id->bar : -1, lineno, func,
		-err, strerror(err));
	va_start(vl, fmt);
	vfprintf(stderr, fmt, vl);
	va_end(vl);
	fprintf(stderr, "\n");
	fflush(stderr);

	/* any error, exit after logging */
	exit(-1);
}

#define PRINT_ERROR(p_id, fmt, ...) \
	pr_err(p_id, __LINE__, __func__, errno, fmt, ##__VA_ARGS__)

#if defined(MMAP_DEBUG)
#define FPR_FN(...) \
	do { \
		fprintf(stdout, __VA_ARGS__); \
		fflush(stdout); \
	} while (0)
#else
#define FPR_FN(...)
#endif

/* internal variables */
static struct map_list *p_crt_mappings;

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
	*return_value = strtoul(str, &endptr, 0);
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
	struct id_info *p_did;
	unsigned long val;

	if (!dev_name || !p_info)
		return -EINVAL;

	p_did = &p_info->d_id;
	/* get node info */
	len = strlen(dev_name);
	if (len <= 3) {
		PRINT_ERROR(p_did, "Node len too short");
		return -EINVAL;
	}
	num = strstr(dev_name, ".");
	if (!num) {
		PRINT_ERROR(p_did, "Dev missing dot separate");
		return -EINVAL;
	}
	num++;
	ret = str2ul(num, &val);
	if (ret < 0) {
		PRINT_ERROR(p_did, "str2ul conversion failure");
		return -EINVAL;
	}
	p_info->d_id.nd = val;

	/* get bar info */
	p_info->d_id.bar = 0;
	num = &dev_name[len - 1];
	while (isdigit(*num))
		num--;
	ret = str2ul(num + 1, &val);
	if (ret < 0) {
		PRINT_ERROR(p_did, "Get BAR num failure");
		return -EINVAL;
	}
	p_info->d_id.bar = val;
	return STATUS_OK;
}

/**
 * @brief is_device_open check if we have a valid fd for device
 *
 * @param[in] p_info  - id of node
 * @param[out] points to the element identified as already open
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int is_device_open(struct map_info *p_info,
			  struct map_list **p_map_list)
{
	int ret, found = 0;
	struct map_list *p_elem;

	if (!p_info || !p_map_list)
		return -EINVAL;

	*p_map_list = NULL;
	p_elem = p_crt_mappings;
	while (p_elem) {
		ret = memcmp(&p_elem->d_id,
			     &p_info->d_id,
			     sizeof(p_info->d_id));
		if (ret == 0 && p_elem->fd > 0) {
			found = 1;
			break;
		}
		p_elem = p_elem->next;
	}
	if (!found)
		return NODE_NOT_FOUND;
	*p_map_list = p_elem;
	return STATUS_OK;
}

/**
 * @brief find_mapping check if we have a mapping for current request
 *
 * @param[in] p_info  - id of node
 * @param[out] points to the element identified as already open
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int find_mapping(struct map_info const *p_info,
			const off_t offset,
			const int d_size,
			struct map_list **p_map_list)
{
	int ret, found = 0;
	struct map_list *p_elem;

	if (!p_info || !p_map_list)
		return -EINVAL;

	*p_map_list = NULL;
	p_elem = p_crt_mappings;
	while (p_elem) {
		ret = memcmp(&p_elem->d_id,
			     &p_info->d_id,
			     sizeof(p_info->d_id));
		if (ret == 0 && p_elem->fd > 0) {
			if (offset >= p_elem->base &&
			    offset + d_size <= p_elem->base + p_elem->size) {
				found = 1;
				break;
			}
		}
		p_elem = p_elem->next;
	}
	if (!found)
		return NODE_NOT_FOUND;
	*p_map_list = p_elem;
	return STATUS_OK;
}

/**
 * @brief ins_elem_map_list insert new element in the mapping list
 *
 * @param[in] p_map_node - element to insert at the end of the list
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int ins_elem_map_list(struct map_list *p_map_node)
{
	struct map_list *head = p_crt_mappings;
	struct map_list *p_new_elem;
	struct id_info *p_did;

	if (!p_map_node)
		return -EINVAL;

	p_did = &p_map_node->d_id;
	p_new_elem = (struct map_list *)malloc(sizeof(*p_new_elem));
	if (!p_new_elem) {
		PRINT_ERROR(p_did, "map_list alloc failure");
		return -ENOMEM;
	}
	memcpy(p_new_elem, p_map_node, sizeof(*p_new_elem));
	p_new_elem->next = NULL;
	if (!p_crt_mappings) {
		p_crt_mappings = p_new_elem;
	} else {
		head = p_crt_mappings;
		while (head->next)
			head = head->next;
		head->next = p_new_elem;
	}
	return STATUS_OK;
}

/**
 * @brief del_elem_map_list delete element from the mapping list
 *
 * @param[in] p_map_node - element to remove from the list
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int del_elem_map_list(struct map_list *p_map_node)
{
	struct map_list *head = p_crt_mappings;

	if (!p_map_node || !head)
		return -EINVAL;

	while (head->next && head->next != p_map_node)
		head = head->next;
	if (head->next == p_map_node) {
		head->next = p_map_node->next;
		free(p_map_node);
		p_map_node = NULL;
	}
	return STATUS_OK;
}

/**
 * @brief pcimem_init init the pcimem library; opens the device as file
 *
 * @param[in] device_name name of the device to init
 * @param[out] p_info->map_size init the map_size to system PAGE_SIZE
 * @param[out] p_info->fd - file descriptor
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_init(char * const device_name, struct map_info *p_info)
{
	int ret = -EINVAL;
	struct id_info *p_did;
	struct map_list *p_map_list;

	if (!p_info)
		return -EINVAL;

	p_did = &p_info->d_id;
	ret = get_Id(device_name, p_info);
	if (ret < 0) {
		PRINT_ERROR(p_did, "Get Id failure");
		return ret;
	}
	/* bypass opening the device if already open */
	is_device_open(p_info, &p_map_list);
	if (p_map_list) {
		p_info->fd = p_map_list->fd;
	} else {
		ret = open(device_name, O_RDWR | O_SYNC);
		if (ret < 0) {
			PRINT_ERROR(p_did, "Open dev %s failure", device_name);
			return -errno;
		}
		FPR_FN("%s opened.\nPage size is %ld\n",
		       device_name,
		       sysconf(_SC_PAGE_SIZE));
		p_info->map_size = sysconf(_SC_PAGE_SIZE);
		p_info->fd = ret;
	}
	return STATUS_OK;
}

/**
 * @brief perform memory map for the previous open device
 *
 * @param[in] p_info->map_size used in map call
 * @param[out] p_info->map_base - base address mapped in user space
 * @param[in] p_info->fd - the device file descriptor
 * @param[in] offset - offset from base to access
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_map_base(struct map_info *p_info,
		    const off_t offset,
		    const int type_width)
{
	off_t base_offset;
	off_t target_size;
	struct id_info *p_did;
	struct map_list elem_list;

	if (!p_info)
		return -EINVAL;

	p_did = &p_info->d_id;
	/* align the base address for mmap */
	base_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);

	/* size must be page aligned also */
	target_size = PAGE_RNDUP(p_info->map_size,
				 sysconf(_SC_PAGE_SIZE));

	FPR_FN("mmap( %10" PRId64 " 0x%x, 0x%x, %d, 0x%lx)\n",
	       target_size,
	       PROT_READ | PROT_WRITE,
	       MAP_SHARED,
	       p_info->fd,
	       base_offset);

	/* Map required size rounded up to be page aligned */
	p_info->map_base = mmap(0,
				target_size,
				PROT_READ | PROT_WRITE | MAP_NORESERVE,
				MAP_LOCKED | MAP_SHARED,
				p_info->fd,
				base_offset);
	if (p_info->map_base == (void *) -1) {
		PRINT_ERROR(p_did, "Mmap failure - size 0x%lx", target_size);
		return -EINVAL;
	}
	elem_list.d_id = p_info->d_id;
	elem_list.base = base_offset;
	elem_list.size = target_size;
	elem_list.p_mapping = p_info->map_base;
	elem_list.fd = p_info->fd;
	ins_elem_map_list(&elem_list);
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
int pcimem_read(struct map_info const *p_info,
		const off_t offset,
		const int d_size,
		void *p_data,
		const int type_width)
{
	struct id_info const *p_did;
	struct map_list *p_elem;
	int res;
	void *virt_addr;

	if (!p_info || !p_data)
		return -EINVAL;

	p_did = &p_info->d_id;
	res = find_mapping(p_info, offset, d_size, &p_elem);
	if (res < 0) {
		PRINT_ERROR(p_did, "Find mapping failure size 0x%x offset 0x%lx",
			    d_size, offset);
		return -EINVAL;
	}
	if (p_elem) {
		virt_addr = p_elem->p_mapping + offset - p_elem->base;
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
			PRINT_ERROR(p_did, "type width %d unsupported",
				    type_width);
			return -EINVAL;
		}
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
int pcimem_write(struct map_info const *p_info,
		 const off_t offset,
		 const int d_size,
		 void *p_data,
		 const int type_width)
{
	struct id_info const *p_did;
	struct map_list *p_elem;
	int res;
	void *virt_addr;

	if (!p_info || !p_data)
		return -EINVAL;

	p_did = &p_info->d_id;
	res = find_mapping(p_info, offset, d_size, &p_elem);
	if (res < 0) {
		PRINT_ERROR(p_did, "Find mapping failure size 0x%x offset 0x%lx",
			    d_size, offset);
		return -EINVAL;
	}
	if (p_elem) {
		virt_addr = p_elem->p_mapping + offset - p_elem->base;
		switch (type_width) {
		case ALIGN_8_BIT:
			*((uint8_t *)virt_addr) = *(uint8_t *)p_data;
			break;
		case ALIGN_16_BIT:
			*((uint16_t *)virt_addr) = *(uint16_t *)p_data;
			break;
		case ALIGN_32_BIT:
			*((uint32_t *)virt_addr) = *(uint32_t *)p_data;
			break;
		case ALIGN_64_BIT:
			*((uint64_t *)virt_addr) = *(uint64_t *)p_data;
			break;
		default:
			PRINT_ERROR(p_did, "type_width %d unsupported",
				    type_width);
			return -EINVAL;
		}
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
int pcimem_blk_read(struct map_info const *p_info,
		    const off_t offset,
		    const int d_size,
		    void *p_data,
		    const int type_width)
{
	struct id_info const *p_did;
	struct map_list *p_elem;
	int res = -EINVAL;
	void *virt_addr;

	if (!p_info || !p_data)
		return -EINVAL;

	p_did = &p_info->d_id;
	res = find_mapping(p_info, offset, d_size, &p_elem);
	if (res < 0) {
		PRINT_ERROR(p_did, "Find mapping failure size 0x%x offset 0x%lx",
			    d_size, offset);
		return -EINVAL;
	}
	if (p_elem) {
		virt_addr = p_elem->p_mapping + offset - p_elem->base;
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
 * @param[in] offset - offset within page
 * @param[in] d_size - size of data buffer
 * @param[in] p_data - data buffer
 * @param[in] type_width - access width in bytes
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_blk_write(struct map_info const *p_info,
		     const off_t offset,
		     const int d_size,
		     void *p_data,
		     const int type_width)
{
	struct id_info const *p_did;
	struct map_list *p_elem;
	int res = -EINVAL;
	void *virt_addr;

	if (!p_info || !p_data)
		return -EINVAL;

	p_did = &p_info->d_id;
	res = find_mapping(p_info, offset, d_size, &p_elem);
	if (res < 0) {
		PRINT_ERROR(p_did, "Find mapping failure size 0x%x offset 0x%lx",
			    d_size, offset);
		return -EINVAL;
	}
	if (p_elem) {
		virt_addr = p_elem->p_mapping + offset - p_elem->base;
		memcpy(virt_addr, p_data, d_size);
	}
	return STATUS_OK;
}

/**
 * @brief pcimem_deinit deinit the pcimem library;
 * unmap, make map_base pointer NULL, closes the file descriptor
 *
 * @param[in/out] p_info->map_base - base address mapped in user space
 * @param[in] p_info->map_size - size of region mapped in user space
 * @param[in/out] p_info->fd - file descriptor
 *
 * @return STATUS_OK on success, error code otherwise
 */
int pcimem_deinit(struct map_info *p_info)
{
	struct id_info const *p_did;
	int ret = -EINVAL;
	struct map_list *p_map_list = NULL;

	if (!p_info || !p_info->map_base)
		return ret;

	p_did = &p_info->d_id;
	if (p_info->map_size >= 0) {
		ret = munmap(p_info->map_base, p_info->map_size);
		if (ret < 0) {
			PRINT_ERROR(p_did,
				    "Failure to unmap base %p size 0x%lx",
				    p_info->map_base, p_info->map_size);
			return -errno;
		}
	}
	p_info->map_base = NULL;
	p_info->map_size = 0;
	is_device_open(p_info, &p_map_list);
	if (p_map_list) {
		if (p_info->fd >= 0) {
			ret = close(p_info->fd);
			p_info->fd = -1;
			p_map_list->fd = -1;
		}
		del_elem_map_list(p_map_list);
	}
	ret = (ret < 0) ? -errno : STATUS_OK;
	return ret;
}
