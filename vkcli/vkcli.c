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

enum scmd_index {
	SC_BAR = ARG_PARAM1 - ARG_CMD,
	SC_OFFSET
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
	ND_LAST,
	ND_INVALID = 0xFFFFFFFF
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
	{ { "" },                   FIO_AXS_CMDS, 4, 4 },
	{ { "" },                   FIO_AXS_CMDS, 5, 5 }
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
static int cmd_get_class(int scmds_cnt, enum cmd_class *cclass);
static int cmd_node_lookup(int node,
			   int resource,
			   enum cmd_mode mode,
			   enum cmd_list cmd_id,
			   char *dev_node,
			   int *f_id);
static int cmd_lookup(enum cmd_class cclass, enum cmd_list *ci);
static int find_size(char *f_name, unsigned int *size);

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

/**
 * @brief string2ul strtoul wrapper function
 *
 * @param[in] str string to convert
 * @param[out] return_value value
 *
 * @return STATUS_OK on success, error code otherwise
 */
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

/**
 * @brief find_size determines file size
 *
 * @param[in] f_name path/file name
 * @param[out] size file size
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int find_size(char *f_name, unsigned int *size)
{
	char e_msg[MAX_ERR_MSG] = "";
	FILE *fp = NULL;
	int ret = -EINVAL;

	if (size != NULL)
		*size = 0;
	else
		return -EINVAL;
	fp = fopen(f_name, "r");
	if (fp == NULL) {
		PERROR("File: %s Not Found!\n", f_name);
		return -EINVAL;
	}
	ret = fseek(fp, 0L, SEEK_END);
	if (ret < 0) {
		PERROR("I/O error on file: %s, err %x\n",
		       f_name,
		       errno);
	} else {
		ret = ftell(fp);
		if (ret > 0)
			*size = ret;
		else
			PERROR("I/O error on file: %s, err %x\n",
			       f_name,
			       errno);
	}
	ret = (ret > 0) ? STATUS_OK : ret;
	fclose(fp);
	return ret;
}

/**
 * @brief cmd_node_lookup construct sys path and use it according to mode
 *
 * @param[in] node device instance
 * @param[in] resource endpoint for enumerating devices i.e bar for pcie
 * @param[in] mode - perform open action or just verify it exists
 * @param[in] cmd_id - command to apply on device / endpoint
 * @param[in/out] dev_node - user requested sys path / open real path
 * @param[out] f_id - file descriptor, when open is performed
 *
 * @return STATUS_OK on success, file errno code otherwise
 */
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

/**
 * @brief is_valid_cmd analyze / validate command line
 *
 * @param[in] cmd_cnt alias for argc
 * @param[in] scmd_cnt sub-command number of arguments
 * @param[in] cmd_line alias for argv
 * @param[out] node device instance
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int is_valid_cmd(int cmd_cnt,
			int scmd_cnt,
			char *cmd_line[],
			enum cmd_node *node)
{
	char e_msg[MAX_ERR_MSG] = "";
	char *str = NULL;
	char limits[] = " .";
	int i, ret = -EINVAL;
	int value;

	*node = ND_INVALID;
	if (cmd_cnt <= ARG_NODE)
		return STATUS_OK;
	str = cmd_line[ARG_NODE];
	if (str == NULL)
		return -EINVAL;
	if (strcmp(str, "--help") == 0)
		return STATUS_OK;
	if (scmd_cnt >= MAX_SUB_CMDS) {
		PERROR("%s: Invalid parameter nr: %d\n",
		       cmd_line[ARG_CMD],
		       scmd_cnt);
		print_usage();
		return -EINVAL;
	}
	/* mirror command line in our cmd_param_tbl */
	for (i = 0; i <  cmd_cnt; i++) {
		strncpy(cmd_param_tbl[i],
			cmd_line[i],
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
		*node = value;
		return STATUS_OK;
	}
	return -EINVAL;
}

/**
 * @brief cmd_get_class returns the command class when valid
 * for processing convenince commands are grouped in classes
 *
 * @param[in] scmd_cnt sub-command number of arguments
 * @param[out] cclass command class
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_get_class(int scmds_cnt, enum cmd_class *cclass)
{
	char e_msg[MAX_ERR_MSG] = "";
	enum cmd_list i = 0;
	int ret = -EINVAL;
	struct cmd_unit *ucmd;

	if (cclass != NULL)
		for (i = CMD_FIRST; i < CMD_LAST; i++) {
			ucmd = &cmd_lookup_tbl[i];
			if (strcmp(cmd_param_tbl[ARG_CMD],
				   ucmd->cmd_name) == 0) {
				*cclass = ucmd->cmd_attrib->class;
				if (scmds_cnt < ucmd->cmd_attrib->min_params ||
				    scmds_cnt > ucmd->cmd_attrib->max_params)
					PERROR("%s: Bad parameter nr: %d\n",
					       cmd_param_tbl[ARG_CMD],
					       scmds_cnt);
				else
					ret = STATUS_OK;
			}
		}
	return ret;
}

/**
 * @brief cmd_lookup finds lookup index for current command
 *
 * @param[in] cclass command class
 * @param[out] ci command lookup index
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_lookup(enum cmd_class cclass, enum cmd_list *ci)
{
	char e_msg[MAX_ERR_MSG] = "";
	enum cmd_list i;
	int ret = -EINVAL;
	char *str;
	struct cmd_unit *ucmd;

	if (ci != NULL) {
		str = cmd_param_tbl[ARG_CMD];
		for (i = CMD_FIRST + 1; i < CMD_LAST; i++) {
			ucmd = &cmd_lookup_tbl[i];
			if (ucmd->cmd_attrib->class == cclass &&
			    strcmp(str, ucmd->cmd_name) == 0)
				break;
		}
		if (i == CMD_LAST) {
			PERROR("bad cmd %s for class: %d\n",
			       str,
			       cclass);
			print_usage();
		} else {
			*ci = i;
			ret = STATUS_OK;
		}
	}
	return ret;
}

/**
 * @brief scmd_get_param retrieves / converts sub-command parameters
 *
 * @param[in] cmd_cnt alias for argc
 * @param[in] cmd_idx command index
 * @param[out] scmd_idx pointer to array of sub_command values
 * @param[out] scmd_cnt sub-command number of identified parameters
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int scmd_get_param(int cmd_cnt,
			  enum cmd_list cmd_idx,
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
	ca = cmd_lookup_tbl[cmd_idx].cmd_attrib;
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
				scmd_idx[count + SC_BAR] = value;
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
	}
	return STATUS_OK;
}

/**
 * @brief cmd_li command handler for load image sub-command
 *
 * @param[in] fd - device file descriptor
 * @param[in] cmd_idx command index
 * @param[in] scmd_idx pointer to array of sub_command values
 * @param[in] scmd_cnt number of values in the sub-command array
 * @param[in] path dev_node remapped path (may differ from user path)
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_li(int fd,
		  int cmd_idx,
		  int *scmd_idx,
		  int scmd_cnt,
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
	}
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

/**
 * @brief cmd_res command handler for reset sub-command
 *
 * @param[in] fd - device file descriptor
 * @param[in] cmd_idx command index
 * @param[in] scmd_idx pointer to array of sub_command values
 * @param[in] scmd_cnt number of values in the sub-command array
 * @param[in] path dev_node remapped path (may differ from user path)
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_res(int fd,
		   int cmd_idx,
		   int *scmd_idx,
		   int scmd_cnt,
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
	    scmd_cnt > 0) {
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

/**
 * @brief cmd_io command handler for io access sub-commands: rb, wb, rf, wf
 *
 * @param[in] fd - device file descriptor
 * @param[in] cmd_idx command index
 * @param[in] scmd_idx pointer to array of sub_command values
 * @param[in] scmd_cnt number of values in the sub-command array
 * @param[in] path dev_node remapped path (may differ from user path)
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_io(int fd,
		  int cmd_idx,
		  int *scmd_idx,
		  int scmd_cnt,
		  char *path)
{
	/* default: 32 bit align */
	int align = ALIGN_32_BIT;
	char e_msg[MAX_ERR_MSG] = "";
	int fnode = 0;
	int *io_data = NULL;
	int io_file = 0;
	unsigned int len = 0;
	int li = 0;
	struct map_info lmap_info = { NULL, 4096 };
	unsigned long long offset;
	int rc, ret = -EINVAL, sys_ps = 0;
	struct cmd_unit *ucmd;

