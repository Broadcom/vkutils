/*
 * Copyright 2019 Broadcom
 *
 * This program is the proprietary software of Broadcom and/or
 * its licensors, and may only be used, duplicated, modified or distributed
 * pursuant to the terms and conditions of a separate, written license
 * agreement executed between you and Broadcom (an "Authorized License").
 * Except as set forth in an Authorized License, Broadcom grants no license
 * (express or implied), right to use, or waiver of any kind with respect to
 * the Software, and Broadcom expressly reserves all rights in and to the
 * Software and all intellectual property rights therein.  IF YOU HAVE NO
 * AUTHORIZED LICENSE, THEN YOU HAVE NO RIGHT TO USE THIS SOFTWARE IN ANY
 * WAY, AND SHOULD IMMEDIATELY NOTIFY BROADCOM AND DISCONTINUE ALL USE OF
 * THE SOFTWARE.
 *
 * Except as expressly set forth in the Authorized License,
 *
 * 1. This program, including its structure, sequence and organization,
 * constitutes the valuable trade secrets of Broadcom, and you shall use
 * all reasonable efforts to protect the confidentiality thereof, and to
 * use this information only in connection with your use of Broadcom
 * integrated circuit products.
 *
 * 2. TO THE MAXIMUM EXTENT PERMITTED BY LAW, THE SOFTWARE IS PROVIDED
 * "AS IS" AND WITH ALL FAULTS AND BROADCOM MAKES NO PROMISES,
 * REPRESENTATIONS OR WARRANTIES, EITHER EXPRESS, IMPLIED, STATUTORY, OR
 * OTHERWISE, WITH RESPECT TO THE SOFTWARE.  BROADCOM SPECIFICALLY
 * DISCLAIMS ANY AND ALL IMPLIED WARRANTIES OF TITLE, MERCHANTABILITY,
 * NONINFRINGEMENT, FITNESS FOR A PARTICULAR PURPOSE, LACK OF VIRUSES,
 * ACCURACY OR COMPLETENESS, QUIET ENJOYMENT, QUIET POSSESSION OR
 * CORRESPONDENCE TO DESCRIPTION. YOU ASSUME THE ENTIRE RISK ARISING
 * OUT OF USE OR PERFORMANCE OF THE SOFTWARE.
 *
 * 3. TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT SHALL
 * BROADCOM OR ITS LICENSORS BE LIABLE FOR (i) CONSEQUENTIAL, INCIDENTAL,
 * SPECIAL, INDIRECT, OR EXEMPLARY DAMAGES WHATSOEVER ARISING OUT OF OR
 * IN ANY WAY RELATING TO YOUR USE OF OR INABILITY TO USE THE SOFTWARE EVEN
 * IF BROADCOM HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES; OR (ii)
 * ANY AMOUNT IN EXCESS OF THE AMOUNT ACTUALLY PAID FOR THE SOFTWARE ITSELF
 * OR U.S. $1, WHICHEVER IS GREATER. THESE LIMITATIONS SHALL APPLY
 * NOTWITHSTANDING ANY FAILURE OF ESSENTIAL PURPOSE OF ANY LIMITED REMEDY.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "bcm_vk.h"

/**
 * @file
 * @brief virtual console for valkyrie
 *
 * This is a UART redirect module using the PCIe BAR2 as shared
 * memory for communication.  UART output will be respooled out
 * while a special input channel is also created for injecting
 * commands to the local VK shell/CLIs.
 */

/* start and end commands for virtual console */
#define VCON_ENABLE              "enable"
#define VCON_DISABLE             "disable"

/* local defines */
#define VCON_PROMPT              "\x1B[0mVK_CON # "
#define VCON_MARKER              0xbeefcafe
#define VCON_DEF_MMAP_SIZE       (256 * 1024)
/* cmd buffer is 128 bytes, with 1 byte as header/control */
#define VCON_MAX_CMD_SIZE        127

