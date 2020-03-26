// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2019 Broadcom
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
#include "pcimem.h"

/**
 * @file
 * @brief virtual console APIs
 *
 * This is a collection of APIs that allow user to create his/her own
 * apps/daemons to use the BAR2-mapped memory channel for input/output
 * to the valkyrie card.
 */

#define DEV_DRV_NAME             "/dev/bcm_vk"
#define DEV_ALT_DRV_NAME         "/dev/bcm-vk"
#define DEV_SYSFS_NAME           "/sys/class/misc/bcm-vk"
#define DEV_SYS_RESOURCE         "/pci/resource"

#define VCON_CMD_CHAN_FREE       0
#define VCON_CMD_CHAN_OCCUPIED   1

/* local defines */
#define VCON_MARKER              0xbeefcafe
#define VCON_DEF_MMAP_SIZE       (256 * 1024)

#define VCON_IN_CMD_POLL_US      (100 * 1000) /* 100ms */
#define VCON_IN_CMD_POLL_MAX     10 /* max polls before timeout */

/* command doorbell notification definitions */
#define VCON_CMD_DB_OFFSET       0x49c
#define VCON_CMD_DB_VAL          0xFFFFFFF0

#define MAX_MMAP_SIZE            (2 * 1024 * 1024)

#define STR_BASE(str)            (str[1] == 'x' ? 16 : 10)

/**
 * interface structure, and this has to match the VK side definition.
 * we only care about the spooled part.
 */
typedef struct _logger_buf {
	uint32_t marker;         /**< marker to indicate if VK has init */
	uint32_t cmd_off;        /**< offset of cmd buffer from start */

	/*-------------------------------------------------------------------*/
	uint32_t spool_nentries; /**< total of spool entries  */
	uint32_t spool_len;      /**< length of per spooled entry */
	uint32_t spool_off;      /**< offset of spooled buffer from beginning */
	uint32_t spool_idx;      /**< idx of the next spooled buffer */
} logger_buf;

/* static logger buffer pointer, to be set in open */
static uint32_t mmap_size = VCON_DEF_MMAP_SIZE;
static logger_buf *p_log_buf;
static uint32_t rd_idx; /* current rd_idx */

struct map_info chan_map_info = { {0, 0}, NULL, 0, 4096 };
struct map_info cmd_map_info = { {0, 0}, NULL, 0, 4096 };

static int string2ul(char *str, unsigned long *return_value)
{
	char *endptr = NULL;

	if (str == NULL || return_value == NULL)
		return -EINVAL;
	*return_value = strtoul(str, &endptr, STR_BASE(str));
	if (endptr == str)
		return -EINVAL;
	if ((*return_value == LONG_MAX ||
	     *return_value == LONG_MIN) && errno == ERANGE)
		return -ERANGE;
	return STATUS_OK;
}

/**
 * @brief Read data to the buffer passed in by caller
 * @param buf pointer to output buf
 * @param buf_size size of the output buffer
 * @Return number of bytes that have been extracted, including '\0', negative
 *         number for error conditions
 *
 * NOTE: the output buffer will always has a complete log. If the buffer could
 *       not fit a line in, the line will stay intact until the next polling.
 */
int vcon_get_cmd_output(char *buf, const size_t buf_size)
{
	int ret = 0, cnt;
	uint32_t entry_len;
	uint32_t nentries;
	char *spool_buf;
	char *p_buf = buf;
	char *p_line;

	if (p_log_buf->marker != VCON_MARKER) {
		PR_FN("Invalid marker");
		return -EACCES;
	}
	entry_len = p_log_buf->spool_len;
	nentries = p_log_buf->spool_nentries;
	spool_buf = ((char *)p_log_buf + p_log_buf->spool_off);

	if (buf_size < entry_len)
		return -E2BIG;

	while ((rd_idx != p_log_buf->spool_idx) &&
	       ((ret + entry_len) <= buf_size)) {
		/* check marker for PCIe going down */
		if (p_log_buf->marker != VCON_MARKER) {
			PR_FN("PCIe interface going down");
			return -EACCES;
		}
		p_line = &spool_buf[rd_idx * entry_len];

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

		rd_idx = (rd_idx + 1) & (nentries - 1);
	}

	return ret;
}

/**
 * @brief Sending a command to the card through PCIe
 * @param fd file descriptor of the device
 * @param cmd command
 */
