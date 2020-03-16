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
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "bcm_vk.h"
#include "pcimem.h"

#define STATUS_OK	0
#define TEST_NODE	0xFFFFFFFF

/* local macros */
#define _STR_BASE(_str)	(_str[1] == 'x' ? 16 : 10)

#define FNAME_LEN	128
#define MAX_FILESIZE	0x4000000	/* 64MB */
#define MAX_ERR_MSG	255

#define DEV_DRV_NAME "/dev/bcm_vk"
#define DEV_LEGACY_DRV_NAME "/dev/bcm-vk"
#define DEV_SYSFS_NAME  "/sys/class/misc/bcm-vk"
#define DEV_SYS_RESOURCE "pci/resource"

#define PERROR(...) do {\
			snprintf(e_msg, \
				 MAX_ERR_MSG, \
				 __VA_ARGS__);\
			fprintf(stderr, \
				" @L:%d %s\n", \
				__LINE__, \
				e_msg);\
			fflush(stderr);\
			} while (0)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define MAX_SCMD_LEN		20
#define MAX_SYS_PATH		200
#define MAX_DID_DIGIT		2
#define MAX_CARDS_PER_HOST	12

/* transaction width - future use - all is 32 bit for now */
enum align {
	ALIGN_8BIT = 1,
	ALIGN_16BIT = 2,
	ALIGN_32BIT = 4,
	ALIGN_64BIT = 8
};

/* fixed position/order of command line arguments */
enum arg_index {
	ARG_SELF,
	ARG_NODE,
	ARG_CMD,
	ARG_PARAM1,
	ARG_PARAM2,
	ARG_PARAM3,
	ARG_PARAM4,
	ARG_PARAM5,
	MAX_SUB_CMDS = (ARG_PARAM5 -
			ARG_PARAM1 + 1)
};

/* supported command IDs */
enum cmd_list {
	CMD_FIRST,		/* 0 - reserved ID */
	CMD_RESET,
	CMD_LOAD_IMAGE,
	CMD_READ_BIN,
	CMD_WRITE_BIN,
	CMD_READ_FILE,
	CMD_WRITE_FILE,
	CMD_LAST
};

/* group commands in classes as they use similar IOCTLs / APIs / handling */
/* use powers of 2 so we can mask */
enum cmd_class {
	IO_AXS_CMDS	= 0x01,
	FIO_AXS_CMDS	= 0x02,
	CTRL_CMDS	= 0x04,
	MAX_CMDS	= (CTRL_CMDS - 1)
};

enum cmd_mode {
	CMD_MODE_VERIFY,
	CMD_MODE_EXEC
};

enum cmd_node {
	ND_FIRST,
	ND_BCM,
	ND_SYS,
	ND_HELP,
	ND_LAST
};

/* command description structure: */
/* list of sub-commands, class it belongs to, min/max param nr. */
struct cmd_attributes {
	char *scmds[MAX_SCMD_LEN];
	enum cmd_class class;
	int min_params;
	int max_params;
};

/* command structure - filled with info for checking command line */
typedef int (*pf_apply)(int, int, int *, int, char *);
struct cmd_unit {
	enum cmd_node cmd_node;
	char *cmd_name;
	pf_apply cmd_apply;
	struct cmd_attributes *cmd_attrib;
};

struct node_unit {
	enum cmd_node node_id;
	char node_norm_path[MAX_SYS_PATH];
	char node_bckup_path[MAX_SYS_PATH];
};

static int cmd_res(int, int, int*, int, char*);
static int cmd_li(int, int, int*, int, char*);
static int cmd_io(int, int, int*, int, char*);

/* node lookup table */
static struct node_unit node_lookup_tbl[] = {
	{ ND_HELP, "", "" },
	{ ND_BCM, DEV_DRV_NAME, DEV_LEGACY_DRV_NAME },
	{ ND_SYS, DEV_SYSFS_NAME, "" }
};

/* command attributes lookup table */
static struct cmd_attributes attr_lookup_tbl[] = {
/*	SUB_CMD(s)                CLASS,    MIN_P, MAX_P */
	{ { "" },                   MAX_CMDS,     0, 0 },
	{ { "" },                   CTRL_CMDS,    0, 0 },
	{ {"boot1", "boot2", "-"},  CTRL_CMDS,    1, 2 },
	{ { "" },                   IO_AXS_CMDS,  2, 2 },
	{ { "" },                   IO_AXS_CMDS,  3, 3 },
	{ { "" },                   FIO_AXS_CMDS, 3, 3 },
	{ { "" },                   FIO_AXS_CMDS, 4, 4 }
};