	ucmd = &cmd_lookup_tbl[cmd_idx];
	if (scmd_cnt < 2) {
		PERROR("%s: invalid io read command; cnt=%d\n",
		       ucmd->cmd_name,
		       scmd_cnt);
		ret = -EINVAL;
	} else {
		offset = scmd_idx[SC_OFFSET];
		pcimem_init(path,
			    &lmap_info,
			    &fnode);
		sys_ps = lmap_info.map_size;
		/* default: word access - same as align */
		len = align;
		if (ucmd->cmd_attrib->class == IO_AXS_CMDS)
			pcimem_map_base(&lmap_info,
					fnode,
					offset,
					align);
		switch (cmd_idx) {
		case CMD_READ_BIN:
			/* pcimem api allows accessing multiple locations */
			/* vkcli supports one location only for now */
			io_data = malloc(len);
			if (io_data == NULL)
				return -EINVAL;
			rc = pcimem_read(&lmap_info,
					 offset,
					 len,
					 io_data,
					 align);
			if (rc >= 0) {
				ret = STATUS_OK;
				fprintf(stdout,
					"0x%04llX: 0x%0*X\n",
					offset,
					2 * align,
					*io_data);
			}
			break;
		case CMD_READ_FILE:
			/* by convention.. second to last is length */
			len = scmd_idx[scmd_cnt - 1];
			io_data = malloc(len);
			if (io_data == NULL)
				return -EINVAL;
			lmap_info.map_size = PAGE_RNDUP(len, sys_ps);
			pcimem_map_base(&lmap_info,
					fnode,
					offset,
					align);
			rc = pcimem_blk_read(&lmap_info,
					     offset,
					     len,
					     io_data,
					     align);
			if (rc >= 0) {
				li = ARG_CMD + scmd_cnt;
				rc = open(cmd_param_tbl[li],
					  O_CREAT |
					  O_SYNC  |
					  O_TRUNC |
					  O_WRONLY,
					  S_IRUSR |
					  S_IWUSR |
					  S_IRGRP |
					  S_IROTH);
				io_file = rc;
				if (rc <= 0) {
					PERROR("Fail to open %s\n",
					       cmd_param_tbl[li]);
					ret = errno;
				} else {
					rc = write(io_file,
						   io_data,
						   len);
				}
				if (rc < 0) {
					PERROR("IO file %s err: %d\n",
					       cmd_param_tbl[li],
					       errno);
					ret = errno;
				} else {
					ret = STATUS_OK;
				}
				if (io_file >= 0)
					close(io_file);
			} else {
				PERROR("Read file %s err: %d\n",
				       cmd_param_tbl[li], rc);
			}
			break;
		case CMD_WRITE_BIN:
			/* pcimem api allows accessing multiple locations */
			/* vkcli supports one location only for now */
			io_data = malloc(len);
			if (io_data == NULL)
				return -EINVAL;
			*io_data = scmd_idx[SC_OFFSET + 1];
			rc = pcimem_write(&lmap_info,
					  offset,
					  len,
					  io_data,
					  align);
			if (rc < 0) {
				PERROR("%s: bad io write; err=0x%x\n",
				       ucmd->cmd_name,
				       rc);
				ret = rc;
			} else {
				ret = STATUS_OK;
			}
			break;
		case CMD_WRITE_FILE:
			li = ARG_CMD + scmd_cnt;
			ret = find_size(cmd_param_tbl[li], &len);
			if (ret != STATUS_OK)
				return -EINVAL;
			io_data = malloc(len);
			if (io_data == NULL)
				return -EINVAL;
			rc = open(cmd_param_tbl[li],
				  O_RDONLY);
			io_file = rc;
			if (rc <= 0) {
				PERROR("Fail to open %s\n",
				       cmd_param_tbl[li]);
				ret = errno;
			} else {
				rc = read(io_file, io_data, len);
			}
			if (rc < 0) {
				PERROR("IO file %s err: %d\n",
				       cmd_param_tbl[li],
				       errno);
				ret = errno;
			} else {
				len = rc;
				lmap_info.map_size = PAGE_RNDUP(len, sys_ps);
				pcimem_map_base(&lmap_info,
						fnode,
						offset,
						align);
				rc = pcimem_blk_write(&lmap_info,
						      offset,
						      len,
						      io_data,
						      align);
				if (rc < 0) {
					PERROR("%s: bad io write; err=0x%x\n",
					       ucmd->cmd_name,
					       rc);
					ret = rc;
				} else {
					ret = STATUS_OK;
				}
			}
			if (io_file >= 0)
				close(io_file);
			break;
		}
		if (io_data != NULL)
			free(io_data);
		fprintf(stdout, "\taccess bar done\n");
		fflush(stdout);
		pcimem_deinit(&lmap_info,
			      &fnode);
	}
	return ret;
}