int vcon_send_cmd(int fd, const char *cmd)
{
	char *cmd_chan;
	uint32_t cnt = 0;
	static uint32_t data = VCON_CMD_DB_VAL;
	int len = 0;
	int rc;

	cmd_chan = ((char *)p_log_buf + p_log_buf->cmd_off);
	if (cmd[0] == '\0')
		goto tx_success;

	if (p_log_buf->marker != VCON_MARKER) {
		PR_FN("failed to find markeri\n");
		return -EACCES;
	}

	/* first byte is an indicator */
	if (*cmd_chan != VCON_CMD_CHAN_FREE) {
		PR_FN("channel busy\n");
		return -EBUSY;
	}
	/* cpy the command to the cmd location, and wait for it to be cleared */
	strncpy(cmd_chan + 1, cmd, VCON_MAX_CMD_SIZE);
	cmd_chan[VCON_MAX_CMD_SIZE] = '\0';
	*cmd_chan = VCON_CMD_CHAN_OCCUPIED; /* mark it to be valid */
	/* press door bell */
	rc = pcimem_write(&cmd_map_info,
			  0,
			  len,
			  &data,
			  ALIGN_32_BIT);
	if (rc < 0) {
		PR_FN("bad io write; err=0x%x\n",
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
		PR_FN("timeout waiting for doorbell clear\n");
		return -ETIMEDOUT;
	}
tx_success:
	return STATUS_OK;
}

/**
 * @brief Open a VK device's PCIe command channel
 * @param dev_name Name of device to be opened or just the numeric number
 * @param offset offset in BAR where mapping should start
 * @param p_mapped_size return mapped size
 * @return positive file descriptor, or negative error value
 */
int vcon_open_cmd_chan(const char *dev_name, uint32_t offset,
		       uint32_t *p_mapped_size)
{
	unsigned long fnode = 0;
	unsigned long req_size = 0;
	int bar = 2;
	char devnode[50];
	char *node_num = NULL;
	off_t bar0_off, bar2_off;
	int ret = -EINVAL;

	/* open device */
	if (strlen(dev_name) > 3) {
		node_num = strstr(dev_name, ".");
		if (!node_num) {
			PR_FN("invalid device\n");
			return -EINVAL;
		}
		node_num++;
	} else {
		node_num = (char *)dev_name;
	}
	string2ul(node_num, &fnode);

	snprintf(devnode,
		 sizeof(devnode),
		 DEV_SYSFS_NAME ".%ld" DEV_SYS_RESOURCE "%d",
		 fnode, 2 * bar);
	pcimem_init(devnode, &chan_map_info, (int *)&fnode);
	chan_map_info.map_size = mmap_size;
	bar2_off = VCON_BUF_BAR2_OFF;
	ret = pcimem_map_base(&chan_map_info,
			      fnode,
			      bar2_off,
			      sizeof(uint32_t));
	if (ret < 0) {
		PR_FN("fail to mmap\n");
		return -EINVAL;
	}
	/* do mmap & map it to our struct */
	p_log_buf = chan_map_info.map_base +
		    chan_map_info.off_base;
	if (p_log_buf->marker != VCON_MARKER) {
		PR_FN("failed to find marker\n");
		ret = -EACCES;
	}
	/*
	 * Do some calculation and see if the mmap region is big enough.
	 * If not, remmap based on new size
	 */
	req_size = p_log_buf->cmd_off + VCON_CMD_CHAN_SIZE;
	if (ret == STATUS_OK && req_size > mmap_size) {
		pcimem_deinit(&chan_map_info,
			      (int *)&fnode);
		pcimem_init(devnode, &chan_map_info, (int *)&fnode);
		chan_map_info.map_size = req_size;
		ret = pcimem_map_base(&chan_map_info,
				      fnode,
				      bar2_off,
				      sizeof(uint32_t));
		if (ret < 0) {
			PR_FN("fail to mmap\n");
			return -EINVAL;
		}
	}
	p_log_buf = chan_map_info.map_base +
		    chan_map_info.off_base;
	*p_mapped_size = chan_map_info.map_size;
	rd_idx = p_log_buf->spool_idx;

	/* Need bar 0 also mapped for sending commands */
	bar = 0;
	memset(devnode, 0, sizeof(devnode));
	string2ul(node_num, &fnode);
	snprintf(devnode,
		 sizeof(devnode),
		 DEV_SYSFS_NAME ".%ld" DEV_SYS_RESOURCE "%d",
		 fnode, 2 * bar);
	pcimem_init(devnode, &cmd_map_info, (int *)&fnode);
	bar0_off = VCON_CMD_DB_OFFSET;
	ret = pcimem_map_base(&cmd_map_info,
			      fnode,
			      bar0_off,
			      sizeof(uint32_t));
	return ret;
}

/**
 * @brief Close the command channel
 * @param fd file descriptor that has been opened
 * @return positive file descriptor, or negative error value
 */
int vcon_close_cmd_chan(void)
{
	int ret = 0;
	unsigned long fnode = 0;

	pcimem_deinit(&chan_map_info,
		      (int *)&fnode);
	return ret;
}
