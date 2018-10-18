/*
 * Copyright 2018 Broadcom
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "bcm_vk.h"

/* local macros */
#define _STR_BASE(_str)     (_str[1] == 'x' ? 16 : 10)

int main(int argc, char *argv[])
{
	char *devnode;
	int fd = -1;
	int rc = -1;
	int i;

	if (argc < 3) {
		printf("Usage: %s /dev/bcm-vk.N <args...>\n", argv[0]);
		printf("Available arguments:\n");
		printf("  gm - get metadata\n");
		printf("  li - load image\n");
		printf("  rb - read bar <barno> <offset>\n");
		printf("  wb - write bar <barno> <offset> <value>\n");
		printf("  reset - reset\n");
		return 0;
	}

	devnode = argv[1];
	fprintf(stdout, "Open %s\n", devnode);
	fflush(stdout);
	fd = open(devnode, O_RDWR);
	if (fd < 0) {
		perror("open failed!");
		fflush(stderr);
		exit(-1);
	}

	for (i = 2; i < argc; i++) {
		char *str = argv[i];

		if (!strcmp(str, "gm")) {
			struct vk_metadata metadata;

			fprintf(stdout, "Get metadata\n");
			fflush(stdout);
			rc = ioctl(fd, VK_IOCTL_GET_METADATA, &metadata);
			if (rc < 0) {
				perror("ioctl failed!");
				fflush(stderr);
			}

			fprintf(stdout, "Metadata version: 0x%x\n",
				metadata.version);
			fprintf(stdout, "Firmware version: 0x%x\n",
				metadata.firmware_version);
			fprintf(stdout, "Card Status: 0x%x\n",
				metadata.card_status);
			fprintf(stdout, "FW Status: 0x%x\n",
				metadata.fw_status);
			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "li")) {
			struct vk_image image;
			char *filename1 = "vk-boot1.bin";
			char *filename2 = "vk-boot2.bin";

			image.type = VK_IMAGE_TYPE_BOOT1;
			image.filename = filename1;

			fprintf(stdout, "Load image\n");
			fflush(stdout);
			rc = ioctl(fd, VK_IOCTL_LOAD_IMAGE, &image);
			if (rc < 0) {
				perror("ioctl failed!");
				fflush(stderr);
			}

			image.type = VK_IMAGE_TYPE_BOOT2;
			image.filename = filename2;

			rc = ioctl(fd, VK_IOCTL_LOAD_IMAGE, &image);
			if (rc < 0) {
				perror("ioctl failed!");
				fflush(stderr);
			}

			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "wb")) {
			struct vk_access access;
			__u32 data[1];
			unsigned long int barno;
			unsigned long long int offset;

			i++;
			str = argv[i];
			barno = strtoul(str, NULL, 10);
			fprintf(stdout, "errno=0x%x\n", errno);
			fprintf(stdout, "barno=%lx\n", barno);

			i++;
			str = argv[i];
			offset = strtoul(str, NULL, _STR_BASE(str));
			fprintf(stdout, "errno=0x%x\n", errno);
			fprintf(stdout, "offset=%llx\n", offset);

			i++;
			str = argv[i];
			data[0] = strtoul(str, NULL, _STR_BASE(str));
			fprintf(stdout, "errno=0x%x\n", errno);
			fprintf(stdout, "data=%x\n", data[0]);

			access.barno = barno;
			access.type = VK_ACCESS_WRITE;
			access.len = sizeof(data);
			access.offset = offset;
			access.data = data;

			fprintf(stdout, "Write Bar\n");
			fflush(stdout);

			rc = ioctl(fd, VK_IOCTL_ACCESS_BAR, &access);
			if (rc < 0) {
				perror("ioctl failed!");
				fflush(stderr);
			}

			fprintf(stdout, "0x%x\n", data[0]);
			fflush(stdout);

			fprintf(stdout, "    access bar done\n");
			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "rb")) {
			struct vk_access access;
			__u32 data[1];
			unsigned long int barno;
			unsigned long long int offset;

			i++;
			str = argv[i];
			barno = strtoul(str, NULL, 10);
			fprintf(stdout, "errno=0x%x\n", errno);
			fprintf(stdout, "barno=%lx\n", barno);

			i++;
			str = argv[i];
			offset = strtoul(str, NULL, _STR_BASE(str));
			fprintf(stdout, "errno=0x%x\n", errno);
			fprintf(stdout, "offset=%llx\n", offset);

			access.barno = barno;
			access.type = VK_ACCESS_READ;
			access.len = sizeof(data);
			access.offset = offset;
			access.data = data;

			fprintf(stdout, "Read Bar\n");
			fflush(stdout);

			rc = ioctl(fd, VK_IOCTL_ACCESS_BAR, &access);
			if (rc < 0) {
				perror("ioctl failed!");
				fflush(stderr);
			}

			fprintf(stdout, "0x%x\n", data[0]);
			fflush(stdout);

			fprintf(stdout, "    access bar done\n");
			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "reset")) {
			struct vk_reset reset;

			reset.arg1 = 4;
			reset.arg2 = 5;

			fprintf(stdout, "Reset\n");
			fflush(stdout);
			rc = ioctl(fd, VK_IOCTL_RESET, &reset);
			if (rc < 0) {
				perror("ioctl failed!");
				fflush(stderr);
			}
			fprintf(stdout, "    reset done\n");
			fflush(stdout);
			continue;
		}

		fprintf(stderr, "Invalid arguments!\n");
		fflush(stderr);
		exit(-1);
	}

	fprintf(stdout, "Close\n");
	fflush(stdout);
	rc = close(fd);
	if (rc < 0) {
		perror("Close failed!");
		fflush(stderr);
		exit(-1);
	}

	return 0;
}