/* default value, could be overwritten by user */
#define VCON_BUF_BAR2_OFF        0x3800000

#define VCON_IN_CMD_POLL_US      (100 * 1000) /* 100ms */
#define VCON_IN_CMD_POLL_MAX     10 /* max polls before timeout */
#define VCON_OUT_THREAD_SLEEP_US 1000 /* 1ms */

/* command doorbell notification definitions */
#define VKCON_CMD_DB_OFFSET      0x49c
#define VKCON_CMD_DB_VAL         0xFFFFFFF0

static const char *true_false[2] = {"False", "True"};
static bool out_thread_running;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define _PR_LINE(...)                      \
{                                          \
	pthread_mutex_lock(&log_mutex);    \
	printf(__VA_ARGS__);               \
	pthread_mutex_unlock(&log_mutex);  \
}

#define MAX_MMAP_SIZE            (2 * 1024 * 1024)
#define VKCON_RNDUP(_x, _s)      (((_x) + (_s) - 1) & ~((_s) - 1))

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

/**
 * @brief Continuous output thread until the process is gone
 * @param arg thread arg, passed in pointer to log buffer
 */
static void *output_thread(void *arg)
{
	uint32_t rd_idx;
	uint32_t entry_len;
	uint32_t nentries;
	logger_buf *log = arg;
	char *spool_buf;
	char *p_buf;

	/* First, get record of spool idx as rd_idx */

	rd_idx = log->spool_idx;
	entry_len = log->spool_len;
	nentries = log->spool_nentries;

	/* log info to user */
	_PR_LINE("VK Virtual Console Output starts:\n");
	_PR_LINE("  idx %d, len 0x%x, nentries 0x%x\n",
		 rd_idx, entry_len, nentries);

	spool_buf = ((char *)log + log->spool_off);

	while (out_thread_running) {
		/* check marker for PCIe going down */
		if (log->marker != VCON_MARKER) {
			_PR_LINE("Possibly PCIe going down, exit...\n");
			exit(-EINVAL);
		}

		if (rd_idx != log->spool_idx) {
			p_buf = &spool_buf[rd_idx * entry_len];
			_PR_LINE("%s", p_buf);

			rd_idx = (rd_idx + 1) & (nentries - 1);
		}

		if (rd_idx == log->spool_idx)
			usleep(VCON_OUT_THREAD_SLEEP_US);
	}

	/* never reach here, except being killed */
	return NULL;
}

/**
 * @brief Sending a command to the card through PCIe
 * @param fd file descriptor of the device
 * @param cmd_p location to the command channel
 * @param cmd command
 */
static int32_t vcon_send_cmd(int fd, char *cmd_p, const char *cmd)
{
	uint32_t cnt = 0;
	static const uint32_t data = VKCON_CMD_DB_VAL;
	static const struct vk_access io_cmd_notify = {
		.barno  = 0,
		.type   = VK_ACCESS_WRITE,
		.len    = sizeof(uint32_t),
		.offset = VKCON_CMD_DB_OFFSET,
		.data   = (uint32_t *)&data,
	};
	int rc;

	/* first byte is an indicator */
	if (*cmd_p != 0) {
		_PR_LINE("Cmd Channel Not Available when sending cmd!\n");
		return -EBUSY;
	}
	if (cmd[0] == '\0')
		goto tx_success;

	/* cpy the command to the cmd location, and wait for it to be cleared */
	strncpy(cmd_p + 1, cmd, VCON_MAX_CMD_SIZE);
	*cmd_p = 1; /* mark it to be valid - press doorbell */

	/* press door bell */
	rc = ioctl(fd, VK_IOCTL_ACCESS_BAR, &io_cmd_notify);
	if (rc)
		return -EFAULT;

	usleep(VCON_IN_CMD_POLL_US);

	/* loop until doorbell cleared */
	while (++cnt <= VCON_IN_CMD_POLL_MAX) {
		if (*cmd_p == 0)
			break;

		usleep(VCON_IN_CMD_POLL_US);
	}

	if (cnt > VCON_IN_CMD_POLL_MAX)
		return -ETIMEDOUT;
tx_success:
	return 0;
}