/* main lookup table */
/* each command has an entry in this table - used for consistency checks */
static struct cmd_unit cmd_lookup_tbl[] = {
	/* iNODE,   CMD,     PF,         ATTRIB */
	{ ND_HELP, "--help", NULL,       &attr_lookup_tbl[CMD_FIRST] },
	{ ND_BCM,   "reset", &cmd_res,    &attr_lookup_tbl[CMD_RESET] },
	{ ND_BCM,   "li",    &cmd_li,     &attr_lookup_tbl[CMD_LOAD_IMAGE] },
	{ ND_SYS,   "rb",    &cmd_io,     &attr_lookup_tbl[CMD_READ_BIN] },
	{ ND_SYS,   "wb",    &cmd_io,     &attr_lookup_tbl[CMD_WRITE_BIN] },
	{ ND_SYS,   "rf",    &cmd_io,     &attr_lookup_tbl[CMD_READ_FILE] },
	{ ND_SYS,   "wf",    &cmd_io,     &attr_lookup_tbl[CMD_WRITE_FILE] }
};

/* variable command line parameter table */
static char cmd_param_tbl[MAX_SUB_CMDS][FNAME_LEN];

/* function prototypes */
static int cmd_get_class(int scmds_cnt);
static int cmd_node_lookup(int node,
			   int resource,
			   enum cmd_mode mode,
			   enum cmd_list cmd_id,
			   char *dev_node,
			   int *f_id);
static int cmd_lookup(enum cmd_class class);

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

	return STATUS_OK;
}

static int cmd_node_lookup(int node,
			   int resource,
			   enum cmd_mode mode,
			   enum cmd_list cmd_id,
			   char *dev_node,
			   int *f_id)
{
	int class;
	char device_node[MAX_SYS_PATH] = "";
	char e_msg[MAX_ERR_MSG] = "";
	char *f_path = NULL;
	int fd = -1;
	enum cmd_node node_idx = 0;

	class = cmd_lookup_tbl[cmd_id].cmd_attrib->class;
	node_idx = cmd_lookup_tbl[cmd_id].cmd_node;
	if (mode != CMD_MODE_VERIFY) {
		char *n_path = node_lookup_tbl[node_idx].node_norm_path;

		if (node_idx == ND_SYS)
			sprintf(device_node,
				"%s.%d/%s%d",
				n_path,
				node,
				DEV_SYS_RESOURCE,
				resource);
		else
			snprintf(device_node,
				 sizeof(device_node),
				 "%s.%d",
				 n_path,
				 node);
		f_path = device_node;
	}
	if (f_id != NULL && class == CTRL_CMDS) {
		f_path = (f_path == NULL) ? dev_node : f_path;
		fd = open(f_path, O_RDWR);
		if (fd < 0 &&
		    strlen(node_lookup_tbl[node_idx].node_bckup_path) > 0) {
			snprintf(device_node,
				 sizeof(device_node),
				 "%s.%d",
				 node_lookup_tbl[node_idx].node_bckup_path,
				 node);
			if (mode != CMD_MODE_VERIFY) {
				fd = open(device_node, O_RDWR);
				if (fd < 0) {
					PERROR("%s open failed; err=0x%x",
					       device_node, errno);
					return errno;
				}
			}
		}
		if (f_id != NULL && class == CTRL_CMDS)
			*f_id = fd;
		fprintf(stdout, "Open %s\n", device_node);
		fflush(stdout);
	}
	if (mode != CMD_MODE_VERIFY)
		strncpy(dev_node, device_node, MAX_SYS_PATH);
	return STATUS_OK;
}

static void print_usage(void)
{
	printf("Usage: vkcli <node_num> <args...>\n");
	printf("node_num: 0..11\n");
	printf("Available arguments:\n");
	printf("\tli: load image\n");
	printf("\t\t[-/boot1/boot2]\n");
	printf("\t\t\t'-' load both stages (both boot1 and boot2)\n");
	printf("\t\t\t'boot1' -- only first stage (boot1)\n");
	printf("\t\t\t'boot2' -- only second stage (boot2)\n");
	printf("\trb: read bar <barno> <offset>\n");
	printf("\trf: read to file <barno> <offset> <len> file\n");
	printf("\twb: write bar <barno> <offset> <value>\n");
	printf("\twf: write from file <barno> <offset> file\n");
	printf("\treset: issue reset command\n");
}

static int is_valid_cmd(int cmd_cnt,
			int scmd_cnt,
			char *argv[],
			enum cmd_node *node_id)
{
	char e_msg[MAX_ERR_MSG] = "";
	char *str = NULL;
	char limits[] = " .";
	int i, ret = -EINVAL;
	int value;

