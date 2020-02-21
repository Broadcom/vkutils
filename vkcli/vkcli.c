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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "bcm_vk.h"

/* local macros */
#define _STR_BASE(_str)	(_str[1] == 'x' ? 16 : 10)

#define FNAME_LEN	128
#define OPTION_LEN	8
#define MAX_FILESIZE	0x4000000	/* 64MB */
#define MAX_ERR_MSG	255

#define DEV_DRV_NAME "/dev/bcm_vk"
#define DEV_LEGACY_DRV_NAME "/dev/bcm-vk"
#define PERROR(...) do {\
			snprintf(e_msg, MAX_ERR_MSG, __VA_ARGS__);\
			fprintf(stderr, " @L:%d %s\n", __LINE__, e_msg);\
			fflush(stderr);\
			} while (0)


/* strtoul wrapper function for unified error handling */
static int string2ul(char *str, unsigned long *return_value)
{
	char *endptr = NULL;

	if (str == NULL || return_value == NULL)
		return -EINVAL;
	*return_value = strtoul(str, &endptr, _STR_BASE(str));
	if (endptr == str)
		return -EFAULT;
	if ((*return_value == LONG_MAX ||
		*return_value == LONG_MIN) && errno == ERANGE)
		return -ERANGE;

	return 0;
}

void print_usage(char **argv)
{
	printf("Usage: %s <node_num> <args...>\n", argv[0]);
	printf("Available arguments:\n");
	printf("  li - load image [-/boot1/boot2] [fname1] [fname2]\n");
	printf("     '-' -- load both stages (both boot1 and boot2)\n");
	printf("     'boot1' -- only first stage (boot1)\n");
	printf("     'boot2' -- only second stage (boot2)\n");
	printf("  rb - read bar <barno> <offset>\n");
	printf("  rf - read to file <barno> <offset> <len> file\n");
	printf("  wb - write bar <barno> <offset> <value>\n");
	printf("  wf - write from file <barno> <offset> file\n");
	printf("  reset - reset\n");
}

