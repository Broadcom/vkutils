// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020 Broadcom
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "bcm_vk.h"
#include "vcon_api.h"
#include "vcon_chan_intf.h"
#include "pcimem.h"

/**
 * @file
 * @brief virtual console APIs
 *
 * This is a collection of APIs that allow user to create his/her own
 * apps/daemons to use the BAR2-mapped memory channel for input/output
 * to the valkyrie card.
 */

#define DEV_DRV_NAME		"/dev/bcm_vk"
#define DEV_ALT_DRV_NAME	"/dev/bcm-vk"
#define DEV_SYSFS_NAME		"/sys/class/misc/bcm-vk"
#define DEV_SYS_RESOURCE	"/pci/resource"

enum chan_state {
	VCON_CMD_CHAN_FREE = 0,
	VCON_CMD_CHAN_OCCUPIED
};

/* local defines */

#define VCON_IN_CMD_POLL_US	(100 * 1000) /* 100ms */
#define VCON_IN_CMD_TIMEOUT_US	(5 * 1000000) /* 5s timeout */
#define VCON_IN_CMD_POLL_MAX	(VCON_IN_CMD_TIMEOUT_US / VCON_IN_CMD_POLL_US)
#define MAX_ERR_MSG		255

/* command doorbell notification definitions */
#define VCON_BOOT_STATUS_OFFSET	0x404
#define VCON_CMD_DB_OFFSET	0x49c
#define VCON_CMD_DB_VAL		0xFFFFFFF0
/* default log region offset value, could be overwritten by user */
#define VCON_BUF_BAR2_OFF	0x3800000
#define VCON_BOOT2_RUNNING	0x100006

#define MAX_MMAP_SIZE		(2 * 1024 * 1024)

/* default memory map sizes */
#define PAGE_MMAP_SIZE		(4 * 1024)
#define VCON_DEF_MMAP_SIZE	(256 * 1024)
#define MAX_MMAP_SIZE		(2 * 1024 * 1024)

/* local macros */
#define PERROR(...) do { \
			snprintf(e_msg, \
				 MAX_ERR_MSG, \
				 __VA_ARGS__);\
			fprintf(stderr, \
				" @L:%d %s\n", \
				__LINE__, \
				e_msg);\
			fflush(stderr);\
			} while (0)

typedef struct _dev_ctx {
	int fd;
	uint32_t req_size;
	struct map_info *m_info;
} dev_ctx;

typedef struct _con_channel {
	dev_ctx *p_dev;
	logger_buf *p_log_buf;    /* logger buffer pointer */
	uint32_t rd_idx;          /* current rd_idx */
} con_channel;

typedef dev_ctx cmd_channel;

typedef struct _con_ctx {
	con_channel con;
	cmd_channel *p_cmd;
} con_ctx;