	*node_id = ND_HELP;
	if (cmd_cnt < ARG_NODE)
		return STATUS_OK;
	str = argv[ARG_NODE];
	if (str == NULL)
		return -EINVAL;
	if (strcmp(str, "--help") == 0)
		return STATUS_OK;
	if (scmd_cnt >= MAX_SUB_CMDS) {
		PERROR("%s: Invalid parameter nr: %d\n",
		       argv[ARG_CMD],
		       scmd_cnt);
		print_usage();
		return -EINVAL;
	}
	/* mirror command line in our cmd_param_tbl */
	for (i = 0; i <  cmd_cnt; i++) {
		strncpy(cmd_param_tbl[i],
			argv[i],
			sizeof(cmd_param_tbl[0]));
		cmd_param_tbl[i][sizeof(cmd_param_tbl[0]) - 1] = '\0';
	}
	/* legacy dev node naming */
	if (strlen(str) > MAX_DID_DIGIT) {
		str = strstr(str, "/dev/bcm");
		if (str == NULL)
			return -EINVAL;
		str = strtok(str, limits);
		if (str == NULL)
			return -EINVAL;
		str = strtok(NULL, limits);
		if (str == NULL)
			return -EINVAL;
		ret = STATUS_OK;
	} else {
		for (i = 0; i < strlen(str); i++) {
			ret = -EINVAL;
			if (isdigit(str[i]))
				ret = STATUS_OK;
			else
				break;
		}
	}
	if (ret != STATUS_OK)
		return ret;
	ret = string2ul(str,
			(unsigned long *)&value);
	if (ret == STATUS_OK &&
	    value >= 0 &&
	    value < MAX_CARDS_PER_HOST) {
		*node_id = value;
		return STATUS_OK;
	}
	return -EINVAL;
}

static int cmd_get_class(int scmds_cnt)
{
	char e_msg[MAX_ERR_MSG] = "";
	enum cmd_list i = 0;

	for (i = CMD_FIRST; i < CMD_LAST; i++) {
		if (strcmp(cmd_param_tbl[ARG_CMD],
			   cmd_lookup_tbl[i].cmd_name) == 0) {
			if (scmds_cnt <
			    cmd_lookup_tbl[i].cmd_attrib->min_params ||
			    scmds_cnt >
			    cmd_lookup_tbl[i].cmd_attrib->max_params) {
				PERROR("%s: Invalid parameter nr: %d\n",
				       cmd_param_tbl[ARG_CMD],
				       scmds_cnt);
				return -EINVAL;
			}
			return cmd_lookup_tbl[i].cmd_attrib->class;
		}
	}
	return -EINVAL;
}

static int cmd_lookup(enum cmd_class cclass)
{
	char e_msg[MAX_ERR_MSG] = "";
	enum cmd_list i;
	char *str;

	str = cmd_param_tbl[ARG_CMD];
	for (i = CMD_FIRST + 1; i < CMD_LAST; i++)
		if (cmd_lookup_tbl[i].cmd_attrib->class == cclass &&
		    strcmp(str, cmd_lookup_tbl[i].cmd_name) == 0)
			break;
	if (i == CMD_LAST) {
		PERROR("bad cmd %s for class: %d\n",
		       str,
		       cclass);
		print_usage();
		return -EINVAL;
	}
	return i;
}

static int scmd_get_param(int cmd_cnt,
			  enum cmd_list c_id,
			  int *scmd_idx,
			  int *scmd_cnt)
{
	int cclass;
	int count = 0, i = 0, limit = 0;
	int idx = ARG_PARAM1;
	struct cmd_attributes *ca;
	int ret;
	int scmds;
	int tot_cnt = 0;
	int value = 0;

	*scmd_idx = 0;
	*scmd_cnt = 0;
	ca = cmd_lookup_tbl[c_id].cmd_attrib;
	scmds = cmd_cnt - ARG_PARAM1;
	cclass = ca->class;
	switch (cclass) {
	case CTRL_CMDS:
		/* arguments validation */
		while ((idx < cmd_cnt) && (count < MAX_SUB_CMDS)) {
			for (i = CMD_FIRST; ca->scmds[i] != NULL; i++) {
				if (strcmp(ca->scmds[i],
					   cmd_param_tbl[idx]) == 0) {
					scmd_idx[count] = i;
					count++;
					break;
				} else {
					continue;
				}
			}
			idx++;
		}
		*scmd_cnt = (count > 0) ? (count - 1) : 0;
		break;
	case IO_AXS_CMDS:
		limit = scmds;
		/* fallthrough - no break */
	case FIO_AXS_CMDS:
		/* affect limit so we don't fail converting file name */
		limit = (limit == 0) ? scmds - 1 : limit;

		while (((idx - ARG_PARAM1) < limit) &&
		       (count < MAX_SUB_CMDS)) {
			ret = string2ul(cmd_param_tbl[idx],
					(unsigned long *)&value);
			if (ret == STATUS_OK) {
				scmd_idx[count] = value;
				count++;
			} else {
				continue;
			}
			idx++;
		}
		/* take into account non-numeric param -last one */
		tot_cnt = (scmds - limit) + count;
		*scmd_cnt = tot_cnt;
		break;
	case MAX_CMDS:
	default:
		break;
	};
	return STATUS_OK;
}