int main(int argc, char *argv[])
{
	char devnode[50];
	char e_msg[MAX_ERR_MSG] = "";
	char *node_num = NULL;
	int fd = -1;
	int rc = -1;
	int i;
	int err = 0;

	if (argc < 3) {
		print_usage(argv);
		return 0;
	}

	if (strlen(argv[1]) > 3) {
		/* devnode must have been explicitly specified */

		fprintf(stdout, " Open, trying devnode %s\n", argv[1]);
		fflush(stdout);
		fd = open(argv[1], O_RDWR);

		if (strstr(argv[1], "/dev/bcm-vk.") == argv[1])
			node_num = &argv[1][12];
	} else {
		node_num = argv[1];
	}

	if (fd < 0) {
		fprintf(stdout, "node_num=%s\n", node_num);
		fflush(stdout);

		snprintf(devnode, sizeof(devnode),
			 DEV_DRV_NAME ".%s", node_num);

		fprintf(stdout, "Open %s\n", devnode);
		fflush(stdout);
		fd = open(devnode, O_RDWR);
		if (fd < 0) {
			/* Try legacy devnode name */
			snprintf(devnode, sizeof(devnode),
				 DEV_LEGACY_DRV_NAME ".%s", node_num);

			fprintf(stdout, "Open, trying legacy: %s\n", devnode);
			fflush(stdout);
			fd = open(devnode, O_RDWR);
			if (fd < 0) {
				PERROR("%s open failed; err=0x%x",
				       devnode, errno);
				exit(errno);
			}
		}
	}

	for (i = 2; i < argc; i++) {
		char *str = argv[i];

		if (!strcmp(str, "li")) {
			struct vk_image image;
			char method[OPTION_LEN] = "-";
			char filename1[FNAME_LEN] = "vk-boot1.bin";
			char filename2[FNAME_LEN] = "vk-boot2.bin";

			if (argc >= 4) {
				/* method */
				i++;
				str = argv[i];
				strncpy(method, str, sizeof(method));
				method[OPTION_LEN - 1] = '\0';
				fprintf(stdout, "method=%s\n", method);
			}
			if (argc >= 5) {
				/* file name 1 defined */
				i++;
				str = argv[i];
				strncpy(filename1, str, sizeof(filename1));
				filename1[FNAME_LEN - 1] = '\0';
				fprintf(stdout, "file name 1=%s\n", filename1);
			}
			if (argc >= 6) {
				/* file name 2 defined */
				i++;
				str = argv[i];
				strncpy(filename2, str, sizeof(filename2));
				filename2[FNAME_LEN - 1] = '\0';
				fprintf(stdout, "file name 2=%s\n", filename2);
			}

			if (!strcmp(method, "boot1")) {
				filename2[0] = '\0';
			} else if (!strcmp(method, "boot2")) {
				/* If user specified file name,
				 * the 1st filename will be used for boot2
				 */
				if (!strcmp(filename1, "vk-boot1.bin"))
					strcpy(filename2, "vk-boot2.bin");
				else
					strcpy(filename2, filename1);
				filename1[0] = '\0';
			}

			if (strcmp(filename1, "")) {
				image.type = VK_IMAGE_TYPE_BOOT1;
				if (strlen(filename1)
				    >= sizeof(image.filename)) {
					PERROR("filename1 > max %ld",
					       sizeof(image.filename));
					exit(EINVAL);
				}
				strncpy(image.filename,
					filename1,
					sizeof(image.filename));
				image.filename[sizeof(image.filename) - 1]
					= '\0';

				fprintf(stdout, "Load image boot1 %s\n",
					filename1);
				fflush(stdout);
				rc = ioctl(fd, VK_IOCTL_LOAD_IMAGE, &image);
				if (rc < 0) {
					PERROR("VK_IOCTL_LOAD_IMAGE 0x%x",
						rc);
					exit(rc);
				}
			}

			if (strcmp(filename2, "")) {
				image.type = VK_IMAGE_TYPE_BOOT2;
				if (strlen(filename2)
				    >= sizeof(image.filename)) {
					PERROR("filename2 > max %ld",
					       sizeof(image.filename));
					exit(EINVAL);
				}
				strncpy(image.filename,
					filename2,
					sizeof(image.filename));
				image.filename[sizeof(image.filename) - 1]
					= '\0';

				fprintf(stdout, "Load image boot2 %s\n",
					filename2);
				fflush(stdout);
				rc = ioctl(fd, VK_IOCTL_LOAD_IMAGE, &image);
				if (rc < 0) {
					PERROR("VK_IOCTL_LOAD_IMAGE 0x%x",
						rc);
					exit(rc);
				}
			}

			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "wb")) {
			struct vk_access access;
			__u32 data[1];
			unsigned long int barno;
			unsigned long long int offset;

			/*
			 * make sure we have enough parameter before moving on,
			 * need 3 more parameters including bar_no, offset and
			 * value
			 */
			if ((i + 3) >= argc) {
				print_usage(argv);
				PERROR("Not enough parameters for wb");
				exit(EINVAL);
			}

			i++;
			str = argv[i];
			err = string2ul(str, &barno);
			if (err < 0) {
				PERROR("bad barno %s err=0x%x",
					str, errno);
				exit(errno);
			}
			fprintf(stdout, "barno=%ld\n", barno);

			i++;
			str = argv[i];
			err = string2ul(str, (unsigned long *)&offset);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad offset %s err=0x%x",
				       str, errno);
				exit(errno);
			}
			fprintf(stdout, "offset=%llx\n", offset);

			i++;
			str = argv[i];
			err = string2ul(str, (unsigned long *)&data[0]);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad value %s err=0x%x",
					str, errno);
				exit(errno);
			}
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
				PERROR("VK_IOCTL_ACCESS_BAR 0x%x",
					rc);
				exit(rc);
			}

			fprintf(stdout, "    access bar done\n");
			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "wf")) {
			struct vk_access access;
			char fname[FNAME_LEN];
			FILE *pfile;
			long fsize;
			char *buffer;
			unsigned long int barno;
			unsigned long long int offset;

			i++;
			str = argv[i];
			err = string2ul(str, &barno);
			if (err < 0) {
				PERROR("bad barno %s err=0x%x",
				       str, errno);
				exit(errno);
			}
			fprintf(stdout, "barno=%ld\n", barno);

			i++;
			str = argv[i];
			err = string2ul(str, (unsigned long *)&offset);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad offset %s err=0x%x",
					str, errno);
				exit(errno);
			}
			fprintf(stdout, "offset=%llx\n", offset);

			i++;
			str = argv[i];
			strncpy(fname, str, sizeof(fname));
			fname[FNAME_LEN - 1] = '\0';
			fprintf(stdout, "fname=%s\n", fname);

			pfile = fopen(fname, "r");
			if (pfile == NULL) {
				PERROR("fopen %s err=0x%x",
					fname, errno);
				exit(errno);
			}

			/* obtain file size */
			fseek(pfile, 0, SEEK_END);
			fsize = ftell(pfile);
			rewind(pfile);
			fprintf(stdout, "file size=%ld\n", fsize);
			if (fsize <= 0 || fsize > MAX_FILESIZE) {
				PERROR("bad file size %s (%ld)",
					fname, fsize);
				fclose(pfile);
				exit(errno);
			}

			/* allocation buffer to contain all file */
			buffer = (char *)malloc(fsize);
			if (buffer == NULL) {
				PERROR("bad alloc %ld bytes err=0x%x",
					fsize, errno);
				fclose(pfile);
				exit(errno);
			}

			/* copy file into the buffer */
			rc = fread(buffer, 1, fsize, pfile);
			if (rc != fsize) {
				PERROR("fread %s err=0x%x",
					fname, errno);
				fclose(pfile);
				free(buffer);
				exit(errno);
			}

			access.barno = barno;
			access.type = VK_ACCESS_WRITE;
			access.len = sizeof(char)*fsize;
			access.offset = offset;
			access.data = (__u32 *)buffer;

			fprintf(stdout, "Write File to Bar\n");
			fflush(stdout);

			rc = ioctl(fd, VK_IOCTL_ACCESS_BAR, &access);
			if (rc < 0) {
				PERROR("VK_IOCTL_ACCESS_BAR 0x%x",
					rc);
				exit(rc);
			}

			fprintf(stdout, "    access bar done\n");
			fflush(stdout);

			fclose(pfile);
			free(buffer);
			continue;
		}

		if (!strcmp(str, "rb")) {
			struct vk_access access;
			__u32 data[1];
			unsigned long int barno;
			unsigned long long int offset;

			/*
			 * make sure we have enough parameters before moving on,
			 * need 2 more, bar_no and offset.
			 */
			if ((i + 2) >= argc) {
				print_usage(argv);
				PERROR("Not enough parameters for rb");
				exit(EINVAL);
			}

			i++;
			str = argv[i];
			err = string2ul(str, &barno);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad barno %s err=0x%x",
					str, errno);
				exit(errno);
			}
			fprintf(stdout, "barno=%ld\n", barno);

			i++;
			str = argv[i];
			err = string2ul(str, (unsigned long *)&offset);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad parameter %s err=0x%x",
					str, errno);
				exit(errno);
			}
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
				PERROR("VK_IOCTL_ACCESS_BAR 0x%x",
					rc);
				exit(rc);
			}

			fprintf(stdout, "0x%x\n", data[0]);
			fflush(stdout);

			fprintf(stdout, "    access bar done\n");
			fflush(stdout);
			continue;
		}

		if (!strcmp(str, "rf")) {
			struct vk_access access;
			char fname[FNAME_LEN];
			FILE *pfile;
			long fsize;
			char *buffer;
			unsigned long int barno;
			unsigned long long int offset;

			i++;
			str = argv[i];
			err = string2ul(str, &barno);
			if (err < 0) {
				PERROR("bad barno %s err=0x%x",
					str, errno);
				exit(errno);
			}
			fprintf(stdout, "barno=%ld\n", barno);

			i++;
			str = argv[i];
			err = string2ul(str, (unsigned long *)&offset);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad offset %s err=0x%x",
					str, errno);
				exit(errno);
			}
			fprintf(stdout, "offset=%llx\n", offset);

			i++;
			str = argv[i];
			err = string2ul(str, (unsigned long *)&fsize);
			if (err < 0) {
				print_usage(argv);
				PERROR("bad file size %s err=0x%x",
					str, errno);
				exit(errno);
			}
			if (fsize <= 0 || fsize > MAX_FILESIZE) {
				print_usage(argv);
				PERROR("bad file size (%ld)", fsize);
				exit(EINVAL);
			}
			fprintf(stdout, "fsize=%lx\n", fsize);

			i++;
			str = argv[i];
			strncpy(fname, str, sizeof(fname));
			fname[FNAME_LEN - 1] = '\0';
			fprintf(stdout, "fname=%s\n", fname);

			pfile = fopen(fname, "w");
			if (pfile == NULL) {
				PERROR("fopen %s err=0x%x", fname, errno);
				exit(errno);
			}

			/* allocation buffer to contain all file */
			buffer = (char *)malloc(fsize);
			if (buffer == NULL) {
				PERROR("bad alloc %ld bytes err=0x%x",
					fsize, errno);
				fclose(pfile);
				exit(errno);
			}

			memset(buffer, 0, fsize);

			access.barno = barno;
			access.type = VK_ACCESS_READ;
			access.len = sizeof(char) * fsize;
			access.offset = offset;
			access.data = (__u32 *)buffer;

			fprintf(stdout, "Read Bar\n");
			fflush(stdout);

			rc = ioctl(fd, VK_IOCTL_ACCESS_BAR, &access);
			if (rc < 0) {
				PERROR("VK_IOCTL_ACCESS_BAR 0x%x",
					rc);
				exit(rc);
			}

			fprintf(stdout, "    access bar done\n");
			fflush(stdout);

			/* copy buffer into file */
			rc = fwrite(buffer, 1, fsize, pfile);
			if (rc != fsize) {
				PERROR("fwrite %ld bytes to %s err=0x%x",
				       fsize, fname, errno);
				exit(rc);
			}

			fclose(pfile);
			free(buffer);

			continue;
		}

		if (!strcmp(str, "reset")) {
			struct vk_reset reset;

			reset.arg1 = 0;
			reset.arg2 = 0;

			fprintf(stdout, "Reset\n");
			fflush(stdout);
			rc = ioctl(fd, VK_IOCTL_RESET, &reset);
			if (rc < 0) {
				PERROR("VK_IOCTL_RESET 0x%x",
					rc);
				exit(rc);
			}
			fprintf(stdout, "    reset done\n");
			fflush(stdout);
			continue;
		}

		print_usage(argv);
		PERROR("Invalid arguments!");
		exit(EINVAL);
	}

	fprintf(stdout, "Close\n");
	fflush(stdout);
	rc = close(fd);
	if (rc < 0) {
		PERROR("Close failed; err=0x%x",
			errno);
		exit(errno);
	}

	return 0;
}
