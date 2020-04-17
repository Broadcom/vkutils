// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2020 Broadcom
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "vcon_chan_intf.h"

#define _PR_LINE	printf
#define banner		"==============================================\n"

/* checks for limits */
#define MAX_NENTRIES    (10 * 1024)
#define MAX_ENTRY_LEN   (512)

int main(int argc, char **argv)
{
	FILE *fp;
	char f_name[FNAME_LEN] = { 0 };
	char c;
	int option_index;
	long offset = VCON_BUF_BAR2_OFF;
	logger_buf log;
	int ret, idx, i;

	static struct option long_options[] = {
		{"file", required_argument, 0, 'f'},
		{0, 0, 0, 0}
	};

	while ((c = getopt_long(argc, argv, "f:",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'f':
			if (strlen(optarg) >= sizeof(f_name)) {
				_PR_LINE("optarg too long for file name");
				return -EINVAL;
			}
			strncpy(f_name, optarg, sizeof(f_name));
			f_name[sizeof(f_name) - 1] = '\0';
			break;
		default:
			_PR_LINE("%c Not supported", c);
			return -EINVAL;
		}
	}

	if (f_name[0] == '\0') {
		_PR_LINE("Usage: %s -f <file name>\n", argv[0]);
		return -EINVAL;
	}

	fp = fopen(f_name, "r");
	if (!fp) {
		_PR_LINE("Fail to open file %s\n", f_name);
		return -EINVAL;
	}

	ret = fseek(fp, offset, SEEK_SET);
	if (ret < 0) {
		_PR_LINE("Fail to locate spool buffer\n");
		goto fail;
	}

	/* read the structure */
	ret = fread(&log, sizeof(logger_buf), 1, fp);
	if (ret <= 0) {
		_PR_LINE("Fail reading logger structure\n");
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
		_PR_LINE("Dump file invalid....exit!\n");
		return -EINVAL;
	}

	idx = (log.spool_idx + 1) & (log.spool_nentries - 1);

	for (i = 0; i < log.spool_nentries; i++) {
		char oneline[log.spool_len];

		ret = fseek(fp,
			    offset + log.spool_off + idx * log.spool_len,
			    SEEK_SET);
		if (ret < 0) {
			_PR_LINE("Locating entry[%d] fails - %s",
				 idx, strerror(errno));
			goto fail;
		}

		/* read line and dump */
		ret = fread(oneline, sizeof(oneline), 1, fp);
		if (ret < 0) {
			_PR_LINE("Error reading entry[%d] - %s",
				 idx, strerror(errno));
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