/**
 * @brief handle_cmd_apply opens the device and calls sub-command handler
 *
 * @param[in] node device instance
 * @param[in] resource endpoint for enumerating devices i.e bar for pcie
 * @param[in] cmd_idx command index
 * @param[out] scmd_idx pointer to array of sub_command values
 * @param[in] scmd_cnt sub-command number of arguments
 * @param[in] path dev_node remapped path (may differ from user path)
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int handle_cmd_apply(enum cmd_node node,
			    int resource,
			    enum cmd_list cmd_idx,
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
			     cmd_idx,
			     path, &fd);
	if (rc < 0) {
		PERROR("error in node access %s; err=0x%x",
		       path, rc);
		return rc;
	}
	rc = cmd_lookup_tbl[cmd_idx].cmd_apply(fd,
					       cmd_idx,
					       scmd_idx,
					       scmd_cnt,
					       path);
	if (rc < 0) {
		PERROR("error in apply cmd %d; err=0x%x",
		       cmd_idx, rc);
		return rc;
	}

	close(fd);
	return STATUS_OK;
}

/**
 * @brief cmd_handler main processing function
 *
 * @param[in] cmd_cnt alias for argc
 * @param[in] scmd_cnt sub-command number of arguments
 * @param[in] fnode identified in the input command line
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int  cmd_handler(int cmd_cnt,
			int scmd_cnt,
			int fnode)
{
	enum cmd_class cmdc;
	enum cmd_list cmd_idx = 0;
	int data[MAX_SUB_CMDS];
	char device_path[MAX_SYS_PATH] = "";
	char e_msg[MAX_ERR_MSG] = "";
	int rc = -1;
	int ret = -EINVAL;
	int scmds_found = 0;

	if (fnode == ND_INVALID) {
		print_usage();
		return STATUS_OK;
	}
	memset(data, 0, sizeof(data));
	rc = cmd_get_class(scmd_cnt, &cmdc);
	if (rc < 0) {
		PERROR("%s: bad command for class %d; err=0x%x",
		       cmd_param_tbl[ARG_CMD],
		       cmdc,
		       rc);
		return ret;
	}
	rc = cmd_lookup(cmdc, &cmd_idx);
	if (rc < 0) {
		PERROR("%s: bad command; err=0x%x",
		       cmd_param_tbl[ARG_CMD],
		       rc);
		ret = rc;
	} else {
		enum cmd_node n_id = rc;

		n_id = cmd_lookup_tbl[cmd_idx].cmd_node;
		if (n_id == ND_HELP) {
			print_usage();
			return STATUS_OK;
		}
		rc = scmd_get_param(cmd_cnt,
				    cmd_idx,
				    data,
				    &scmds_found);
		if (rc == STATUS_OK) {
			int bar = 0;

			bar = (cmdc & (IO_AXS_CMDS | FIO_AXS_CMDS)) ?
					data[SC_BAR] * 2 : 0;
			rc = cmd_node_lookup(fnode,
					     bar,
					     CMD_MODE_VERIFY,
					     cmd_idx,
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
							bar,
							cmd_idx,
							data,
							scmds_found,
							device_path);
			}
		} else {
			PERROR("bad command; %d %s err=0x%x",
			       cmd_idx,
			       device_path,
			       errno);
			ret = errno;
		}
		fprintf(stdout, "\tcommand done");
		fflush(stdout);
	}
	return ret;
}

/**
 * @brief main standard entry point
 */
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
	rc = cmd_handler(cmd_cnt, scmd_cnt, node);
	/* DO NOT REMOVE */
	/* Following line used as END marker by calling scripts */
	printf("\nClose\n");
	return rc;
}
