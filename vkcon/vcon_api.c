// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2019 Broadcom
 */

#include <errno.h>
#include <fcntl.h>
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
#define VCON_RNDUP(_x, _s)       (((_x) + (_s) - 1) & ~((_s) - 1))

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

/**
 * @brief Read data to the buffer passed in by caller
 * @param buf pointer to output buf
 * @param buf_size size of the output buffer
 * @Return number of bytes that have been extracted, including '\0', negative
 *         number for error conditions
 *
 * NOTE: the output buffer will always has a complete log.  If the buffer could
 *        not fit a line in, the line will stay intact until the next polling.
 */
int vcon_get_cmd_output(char *buf, const size_t buf_size)
{
	int ret = 0, cnt;
	uint32_t entry_len;
	uint32_t nentries;
	char *spool_buf;
	char *p_buf = buf;
	char *p_line;

	if (p_log_buf->marker != VCON_MARKER)
		return -EACCES;

	entry_len = p_log_buf->spool_len;
	nentries = p_log_buf->spool_nentries;
	spool_buf = ((char *)p_log_buf + p_log_buf->spool_off);

	if (buf_size < entry_len)
		return -E2BIG;

	while ((rd_idx != p_log_buf->spool_idx) &&
	       ((ret + entry_len) <= buf_size)) {

		/* check marker for PCIe going down */
		if (p_log_buf->marker != VCON_MARKER)
			return -EACCES;

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
	uint32_t cnt = 0;
	static const uint32_t data = VCON_CMD_DB_VAL;
	static const struct vk_access io_cmd_notify = {
		.barno  = 0,
		.type   = VK_ACCESS_WRITE,
		.len    = sizeof(uint32_t),
		.offset = VCON_CMD_DB_OFFSET,
		.data   = (uint32_t *)&data,
	};
	int rc;
	char *cmd_chan;

	cmd_chan = ((char *)p_log_buf + p_log_buf->cmd_off);

	if (cmd[0] == '\0')
		goto tx_success;

	if (p_log_buf->marker != VCON_MARKER)
		return -EACCES;


	/* first byte is an indicator */
	if (*cmd_chan != VCON_CMD_CHAN_FREE)
		return -EBUSY;

	/* cpy the command to the cmd location, and wait for it to be cleared */
	strncpy(cmd_chan + 1, cmd, VCON_MAX_CMD_SIZE);
	cmd_chan[VCON_MAX_CMD_SIZE] = '\0';
	*cmd_chan = VCON_CMD_CHAN_OCCUPIED; /* mark it to be valid */

	/* press door bell */
	rc = ioctl(fd, VK_IOCTL_ACCESS_BAR, &io_cmd_notify);
	if (rc)
		return -EFAULT;

	usleep(VCON_IN_CMD_POLL_US);

	/* loop until doorbell cleared */
	while (++cnt <= VCON_IN_CMD_POLL_MAX) {
		if (*cmd_chan == VCON_CMD_CHAN_FREE)
			break;

		usleep(VCON_IN_CMD_POLL_US);
	}

	if (cnt > VCON_IN_CMD_POLL_MAX)
		return -ETIMEDOUT;
tx_success:
	return 0;
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
	char devnode[50];
	char *node_num = NULL;
	uint32_t bar2_off = (offset) ? offset : VCON_BUF_BAR2_OFF;
	int fd = -1;
	int ret = 0;

	/* open device */
	if (strlen(dev_name) > 3) {
		node_num = strstr(dev_name, ".");
		if (!node_num)
			return -EINVAL;
		node_num++;
	} else {
		node_num = (char *)dev_name;
	}

	snprintf(devnode, sizeof(devnode), DEV_DRV_NAME ".%s", node_num);
	devnode[sizeof(devnode) - 1] = '\0';
	fd = open(devnode, O_RDWR | O_SYNC);
	if (fd < 0) {
		/* try alternate name */
		snprintf(devnode, sizeof(devnode), DEV_ALT_DRV_NAME ".%s",
			 node_num);
		devnode[sizeof(devnode) - 1] = '\0';
		fd = open(devnode, O_RDWR | O_SYNC);
		if (fd < 0)
			return fd;
	}

	while (p_log_buf == NULL) {
		uint32_t req_size;

		/* do mmap & map it to our struct */
		p_log_buf = mmap(0, mmap_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, bar2_off);
		if ((p_log_buf == MAP_FAILED)
		    || (p_log_buf->marker != VCON_MARKER)) {
			ret = -EACCES;
			goto fail;
		}

		/*
		 * Do some calculation and see if the mmap region is big enough.
		 * If not, remmap based on new size
		 */
		req_size = p_log_buf->cmd_off + VCON_CMD_CHAN_SIZE;
		if (req_size > mmap_size) {

			munmap(p_log_buf, mmap_size);
			mmap_size = VCON_RNDUP(req_size, 0x1000);
			p_log_buf = NULL;

			if (mmap_size > MAX_MMAP_SIZE) {
				ret = -ENOMEM;
				goto fail;
			}
		}
	}

	*p_mapped_size = mmap_size;
	rd_idx = p_log_buf->spool_idx;
	return fd;

fail:
	close(fd);
	return ret;
}

/**
 * @brief Close the command channel
 * @param fd file descriptor that has been opened
 * @return positive file descriptor, or negative error value
 */
int vcon_close_cmd_chan(int fd)
{
	int ret;

	ret = munmap(p_log_buf, mmap_size);
	close(fd);

	return ret;
}
