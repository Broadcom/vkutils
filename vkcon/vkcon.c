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
#include "version.h"
#include "vkutil_msg.h"

/**
 * @file
 * @brief virtual console for valkyrie
 *
 * This is a UART redirect module using the PCIe BAR2 as shared
 * memory for communication.  UART output will be respooled out
 * while a special input channel is also created for injecting
 * commands to the local VK shell/CLIs.
 */

/* local defines */
#define VKCON_PROMPT			"\x1B[0mVK_CON # "
#define VKCON_OUT_THREAD_SLEEP_US	10000  /* 10ms */
#define VKCON_THREAD_CREATION_DELAY	500000 /* half a sec */
#define VKCON_OUT_BUF_SIZE		(2 * 1024)

static const char *true_false[2] = {"False", "True"};
static bool out_thread_running;

/**
 * @brief Continuous output thread until the process is gone
 * @param arg thread arg, passed in device context
 */
static void *output_thread(void *arg)
{
	char buf[VKCON_OUT_BUF_SIZE];
	int ret;

	/* log info to user */
	_PR_LINE("VK Virtual Console Output starts:\n");
	while (out_thread_running) {

		ret = vcon_get_cmd_output(arg, buf, sizeof(buf));
		if (ret == 0) {
			usleep(VKCON_OUT_THREAD_SLEEP_US);
		} else if (ret < 0) {
			_PR_LINE("Get command output failure - %s(%d)\n",
				 strerror(-ret), ret);
			if (ret == -EACCES)
				_PR_LINE("Possibly PCIe going down, exit...\n");
			exit(-EINVAL);
		} else {
			int i;

			pthread_mutex_lock(&log_mutex);
			for (i = 0; i < ret; i++)
				_PR_LINE("%c", buf[i]);
			pthread_mutex_unlock(&log_mutex);
		}
	}

	/* never reach here, except being killed */
	return NULL;
}

/**
 * @brief Continue loop for handling input from user
 * @param dev device context
 * @param p_log_buf pointer to the logger structure
 */
static void vcon_in_cmd_loop(void *ctx)
{
	char line[VCON_MAX_CMD_SIZE];
	char *p_char = line;
	int32_t ret;
	size_t in_size;

	do {
		_PR_LINE(VKCON_PROMPT);
		in_size = sizeof(line);
		ret = getline(&p_char, &in_size, stdin);

		/*
		 * the last character returned will normally be \n - line feed,
		 * replace it with \0 for our consumption on VK
		 */
		if (ret < 0) {
			_PR_LINE("Error reading line from stdin - (%s)%d\n",
				 strerror(-ret), ret);
			break;
		}
		p_char[ret - 1] = '\0';

		/* check for special exit string */
		if (strcmp(p_char, "quit") == 0)
			break;

		ret = vcon_send_cmd(ctx, p_char);
		if (ret < 0) {
			_PR_LINE("Send Cmd Failure - %s(%d)\n",
				 strerror(-ret), ret);
			if (ret == -EACCES)
				_PR_LINE("Possibly PCIe going down, exit...\n");
			break;
		}
	} while (ret >= 0);
}

int main(int argc, char **argv)
{
	int c;
	char dev_name[FNAME_LEN];
	int option_index;
	int32_t ret = -1;
	bool input_enable = false;
	bool output_enable = false;
	pthread_attr_t attr;
	size_t mmapped_size;
	pthread_t output;
	void *p_ctx_con;

	static struct option long_options[] = {
		{"dev", required_argument, 0, 'd'},
		{"in", no_argument, 0, 'i'},
		{"out", no_argument, 0, 'o'},
		{"version", no_argument, 0, 'v'},
		{0, 0, 0, 0}
	};

	memset(dev_name, 0, sizeof(dev_name));

	while ((c = getopt_long(argc, argv, "d:i:o:s:v:",
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
		case 'o':
			if (strcmp(VCON_ENABLE, optarg) == 0)
				output_enable = true;
			break;
		case 'v':
			_PR_LINE("%s version %s.%s.%s\n",
				 argv[0],
				 PKG_VERSION_MAJOR,
				 PKG_VERSION_MINOR,
				 PKG_VERSION_PATCH);
			/*
			 * version query cannot be combined
			 * with other commands. Exit after reporting
			 */
			return 0;
		default:
			_PR_LINE("%c Not supported", c);
			return -EINVAL;
		}
	}

	if ((dev_name[0] == '\0') ||
	    (!input_enable && !output_enable)) {

		_PR_LINE("Parameters Err: Name(%s), input %s output %s\n",
			 dev_name,
			 true_false[input_enable],
			 true_false[output_enable]);
		_PR_LINE("Dev name and at least one of io must be specified\n");

		return -EINVAL;
	}
	ret = vcon_open_cmd_chan(&p_ctx_con,
				 dev_name,
				 &mmapped_size);
	if (ret < 0) {
		_PR_LINE("Fail to open communication channel - %s(%d)\n",
			 strerror(-errno), errno);
		return -EINVAL;
	}

	_PR_LINE("VKCON cmd chan open successful - size %ld\n",
		 mmapped_size);

	/* send down an enable cmd anyway */
	ret = vcon_send_cmd(p_ctx_con, VCON_ENABLE);
	if (ret < 0) {
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
		ret = pthread_create(&output, &attr, output_thread, p_ctx_con);
		if (ret) {
			_PR_LINE("Error creating output thread, %d\n", ret);
			goto free_and_exit;
		}
	}

	/* just delay a bit to allow output thread to have its first breath */
	usleep(VKCON_THREAD_CREATION_DELAY);

	/* drop to busy loop just for input  */
	if (input_enable) {
		_PR_LINE("VK Virtual Console Input starts:\n");
		vcon_in_cmd_loop(p_ctx_con);
		_PR_LINE("VCON Input Exit...\n");
	}

	/* only if both are enabled that we need to wait for output thread */
	if (output_enable) {
		/* if we reach here and input is enabled, user has typed quit */
		if (input_enable)
			out_thread_running = false;
		pthread_join(output, NULL);

	}

free_and_exit:
	if (ret != -EACCES) {
		ret = vcon_send_cmd(p_ctx_con, VCON_DISABLE);
		if (ret < 0) {
			_PR_LINE("VCON_DISABLE Send Cmd Failure - %s(%d)\n",
				 strerror(-ret), ret);
		}
	}

	/* close communication channel in the end */
	ret = vcon_close_cmd_chan(p_ctx_con);
	if (ret < 0)
		_PR_LINE("Error closing channel. handle %p - %s(%d)\n",
			 p_ctx_con, strerror(-errno), errno);
	return ret;
}