/**
 * @brief Continue loop for handling input from user
 * @param fd file descriptor
 * @param p_log_buf pointer to the logger structure
 */
static void vcon_in_cmd_loop(int fd, logger_buf *p_log_buf)
{
	char line[VCON_MAX_CMD_SIZE];
	char *p_char = line;
	int32_t ret;
	size_t in_size;
	char *p_cmd = ((char *)p_log_buf + p_log_buf->cmd_off);

	do {
		_PR_LINE(VCON_PROMPT);
		in_size = sizeof(line);
		ret = getline(&p_char, &in_size, stdin);

		/*
		 * the last character returned will normally be \n - line feed,
		 * replace it with \0 for our consumption on VK
		 */
		if (ret < 0) {
			_PR_LINE("Error reading line from stdin (%s)",
				 strerror(ret));
			break;
		}
		p_char[ret - 1] = '\0';

		/* check for special exit string */
		if (strcmp(p_char, "quit") == 0)
			break;

		/* check marker for PCIe going down */
		if (p_log_buf->marker != VCON_MARKER) {
			_PR_LINE("Possibly PCIe going down, exit...\n");
			break;
		}

		/* send command down */
		ret = vcon_send_cmd(fd, p_cmd, p_char);
		if (ret) {
			_PR_LINE("Send Cmd Failure %s\n, input exits...",
				 strerror(-ret));
			break;
		}
	} while (ret >= 0);
}

#define DEV_DRV_NAME "/dev/bcm_vk"
#define DEV_LEGACY_DRV_NAME "/dev/bcm-vk"