static int string2ul(char *str, unsigned long *return_value)
{
	char *endptr = NULL;
	char e_msg[MAX_ERR_MSG] = "";

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
 * @brief Read data to the buffer passed in by caller
 * @param ctx channel context
 * @param buf pointer to output buf
 * @param buf_size size of the output buffer
 * @Return number of bytes that have been extracted, including '\0', negative
 *         number for error conditions
 *
 * NOTE: the output buffer will always has a complete log. If the buffer could
 *       not fit a line in, the line will stay intact until the next polling.
 */
int vcon_get_cmd_output(void *ctx, char *buf, const size_t buf_size)
{
	int cnt;
	char e_msg[MAX_ERR_MSG] = "";
	uint32_t entry_len;
	uint32_t nentries;
	char *p_buf = buf;
	con_ctx *p_ctx;
	char *p_line;
	logger_buf *p_log_buf;
	uint32_t *rd_idx;
	int ret = 0;
	char *spool_buf;

	if (!ctx || !buf) {
		PERROR("Invalid parameters\n");
		return -EACCES;
	}
	p_ctx = (con_ctx *)ctx;
	p_log_buf = p_ctx->con.p_log_buf;
	rd_idx = &p_ctx->con.rd_idx;
	if (!p_log_buf || p_log_buf->marker != VCON_MARKER) {
		PERROR("Invalid marker\n");
		return -EACCES;
	}
	entry_len = p_log_buf->spool_len;
	nentries = p_log_buf->spool_nentries;
	spool_buf = ((char *)p_log_buf + p_log_buf->spool_off);

	if (buf_size < entry_len)
		return -E2BIG;

	while ((*rd_idx != p_log_buf->spool_idx) &&
	       ((ret + entry_len) <= buf_size)) {
		/* check marker for PCIe going down */
		if (p_log_buf->marker != VCON_MARKER) {
			PERROR("PCIe interface going down\n");
			return -EACCES;
		}
		p_line = &spool_buf[*rd_idx * entry_len];

		/* cp the whole line but excluding terminator */
		cnt = 0;
		while ((p_line[cnt] != '\0') && (cnt < entry_len)) {
			p_buf[cnt] = p_line[cnt];
			cnt++;
		}

		/*
		 * if there is error in the conversion above, the current
		 * idx is skipped, which is equivalent of dropping one log.
		 */
		if ((cnt != 0) && (cnt < entry_len)) {
			/* update pointer */
			p_buf += cnt;
			ret += cnt;
		}

		*rd_idx = (*rd_idx + 1) & (nentries - 1);
	}

	return ret;
}

/**
 * @brief Sending a command to the card through PCIe
 * @param ctx channel context
 * @param cmd command
 */
int vcon_send_cmd(void *ctx, const char *cmd)
{
	char *cmd_chan;
	uint32_t cnt = 0;
	static uint32_t data = VCON_CMD_DB_VAL;
	char e_msg[MAX_ERR_MSG] = "";
	int len = 0;
	con_ctx *p_ctx;
	logger_buf *p_log_buf;
	int rc;

	if (!ctx)
		return -EINVAL;
	p_ctx = (con_ctx *)ctx;
	p_log_buf = p_ctx->con.p_log_buf;
	if (!p_log_buf) {
		PERROR("Invalid log buffer\n");
		return -EACCES;
	}
	cmd_chan = ((char *)p_log_buf +
		    p_log_buf->cmd_off);
	if (cmd[0] == '\0')
		goto tx_success;

	if (p_log_buf->marker != VCON_MARKER) {
		PERROR("failed to find marker\n");
		return -EACCES;
	}

	/* first byte is an indicator */
	if (*cmd_chan != VCON_CMD_CHAN_FREE) {
		PERROR("channel busy\n");
		return -EBUSY;
	}
	/* cpy the command to the cmd location, and wait to be cleared */
	strncpy(cmd_chan + 1, cmd, VCON_MAX_CMD_SIZE);
	cmd_chan[VCON_MAX_CMD_SIZE] = '\0';
	*cmd_chan = VCON_CMD_CHAN_OCCUPIED; /* mark it to be valid */
	/* press door bell */
	rc = pcimem_write(p_ctx->p_cmd->m_info,
			  VCON_CMD_DB_OFFSET,
			  len,
			  &data,
			  ALIGN_32_BIT);
	if (rc < 0) {
		PERROR("bad io write; err=0x%x\n",
		       rc);
		return rc;
	}
	usleep(VCON_IN_CMD_POLL_US);
	/* loop until doorbell cleared */
	while (++cnt <= VCON_IN_CMD_POLL_MAX) {
		if (*cmd_chan == VCON_CMD_CHAN_FREE)
			break;
		usleep(VCON_IN_CMD_POLL_US);
	}
	if (cnt > VCON_IN_CMD_POLL_MAX) {
		PERROR("timeout waiting for doorbell clear\n");
		return -ETIMEDOUT;
	}
tx_success:
	return STATUS_OK;
}

/**
 * @brief Memory map a VK device's PCIe node: device/bar
 * @param ctx channel context
 * @param dev_name Name of device to be mapped or just the numeric number
 * @param bar BAR where mapping should start
 * @param offset offset in BAR where mapping should start
 * @return STATUS_OK, or negative error value
 */
int vcon_mem_map_node(dev_ctx **ctx,
		      const char *dev_name,
		      int bar,
		      uint32_t offset,
		      uint32_t size)
{
	char devnode[FNAME_LEN];
	char e_msg[MAX_ERR_MSG] = "";
	unsigned long fnode = 0;
	int fd = -1;
	dev_ctx *p_dev;
	char *node_num = NULL;
	int ret = -EINVAL;

	if (!ctx)
		return -EINVAL;

	if (*ctx) {
		p_dev = *ctx;
	} else {
		struct map_info *p_map_info;

		p_dev = (dev_ctx *)malloc(sizeof(*p_dev));
		if (!p_dev)
			return -EINVAL;
		*ctx = p_dev;
		p_dev->fd = -1;
		p_map_info = (struct map_info *)malloc(sizeof(*p_map_info));
		if (!p_map_info)
			return -EINVAL;
		p_dev->m_info = p_map_info;
	}
	memset(devnode, 0, sizeof(devnode));
	/* open device */
	ret = strlen(dev_name);
	if (ret > 3) {
		node_num = strstr(dev_name, ".");
		if (!node_num) {
			PERROR("invalid device\n");
			return -EINVAL;
		}
		node_num++;
	} else {
		node_num = (char *)dev_name;
	}

	ret = string2ul(node_num, &fnode);
	if (ret < 0) {
		PERROR("invalid node\n");
		return -EINVAL;
	}
	snprintf(devnode,
		 sizeof(devnode),
		 DEV_SYSFS_NAME ".%ld" DEV_SYS_RESOURCE "%d",
		 fnode, 2 * bar);
	p_dev->m_info->d_id.nd = fnode;
	p_dev->m_info->d_id.bar = bar;
	if (p_dev->fd > 0) {
		p_dev->m_info->fd = p_dev->fd;
		ret = pcimem_deinit(p_dev->m_info);
		if (ret < 0) {
			PERROR("fail to unmmap\n");
			return -EINVAL;
		}
	}
	pcimem_init(devnode, p_dev->m_info);
	p_dev->fd = p_dev->m_info->fd;
	p_dev->req_size = size;
	p_dev->m_info->map_size = size;
	ret = pcimem_map_base(p_dev->m_info,
			      offset,
			      sizeof(uint32_t));
	if (ret < 0) {
		PERROR("fail to mmap BAR 2\n");
		return -EINVAL;
	}
	return ret;
}

/**
 * @brief Open a VK device's PCIe console channel
 * @param ctx channel context
 * @param dev_name Name of device to be opened or just the numeric number
 * @param offset offset in BAR where mapping should start
 * @return STATUS_OK, or negative error value
 */
int vcon_open_cmd_chan(void **ctx,
		       const char *dev_name,
		       size_t *mmap_size)
{
	int bar;
	off_t bar0_off;
	off_t bar2_off;
	char e_msg[MAX_ERR_MSG] = "";
	con_ctx *p_ctx;
	dev_ctx *p_dev;
	logger_buf *p_log_buf;
	int ret = STATUS_OK;
	unsigned long new_size;
	uint32_t boot_status;

	if (!ctx || !dev_name || !mmap_size) {
		PERROR("invalid parameters\n");
		return -EINVAL;
	}
	*ctx = (con_ctx *)malloc(sizeof(con_ctx));
	p_ctx = (con_ctx *)*ctx;
	if (!p_ctx) {
		PERROR("fail to alloc context\n");
		return -EINVAL;
	}

	bar = 0;
	bar0_off = VCON_CMD_DB_OFFSET;
	p_dev = NULL;
	ret = vcon_mem_map_node(&p_dev,
				dev_name,
				bar,
				bar0_off,
				PAGE_MMAP_SIZE);
	if (ret < 0) {
		PERROR("failed to open cmd channel\n");
		if (p_dev)
			free(p_dev);
		vcon_close_cmd_chan(p_ctx);
		return ret;
	}
	p_ctx->p_cmd = p_dev;

	if (!p_dev) {
		PERROR("fail to map node %s\n", dev_name);
		return -EINVAL;
	}
	/* read boot-status */
	ret = pcimem_blk_read(p_dev->m_info,
			      VCON_BOOT_STATUS_OFFSET,
			      sizeof(boot_status),
			      &boot_status,
			      ALIGN_32_BIT);
	if ((ret < 0) || (boot_status != VCON_BOOT2_RUNNING)) {
		PERROR("Card not in proper status 0x%x - ret(%d)\n",
		       boot_status, ret);
		free(p_dev);
		vcon_close_cmd_chan(p_ctx);
		return -EINVAL;
	}

	bar = 2;
	bar2_off = VCON_BUF_BAR2_OFF;
	p_dev = NULL;
	ret = vcon_mem_map_node(&p_dev,
				dev_name,
				bar,
				bar2_off,
				VCON_DEF_MMAP_SIZE);
	if (ret < 0) {
		PERROR("failed to open channel\n");
		if (p_dev)
			free(p_dev);
		vcon_close_cmd_chan(p_ctx);
		return ret;
	}
	p_ctx->con.p_dev = p_dev;

	if (!p_dev) {
		PERROR("fail to map node %s\n", dev_name);
		return -EINVAL;
	}
	/* do mmap & map it to our struct */
	p_log_buf = p_dev->m_info->map_base;
	p_ctx->con.p_log_buf = p_log_buf;

	if (!p_log_buf || p_log_buf->marker != VCON_MARKER) {
		PERROR("failed to find marker\n");
		if (p_dev)
			free(p_dev);
		vcon_close_cmd_chan(p_ctx);
		ret = -EACCES;
	}
	/*
	 * Do some calculation and see if
	 * the mmap region is big enough.
	 * If not, remmap based on new size
	 */
	new_size = p_log_buf->cmd_off + VCON_CMD_CHAN_SIZE;
	if (ret == STATUS_OK &&
	    new_size > p_dev->req_size) {
		ret = vcon_mem_map_node(&p_dev,
					dev_name,
					bar,
					bar2_off,
					new_size);
		if (ret < 0) {
			PERROR("failed to open channel\n");
			if (p_dev)
				free(p_dev);
			vcon_close_cmd_chan(p_ctx);
			return ret;
		}
		p_ctx->con.p_dev = p_dev;
		p_log_buf = p_dev->m_info->map_base;
		p_ctx->con.p_log_buf = p_log_buf;
	}
	if (!p_log_buf || p_log_buf->marker != VCON_MARKER) {
		PERROR("failed to find marker\n");
		vcon_close_cmd_chan(p_ctx);
		ret = -EACCES;
	}
	p_ctx->con.p_dev = p_dev;
	p_ctx->con.p_log_buf = p_log_buf;
	p_ctx->con.rd_idx = p_log_buf->spool_idx;
	*mmap_size = p_ctx->con.p_dev->m_info->map_size;
	return ret;
}

/**
 * @brief Close the channel
 * @param ctx channel context
 * @return STATUS_OK, or negative error value
 */
int vcon_close_cmd_chan(void *ctx)
{
	con_ctx *p_ctx;
	dev_ctx *p_dev;
	int ret = STATUS_OK;

	if (!ctx)
		return -EINVAL;
	p_ctx = (con_ctx *)ctx;
	p_dev = p_ctx->con.p_dev;
	if (p_dev && p_dev->m_info) {
		p_dev->m_info->fd = p_dev->fd;
		ret = pcimem_deinit(p_dev->m_info);
		free(p_dev->m_info);
		free(p_dev);
	}
	p_dev = p_ctx->p_cmd;
	if (p_dev && p_dev->m_info) {
		p_dev->m_info->fd = p_dev->fd;
		ret = pcimem_deinit(p_dev->m_info);
		free(p_dev->m_info);
		free(p_dev);
	}
	free(p_ctx);
	return ret;
}