static int cmd_li(int fd,
		  int cmd_idx,
		  int *scmd_idx,
		  int cnt,
		  char *path)
{
	int arg_idx, rc;
	char e_msg[MAX_ERR_MSG] = "";
	struct vk_image image[] = {{.filename = "vk-boot1.bin",
				    .type = VK_IMAGE_TYPE_BOOT1},
				   {.filename = "vk-boot2.bin",
				    .type = VK_IMAGE_TYPE_BOOT2}
				  };
	int start_idx = 0, end_idx = 0;

	fprintf(stdout,
		"Issue command %s\n", cmd_lookup_tbl[cmd_idx].cmd_name);
	fflush(stdout);

	/* only support li at this time */
	if (strcmp("li", cmd_lookup_tbl[cmd_idx].cmd_name) != 0) {
		PERROR("Unsupported load command %s\n",
		       cmd_lookup_tbl[cmd_idx].cmd_name);
		return -EINVAL;
	}
	switch (scmd_idx[0]) {
	case 0:
		start_idx = 0;
		end_idx = 0;
		break;
	case 1:
		start_idx = 1;
		end_idx = 1;
		break;
	case 2:
		start_idx = 0;
		end_idx = 1;
		break;
	default:
		return -EINVAL;
	};
	for (arg_idx = start_idx; arg_idx < end_idx + 1; arg_idx++) {
		int nr_elem = ARRAY_SIZE(image);

		if (arg_idx >= nr_elem) {
			PERROR("VK_IO fail index out of bounds\n");
			return -EINVAL;
		}
		rc = ioctl(fd,
			   VK_IOCTL_LOAD_IMAGE,
			   &image[arg_idx]);
		if (rc < 0) {
			PERROR("VK_IO fail 0x%x",
			       rc);
			return rc;
		}
	}
	return STATUS_OK;
}

static int cmd_res(int fd,
		   int cmd_idx,
		   int *scmd_idx,
		   int cnt,
		   char *path)
{
	char e_msg[MAX_ERR_MSG] = "";
	int rc;
	struct vk_reset reset;

	reset.arg1 = 0;
	reset.arg2 = 0;
	fprintf(stdout,
		"Issue command %s\n",
		cmd_lookup_tbl[cmd_idx].cmd_name);
	fflush(stdout);

	/* only sypport reset at this time */
	if (strcmp("reset", cmd_lookup_tbl[cmd_idx].cmd_name) != 0 ||
	    cnt > 0) {
		PERROR("Unsupported control command %s\n",
		       cmd_lookup_tbl[cmd_idx].cmd_name);
		return -EINVAL;
	}
	/* we could use a generic IOCTL instead */
	rc = ioctl(fd, VK_IOCTL_RESET, &reset);
	if (rc < 0) {
		PERROR("VK_IOCTL_RESET failed 0x%x fd: %x\n", rc, fd);
		return rc;
	}
	return STATUS_OK;
}

