// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020 Broadcom
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "vcon_api.h"
#include "vkutil_msg.h"

#define _PR_LINE	printf
#define banner		"==============================================\n"

/* checks for limits */
#define MAX_NENTRIES    (10 * 1024)
#define MAX_ENTRY_LEN   (512)
#define MAX_CONSOLE_LEN (1 * 1024 * 1024)

static int parse_console_log(char *f_name)
{
	char e_msg[MAX_ERR_MSG] = "";
	FILE *fp;
	int ret;
	long rd_idx;
	long wr_idx;
	char c;
	console_buf clog;

	fp = fopen(f_name, "r");
	if (!fp) {
		PERROR("Fail to open file; error: %d\n", errno);
		return errno;
	}

	ret = fseek(fp, 0, SEEK_SET);
	if (ret < 0) {
		PERROR("Console header fseek failed: %d\n", ret);
		goto fail;
	}

	ret = fread(&clog, sizeof(console_buf), 1, fp);
	if (ret <= 0) {
		PERROR("Console header fread failed: %d\n", ret);
		goto fail;
	}

	_PR_LINE(banner);
	_PR_LINE("File %s - console buffer located\n", f_name);
	_PR_LINE("  ==> size %d wr_idx %d rd_idx %d\n",
		 clog.size, clog.wr, clog.rd);
	_PR_LINE(banner);

	/* sanity check the header info */
	if (clog.size != (MAX_CONSOLE_LEN - CONSOLE_ADDR(data))) {
		PERROR("Console header size is invalid!\n");
		ret = -EINVAL;
		goto fail;
	}

	if (clog.rd > clog.size || clog.wr > clog.size) {
		PERROR("Console header wr idx or/and rd idx invalid!\n");
		ret = -EINVAL;
		goto fail;
	}

	/* reset the file pointer to read from last write index */
	wr_idx = clog.wr + CONSOLE_ADDR(data);
	rd_idx = wr_idx + 1;
	rd_idx =  (rd_idx >= MAX_CONSOLE_LEN ? CONSOLE_ADDR(data) : rd_idx);
	ret = fseek(fp, rd_idx, SEEK_SET);
	if (ret < 0) {
		PERROR("Console data fseek failed: %d\n", ret);
		goto fail;
	}

	while (rd_idx != wr_idx) {
		c = fgetc(fp);

		/* eof reached but not end of data, so loop back to start */
		if (rd_idx >= MAX_CONSOLE_LEN || feof(fp)) {
			rd_idx = CONSOLE_ADDR(data);
			ret = fseek(fp, rd_idx, SEEK_SET);
			if (ret < 0) {
				PERROR("Console fseek at failed: %d\n", ret);
				goto fail;
			}
		}
		rd_idx++;
		_PR_LINE("%c", c);
	}
	printf("\n\n");

fail:
	fclose(fp);
	return ret;
}

static int parse_logger(char *f_name)
{
	char e_msg[MAX_ERR_MSG] = "";
	FILE *fp;
	long offset = VCON_BUF_BAR2_OFF;
	logger_buf log;
	int ret, idx, i;

	fp = fopen(f_name, "r");
	if (!fp) {
		PERROR("Fail to open file %d\n", errno);
		return -EINVAL;
	}

	ret = fseek(fp, offset, SEEK_SET);
	if (ret < 0) {
		PERROR("Fail to locate spool buffer\n");
		goto fail;
	}

	/* read the structure */
	ret = fread(&log, sizeof(logger_buf), 1, fp);
	if (ret <= 0) {
		PERROR("Fail reading logger structure\n");
		goto fail;
	}

	_PR_LINE(banner);
	_PR_LINE("File %s - spool buffer located\n", f_name);
	_PR_LINE("  => entries 0x%x, idx %d len %d off 0x%x (marker 0x%x)\n",
		 log.spool_nentries,
		 log.spool_idx,
		 log.spool_len,
		 log.spool_off,
		 log.marker);
	_PR_LINE("  => Last [%d] - offset 0x%lx\n", log.spool_idx,
		 offset + log.spool_off + log.spool_idx * log.spool_len);
	_PR_LINE(banner);

	/* sanity check on values */
	if ((log.marker != VCON_MARKER) ||
	    !log.spool_len ||
	    (log.spool_nentries > MAX_NENTRIES) ||
	    (log.spool_len > MAX_ENTRY_LEN)) {
		PERROR("Fail: invalid logger header!\n");
		ret = -EINVAL;
		goto fail;
	}

	idx = (log.spool_idx + 1) & (log.spool_nentries - 1);

	for (i = 0; i < log.spool_nentries; i++) {
		char oneline[log.spool_len];

		ret = fseek(fp,
			    offset + log.spool_off + idx * log.spool_len,
			    SEEK_SET);
		if (ret < 0) {
			PERROR("Locating entry[%d] fails - %d",
			       idx, ret);
			goto fail;
		}

		/* read line and dump */
		ret = fread(oneline, sizeof(oneline), 1, fp);
		if (ret < 0) {
			PERROR("Error reading entry[%d] - %d",
			       idx, ret);
			goto fail;
		}

		if (oneline[0] != '\0') {
			oneline[log.spool_len - 1] = '\0';
			_PR_LINE("<%4d> %s", idx, oneline);
		}
		idx = (idx + 1) & (log.spool_nentries - 1);
	}
fail:
	fclose(fp);
	return ret;
}

static void usage(char *name)
{
	_PR_LINE("Usage:\n");
	_PR_LINE("\t%s -f <logger file name>\n", name);
	_PR_LINE("\t%s -c <console log file name>\n", name);
}

int main(int argc, char **argv)
{
	char e_msg[MAX_ERR_MSG] = "";
	char f_name[FNAME_LEN] = { 0 };
	char c;
	int option_index;
	int infile_f = 0;
	int infile_c = 0;

	static struct option long_options[] = {
		{"file", required_argument, 0, 'f'},
		{"console", required_argument, 0, 'c'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "f:c:",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'f':
			if (strlen(optarg) >= sizeof(f_name)) {
				PERROR("optarg too long for file name\n");
				return -EINVAL;
			}
			strncpy(f_name, optarg, sizeof(f_name));
			f_name[sizeof(f_name) - 1] = '\0';
			infile_f = 1;
			break;
		case 'c':
			if (strlen(optarg) >= sizeof(f_name)) {
				PERROR("optarg too long for file name\n");
				return -EINVAL;
			}
			strncpy(f_name, optarg, sizeof(f_name));
			f_name[sizeof(f_name) - 1] = '\0';
			infile_c = 1;
			break;
		default:
			PERROR("%c Not supported\n", c);
			usage(argv[0]);
			return -EINVAL;
		}
	}

	if (infile_f && infile_c) {
		PERROR("Error: only one input file can be specified!\n");
		usage(argv[0]);
		return -EINVAL;
	}

	if (f_name[0] == '\0') {
		usage(argv[0]);
		return -EINVAL;
	}

	if (infile_c)
		parse_console_log(f_name);

	if (infile_f)
		parse_logger(f_name);

	return 0;
}
