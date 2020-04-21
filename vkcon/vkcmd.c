// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2019 Broadcom
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
#include "vcon_api.h"
#include "vcon_chan_intf.h"

/**
 * @file
 * @brief single shell cmd executor for valkyrie
 *
 * This is a simple single command utilty for the Valkyrie card
 */

/* start and end commands for virtual console */
#define VKCMD_SLEEP_US			10000     /* 10ms */
#define VKCMD_OUT_BUF_SIZE		(2 * 1024)
#define VKCMD_MIN_DURATION_SEC		1
#define VCON_CMD_DB_OFFSET		0x49c

/* local macros */
#define _PR_LINE(...)                       \
{                                           \
	printf(__VA_ARGS__);                \
}

#define _ELAPSED_MS(_end, _st) \
	((_end.tv_sec - _st.tv_sec) * 1000L + \
	 (_end.tv_nsec - _st.tv_nsec) / 1000000L)

int main(int argc, char **argv)
{
	int c;
	int option_index;
	int32_t ret = -1, rc;
	char *dev_name = NULL;
	char *cmd = NULL;
	uint period = VKCMD_MIN_DURATION_SEC * 1000;
	char buf[VKCMD_OUT_BUF_SIZE];
	struct timespec start_time, end_time;
	size_t mmapped_size;
	void *p_ctx_con;

	static struct option long_options[] = {
		{"dev", required_argument, 0, 'd'},
		{"in", no_argument, 0, 'i'},
		{"out", no_argument, 0, 'o'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "c:d:s:",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'c':
			cmd = optarg;
			break;

		case 'd':
			dev_name = optarg;
			break;
		case 's':
			period = strtoul(optarg, NULL, 0);
			if (period < VKCMD_MIN_DURATION_SEC)
				period = VKCMD_MIN_DURATION_SEC;
			period *= 1000;
			break;
		default:
			_PR_LINE("%c Not supported", c);
			return -EINVAL;
		}
	}

	if (!dev_name || !cmd) {
		_PR_LINE("Parameters Err: Device name %s Cmd %s\n",
			 dev_name, cmd);
		return -EINVAL;
	}

	ret = vcon_open_cmd_chan(&p_ctx_con,
				 dev_name,
				 &mmapped_size);
	if (ret < 0) {
		_PR_LINE("Fail to open command channel - %s(%d)",
			 strerror(-errno), errno);
		return -EINVAL;
	}
	_PR_LINE("VKCMD: %s @dev %s running %d ms \n",
		 cmd, dev_name, period);

	/* turn off coloring scheme */
	ret = vcon_send_cmd(p_ctx_con, VCON_COLOR_OFF);
	if (ret) {
		_PR_LINE("Failure to turn color off, abort!\n");
		goto free_and_exit;
	}

	/* send down cmd */
	ret = vcon_send_cmd(p_ctx_con, cmd);
	if (ret) {
		_PR_LINE("Failure to send down enable cmd @start - err %s\n",
			 strerror(-ret));
		goto free_and_exit;
	}

	/* get start time and loop until reaching specified period */
	clock_gettime(CLOCK_MONOTONIC, &start_time);
	end_time = start_time;
	while (_ELAPSED_MS(end_time, start_time) < period) {
		int i;

		ret = vcon_get_cmd_output(p_ctx_con, buf, sizeof(buf));
		if (ret < 0) {
			_PR_LINE("Error getting data from card, exit...\n");
			goto free_and_exit;
		}

		for (i = 0; i < ret; i++)
			_PR_LINE("%c", buf[i]);

		usleep(VKCMD_SLEEP_US);
		clock_gettime(CLOCK_MONOTONIC, &end_time);
	}

	/* normal exit */
	_PR_LINE("VKCMD: ends...\n");

	/* turn color back on */
	ret = vcon_send_cmd(p_ctx_con, VCON_COLOR_ON);
	if (ret) {
		_PR_LINE("Failure to turn color back ON!\n");
		goto free_and_exit;
	}
	ret = 0;

free_and_exit:
	if (ret)
		_PR_LINE("Error to exit - %s(%d)\n", strerror(-ret), ret);

	/* close command channel in the end */
	rc = vcon_close_cmd_chan(p_ctx_con);
	if (rc)
		_PR_LINE("Error closing channel. handle %p - %s(%d)\n",
			 p_ctx_con, strerror(-errno), errno);
	return ret;
}