static int cmd_io(int fd,
		  int cmd_idx,
		  int *scmd_idx,
		  int scmd_cnt,
		  char *path)
{
	int data[MAX_SUB_CMDS];
	char e_msg[MAX_ERR_MSG] = "";
	int fnode = 0;
	map_info lmap_info = { NULL, 4096 };
	unsigned long long offset;
	int rc, ret = -EINVAL;
	int value;

	if (scmd_cnt < 2) {
		PERROR("%s: invalid io read command; cnt=%d\n",
		       cmd_lookup_tbl[cmd_idx].cmd_name,
		       scmd_cnt);
		ret = -EINVAL;
	} else {
		/* by convention.. */
		offset = scmd_idx[1];
		pcimem_init(path,
			    &lmap_info,
			    &fnode);
		pcimem_map_base(&lmap_info,
				fnode,
				offset,
				ALIGN_32BIT);
		if (cmd_idx == CMD_READ_BIN) {
			data[0] = (int)pcimem_read(&lmap_info,
						   offset,
						   ALIGN_32BIT);
			if (data[0] >= 0) {
				ret = STATUS_OK;
				fprintf(stdout,
					"0x%04llX: 0x%0*X\n",
					offset,
					2 * ALIGN_32BIT,
					data[0]);
			}
		} else if (cmd_idx == CMD_WRITE_BIN) {
			/* by convention.. */
			value = scmd_idx[2];
			rc = (int)pcimem_write(&lmap_info,
						offset,
						value,
						ALIGN_32BIT);
			if (rc < 0) {
				PERROR("%s: bad io write; err=0x%x\n",
				       cmd_lookup_tbl[cmd_idx].cmd_name,
				       rc);
				ret = rc;
			} else {
				ret = STATUS_OK;
			}
		}
		fprintf(stdout, "\taccess bar done\n");
		fflush(stdout);
		pcimem_deinit(&lmap_info,
			      &fnode);
	}
	return ret;
}


static int handle_cmd_apply(enum cmd_node node,
			    int resource,
			    enum cmd_list c_id,
			    int *scmd_idx,
			    int scmd_cnt,
			    char *path)
{
	char e_msg[MAX_ERR_MSG] = "";
	int fd = 0;
	int rc;

	rc = cmd_node_lookup(node,
			     resource,
			     CMD_MODE_EXEC,
			     c_id,
			     path, &fd);
	if (rc < 0) {
		PERROR("error in node access %s; err=0x%x",
		       path, rc);
		return rc;
	}
	rc = cmd_lookup_tbl[c_id].cmd_apply(fd,
					    c_id,
					    scmd_idx,
					    scmd_cnt,
					    path);
	if (rc < 0) {
		PERROR("error in apply cmd %d; err=0x%x",
		       c_id, rc);
		return rc;
	}

	close(fd);
	return STATUS_OK;
}

static int  cmd_handler(int cmd_cnt,
			int scmd_cnt,
			int fnode)
{
	enum cmd_class cmdc;
	int data[MAX_SUB_CMDS];
	char device_path[MAX_SYS_PATH] = "";
	char e_msg[MAX_ERR_MSG] = "";
	int rc = -1;
	int ret = -EINVAL;
	int scmds_found = 0;

	if (fnode == ND_HELP) {
		print_usage();
		return STATUS_OK;
	}
	cmdc = cmd_get_class(scmd_cnt);
	rc = cmd_lookup(cmdc);
	if (rc < 0) {
		PERROR("%s: bad command; err=0x%x",
		       cmd_param_tbl[ARG_CMD],
		       rc);
		ret = rc;
	} else {
		enum cmd_list c_id = rc;

		rc = scmd_get_param(cmd_cnt,
				    c_id,
				    data,
				    &scmds_found);
		if (rc == STATUS_OK) {
			int res = 0;

			res = (cmdc & (IO_AXS_CMDS | FIO_AXS_CMDS)) ?
					data[0] : 0;
			rc = cmd_node_lookup(fnode,
					     res,
					     CMD_MODE_VERIFY,
					     c_id,
					     device_path,
					     NULL);
			if (rc < 0) {
				PERROR("bad node; %d %s err=0x%x",
				       fnode,
				       device_path,
				       errno);
				ret = rc;
			} else {
				ret =  handle_cmd_apply(fnode,
							res,
							c_id,
							data,
							scmds_found,
							device_path);
			}
		} else {
			PERROR("bad command; %d %s err=0x%x",
			       c_id,
			       device_path,
			       errno);
			ret = errno;
		}
		fprintf(stdout, "\tcommand done\n");
		fflush(stdout);
	}
	return ret;
}

int main(int argc,
	 char *argv[])
{
	char e_msg[MAX_ERR_MSG] = "";
	enum cmd_node node;
	int rc = -1;
	int cmd_cnt, scmd_cnt;

	cmd_cnt = argc;
	scmd_cnt = cmd_cnt - ARG_PARAM1;
	rc = is_valid_cmd(cmd_cnt, scmd_cnt, argv, &node);
	if (rc != STATUS_OK) {
		PERROR("%s / %s: invalid command / node; err=0x%x",
		       argv[ARG_CMD],
		       argv[ARG_NODE],
		       rc);
		print_usage();
		return rc;
	}
	return cmd_handler(cmd_cnt, scmd_cnt, node);
}