int main(int argc, char **argv)
{
	int c;
	int option_index;
	int32_t ret = -1;
	char dev_name[30];
	char devnode[50];
	char *node_num = NULL;
	bool input_enable = false;
	bool output_enable = false;
	uint32_t mmap_size = VCON_DEF_MMAP_SIZE;
	uint32_t bar2_off = VCON_BUF_BAR2_OFF;
	logger_buf *p_log_buf = NULL;
	pthread_attr_t attr;
	char *p_cmd;
	pthread_t output;
	int fd = -1;

	static struct option long_options[] = {
		{"dev", required_argument, 0, 'd'},
		{"in", no_argument, 0, 'i'},
		{"loc", required_argument, 0, 'l'},
		{"out", no_argument, 0, 'o'},
		{"size", required_argument, 0, 's'},
		{0, 0, 0, 0}
	};

	memset(dev_name, 0, sizeof(dev_name));

	while ((c = getopt_long(argc, argv, "d:i:l:o:s:",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'd':
			if (strlen(optarg) >= sizeof(dev_name)) {
				_PR_LINE("optarg too long for dev_name");
				return -EINVAL;
			}
			strncpy(dev_name, optarg, sizeof(dev_name));
			dev_name[sizeof(dev_name) - 1] = '\0';
			break;
		case 'i':
			if (strcmp(VCON_ENABLE, optarg) == 0)
				input_enable = true;
			break;
		case 'l':
			bar2_off = strtoul(optarg, NULL, 0);
			break;
		case 'o':
			if (strcmp(VCON_ENABLE, optarg) == 0)
				output_enable = true;
			break;
		case 's':
			mmap_size = strtoul(optarg, NULL, 0);
			break;
		default:
			_PR_LINE("%c Not supported", c);
			return -1;
		}
	}

	if ((dev_name[0] == '\0') ||
	    (!input_enable && !output_enable)) {

		_PR_LINE("Parameters Err: Name(%s), input %s output %s\n",
			 dev_name,
			 true_false[input_enable],
			 true_false[output_enable]);
		_PR_LINE("Dev name and at least one of io must be specified\n");

		goto free_and_exit;
	}

	/* open device */
	if (strlen(dev_name) > 3) {
		/* devnode must have been explicitly specified */

		_PR_LINE(" Open, trying devnode %s\n", dev_name);
		fd = open(dev_name, O_RDWR | O_SYNC);

		if (strstr(dev_name, "/dev/bcm-vk.") == dev_name)
			node_num = &dev_name[12];
	} else {
		node_num = dev_name;
	}

	if (fd < 0) {
		_PR_LINE("node_num=%s\n", node_num);

		snprintf(devnode, sizeof(devnode),
			 DEV_DRV_NAME ".%s", node_num);

		_PR_LINE("Open %s\n", devnode);

		fd = open(devnode, O_RDWR | O_SYNC);
		if (fd < 0) {
			/* Try legacy devnode name */
			snprintf(devnode, sizeof(devnode),
				 DEV_LEGACY_DRV_NAME ".%s", node_num);

			_PR_LINE("Open, trying legacy: %s\n", devnode);

			fd = open(devnode, O_RDWR | O_SYNC);
			if (fd < 0) {
				_PR_LINE("Error opening device err %d(%s)\n",
					 fd, strerror(-fd));
				return fd;
			}
		}
	}

	while (p_log_buf == NULL) {

		uint32_t req_size;

		/* do mmap & map it to our struct */
		p_log_buf = mmap(0, mmap_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, bar2_off);
		if ((p_log_buf == MAP_FAILED)
		    || (p_log_buf->marker != VCON_MARKER)) {
			_PR_LINE(
			    "Error mmap size 0x%x from dev %s,%s marker 0x%x\n",
			    mmap_size, dev_name,
			    (p_log_buf == MAP_FAILED) ? "failed mmap" : "",
			    (p_log_buf == MAP_FAILED) ? 0 : p_log_buf->marker);
			return -ENOMEM;
		}

		/*
		 * Do some calculation and see if the mmap region is big enough.
		 * If not, remmap based on new size
		 */
		req_size = p_log_buf->cmd_off + VCON_MAX_CMD_SIZE;
		if (req_size > mmap_size) {

			_PR_LINE(
			  "Region indicated 0x%x greater than mmap size 0x%x\n",
			  req_size, mmap_size);

			munmap(p_log_buf, mmap_size);
			mmap_size = VKCON_RNDUP(req_size, 0x1000);
			p_log_buf = NULL;

			_PR_LINE("Remap region to size 0x%x\n", mmap_size);
			if (mmap_size > MAX_MMAP_SIZE)
				return -ENOMEM;
		}
	}

	/* send down an enable cmd anyway */
	p_cmd = ((char *)p_log_buf + p_log_buf->cmd_off);
	ret = vcon_send_cmd(fd, p_cmd, VCON_ENABLE);
	if (ret) {
		_PR_LINE("Failure to send down enable cmd @start - err %s\n",
			 strerror(-ret));
		goto free_and_exit;
	}

	/* spawn logging thread if output enable */
	if (output_enable) {
		ret = pthread_attr_init(&attr);
		if (ret) {
			_PR_LINE("Error initializing output thread attr! %d\n",
				 ret);
			goto free_and_exit;
		}
		out_thread_running = true;
		ret = pthread_create(&output, &attr, output_thread, p_log_buf);
		if (ret) {
			_PR_LINE("Error creating output thread, %d\n", ret);
			goto free_and_exit;
		}
	}

	/* drop to busy loop just for input  */
	if (input_enable) {
		_PR_LINE("VK Virtual Console Input starts\n");
		vcon_in_cmd_loop(fd, p_log_buf);
		_PR_LINE("VCON Input Exit...\n");
	}

	/* only if both are enabled that we need to wait for output thread */
	if (output_enable) {
		/* if we reach here and input is enabled, user has typed quit */
		if (input_enable)
			out_thread_running = false;
		pthread_join(output, NULL);

	}

	/* do an unmmap */
	munmap(p_log_buf, mmap_size);

free_and_exit:
	return ret;
}
