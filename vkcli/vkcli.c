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
#include "version.h"
#include "vkutil_msg.h"

#define DEV_SYSFS_NAME		"/sys/class/misc/bcm-vk"
#define DEV_DRV_NAME		"/dev/bcm_vk"
#define DEV_LEGACY_DRV_NAME	"/dev/bcm-vk"
#define DEV_SYS_RESOURCE	"pci/resource"

#define MAX_CARDS_PER_HOST	12
#define MAX_DID_DIGIT		2
#define MAX_FILESIZE		0x4000000	/* 64MB */
#define MAX_SCMD_LEN		20
#define MAX_SYS_PATH		200
#define MAX_DEV_NODE_LEN	MAX_SYS_PATH
#define TEST_NODE		0xFFFFFFFF

/* defines for register representing card */
#define BOOT_STATUS_REG 0x404
#define BOOT_STATUS_UCODE_NOT_RUN 0x10002
#define BOOT_STATUS_BAR_NUM 0

/* local macros */

#define S_LERR(a, b) do { \
			if ((a) >= 0) \
				(a) = (b); \
			} while (0)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

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
	ARG_LAST,
	MAX_SUB_CMDS = (ARG_PARAM5 -
			ARG_PARAM1 + 1)
};

enum scmd_index {
	SC_BAR = ARG_PARAM1 - ARG_CMD,
	SC_OFFSET,
	SC_VAL,
	SC_MAX
};

/* supported command IDs */
enum cmd_list {
	CMD_FIRST,		/* 0 - reserved ID */
	CMD_INFO,
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
	INFO_CMDS	= 0x08,
	MAX_CMDS	= (INFO_CMDS << 1)
};

enum cmd_mode {
	CMD_MODE_VERIFY,
	CMD_MODE_EXEC
};

enum li_method {
	LI_BOOT1 = 0,
	LI_BOOT2,
	LI_BOOT1_BOOT2
};

enum cmd_node {
	ND_FIRST,
	ND_VER,
	ND_BCM,
	ND_SYS,
	ND_HELP,
	ND_LAST,
	ND_INVALID = 0xFFFFFFFF
};

enum node_path {
	ND_NORMAL_PATH,
	ND_BACKUP_PATH
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
	char node_backup_path[MAX_SYS_PATH];
};

static int cmd_io(int, int, int*, int, char*);
static int cmd_li(int, int, int*, int, char*);
static int cmd_fio(int, int, int*, int, char*);
static int cmd_res(int, int, int*, int, char*);
static int cmd_ver(int, int, int*, int, char*);

/* node lookup table */
static struct node_unit node_lookup_tbl[] = {
	{ ND_HELP, "", "" },
	{ ND_VER, "", "" },
	{ ND_BCM, DEV_DRV_NAME, DEV_LEGACY_DRV_NAME },
	{ ND_SYS, DEV_SYSFS_NAME, "" },
};

/* command attributes lookup table */
static struct cmd_attributes attr_lookup_tbl[] = {
/*	SUB_CMD(s)                CLASS,    MIN_P, MAX_P */
	{ { "" },                   MAX_CMDS,     0, 0 },
	{ { "" },                   INFO_CMDS,    0, 0 },
	{ { "force" },              CTRL_CMDS,    0, 1 },
	{ { "boot1", "boot2", "-"}, CTRL_CMDS,    1, 3 },
	{ { "" },                   IO_AXS_CMDS,  2, 2 },
	{ { "" },                   IO_AXS_CMDS,  3, 3 },
	{ { "" },                   FIO_AXS_CMDS, 4, 4 },
	{ { "" },                   FIO_AXS_CMDS, 3, 3 }
};

/* main lookup table */
/* each command has an entry in this table - used for consistency checks */
static struct cmd_unit cmd_lookup_tbl[] = {
	/* NODE,   CMD,     PF,         ATTRIB */
	{ ND_HELP, "--help", NULL,        &attr_lookup_tbl[CMD_FIRST] },
	{ ND_VER,  "--version", &cmd_ver, &attr_lookup_tbl[CMD_INFO] },
	{ ND_BCM,   "reset", &cmd_res,    &attr_lookup_tbl[CMD_RESET] },
	{ ND_BCM,   "li",    &cmd_li,     &attr_lookup_tbl[CMD_LOAD_IMAGE] },
	{ ND_SYS,   "rb",    &cmd_io,     &attr_lookup_tbl[CMD_READ_BIN] },
	{ ND_SYS,   "wb",    &cmd_io,     &attr_lookup_tbl[CMD_WRITE_BIN] },
	{ ND_SYS,   "rf",    &cmd_fio,    &attr_lookup_tbl[CMD_READ_FILE] },
	{ ND_SYS,   "wf",    &cmd_fio,    &attr_lookup_tbl[CMD_WRITE_FILE] },
};

/* variable command line parameter table */
static char cmd_param_tbl[ARG_LAST][FNAME_LEN];

/* function prototypes */
static int cmd_lookup(enum cmd_class cclass, enum cmd_list *ci);

static void print_usage(void)
{
	FPR_FN("Usage: vkcli <node_num> <args...>\n");
	FPR_FN("node_num: 0..11\n");
	FPR_FN("Available arguments:\n");
	FPR_FN("\tli: load image\n");
	FPR_FN("\t\t<-/boot1/boot2> [fname1] [fname2]\n");
	FPR_FN("\t\t\t'-' load both stages (both boot1 and boot2)\n");
	FPR_FN("\t\t\t'boot1' -- only first stage (boot1)\n");
	FPR_FN("\t\t\t'boot2' -- only second stage (boot2)\n");
	FPR_FN("\trb: read bar <barno> <offset>\n");
	FPR_FN("\trf: read to file <barno> <offset> <len> <fname>\n");
	FPR_FN("\twb: write bar <barno> <offset> <value>\n");
	FPR_FN("\twf: write from file <barno> <offset> <fname>\n");
	FPR_FN("\treset [force]: issue reset command / unconditional\n");
	FPR_FN("\t--version query version information\n");
	FPR_FN("\t--help prints this help\n");
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
	*return_value = strtoul(str, &endptr, 0);
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
	int lret;
	int ret = STATUS_OK;

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
		lret = ftell(fp);
		if (lret < 0)
			PERROR("I/O error on file: %s, err %x\n",
			       f_name,
			       errno);
		else
			*size = lret;
		S_LERR(ret, lret);
	}
	lret = fclose(fp);
	lret = (lret == EOF) ? -errno : 0;
	S_LERR(ret, lret);
	return ret;
}

/**
 * @brief sys_path_translate construct sys path from node
 *
 * @param[in] node device instance
 * @param[in] node_path - allowed values: ND_NORMAL_PATH / ND_BACKUP_PATH
 *                        select node_norm_path / node_backup_path
 * @param[in] resource endpoint for enumerating devices i.e bar for pcie
 * @param[in] cmd_id - command to apply on device / endpoint
 * @param[in/out] dev_node - user requested sys path / open real path
 * @param[in/out] dev_node_size - size of input path / output real path
 *
 * @return STATUS_OK on success, file errno code otherwise
 */
static int sys_path_translate(const int node,
			      const enum node_path node_path,
			      const int resource,
			      const enum cmd_list cmd_id,
			      char *dev_node,
			      int *dev_node_size)
{
	enum cmd_node node_idx = 0;
	char *n_path;
	int ret = STATUS_OK;

	if ((!dev_node) || (!dev_node_size))
		return -EINVAL;

	if (cmd_id == ND_VER)
		return ret;

	node_idx = cmd_lookup_tbl[cmd_id].cmd_node;
	if (node_path == ND_NORMAL_PATH)
		n_path = node_lookup_tbl[node_idx].node_norm_path;
	else
		n_path = node_lookup_tbl[node_idx].node_backup_path;
	if (node_idx == ND_SYS) {
		ret = snprintf(dev_node,
			       *dev_node_size,
			       "%s.%d/%s%d",
			       n_path,
			       node,
			       DEV_SYS_RESOURCE,
			       resource);
	} else {
		if (strlen(n_path) <= 0)
			return -EINVAL;

		ret = snprintf(dev_node,
			       *dev_node_size,
			       "%s.%d",
			       n_path,
			       node);
	}
	if (ret < 0)
		return ret;

	if (ret <= *dev_node_size)
		*dev_node_size = ret + 1;
	return STATUS_OK;
}

/**
 * @brief cmd_node_lookup construct sys path and use it according to mode
 *
 * @param[in] node device instance
 * @param[in] resource endpoint for enumerating devices i.e bar for pcie
 * @param[in] mode - perform open action or just verify it exists
 * @param[in] cmd_id - command to apply on device / endpoint
 * @param[in/out] dev_node - user requested sys path / open real path
 * @param[in/out] dev_node_size - sys path size / open real path size
 * @param[out] f_id - file descriptor, when open is performed
 *
 * @return STATUS_OK on success, file errno code otherwise
 */
static int cmd_node_lookup(int node,
			   int resource,
			   enum cmd_mode mode,
			   enum cmd_list cmd_id,
			   char *dev_node,
			   int *dev_node_size,
			   int *f_id)
{
	int class;
	char device_node[MAX_SYS_PATH] = "";
	int device_node_len;
	char e_msg[MAX_ERR_MSG] = "";
	char *f_path = NULL;
	int fd = -1;
	int ret = STATUS_OK;
	enum cmd_node node_idx = 0;

	class = cmd_lookup_tbl[cmd_id].cmd_attrib->class;
	node_idx = cmd_lookup_tbl[cmd_id].cmd_node;
	device_node_len = sizeof(device_node);
	if (mode != CMD_MODE_VERIFY) {
		ret = sys_path_translate(node,
					 ND_NORMAL_PATH,
					 resource,
					 cmd_id,
					 device_node,
					 &device_node_len);
		if (ret <  0)
			return ret;

		f_path = device_node;
	}
	if (f_id != NULL && class == CTRL_CMDS) {
		f_path = (f_path == NULL) ? dev_node : f_path;
		fd = open(f_path, O_RDWR);
		if (fd < 0) {
			ret = sys_path_translate(node,
						 ND_BACKUP_PATH,
						 resource,
						 cmd_id,
						 device_node,
						 &device_node_len);
			if (ret <  0)
				return ret;

			if (mode != CMD_MODE_VERIFY) {
				fd = open(device_node, O_RDWR);
				if (fd < 0) {
					PERROR("%s open failed; err=0x%x",
					       device_node, errno);
					return -errno;
				}
			}
		}
		if (f_id != NULL && class == CTRL_CMDS)
			*f_id = fd;
		FPR_FN("Open %s\n", device_node);
	}
	if (mode != CMD_MODE_VERIFY) {
		strncpy(dev_node, device_node, device_node_len);
		dev_node[device_node_len - 1] = '\0';
		*dev_node_size = device_node_len;
	}
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
	int i;
	char limits[] = " .";
	int lret;
	int ret = STATUS_OK;
	char *str = NULL;
	int value;

	*node = ND_INVALID;
	if (cmd_cnt <= ARG_NODE)
		return STATUS_OK;
	str = cmd_line[ARG_NODE];
	if (str == NULL)
		return -EINVAL;
	/*
	 * mirror command line in our cmd_param_tbl
	 */
	for (i = 0; i <  cmd_cnt; i++) {
		strncpy(cmd_param_tbl[i],
			cmd_line[i],
			sizeof(cmd_param_tbl[0]));
		cmd_param_tbl[i][sizeof(cmd_param_tbl[0]) - 1] = '\0';
	}
	if (strcmp(str, "--help") == 0) {
		*node = ND_INVALID;
		return STATUS_OK;
	}
	if (strcmp(str, "--version") == 0) {
		*node = ND_VER;
		return STATUS_OK;
	}
	if (scmd_cnt > MAX_SUB_CMDS) {
		PERROR("%s: Invalid parameter nr: %d\n",
		       cmd_line[ARG_CMD],
		       scmd_cnt);
		print_usage();
		return -EINVAL;
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
		ret = STATUS_OK;
		for (i = 0; i < strlen(str); i++) {
			if (isdigit(str[i]) == 0) {
				ret = -EINVAL;
				break;
			}
		}
	}
	if (ret != STATUS_OK)
		return ret;
	lret = string2ul(str,
			 (unsigned long *)&value);
	S_LERR(ret, lret);
	if (ret == STATUS_OK &&
	    value >= 0 &&
	    value < MAX_CARDS_PER_HOST)
		*node = value;
	return ret;
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
	int cmd_idx = 0;
	int lret;
	int ret = STATUS_OK;
	struct cmd_unit *ucmd;

	if (cclass == NULL) {
		PERROR("NULL param\n");
		ret = -EINVAL;
	} else {
		cmd_idx = ARG_CMD;
		if (scmds_cnt == 0)
			cmd_idx = ARG_NODE;
		for (i = CMD_FIRST; i < CMD_LAST; i++) {
			ucmd = &cmd_lookup_tbl[i];
			ret = strcmp(cmd_param_tbl[cmd_idx],
				     ucmd->cmd_name);
			if (ret == 0) {
				*cclass = ucmd->cmd_attrib->class;
				if (*cclass != INFO_CMDS)
					scmds_cnt--;
				if (scmds_cnt < ucmd->cmd_attrib->min_params ||
				    scmds_cnt > ucmd->cmd_attrib->max_params) {
					PERROR("%s: Bad parameter nr: %d\n",
					       cmd_param_tbl[ARG_CMD],
					       scmds_cnt);
					ret = -EINVAL;
				}
				break;
			}
		}
		lret = (i < CMD_LAST) ? ret : -EINVAL;
		S_LERR(ret, lret);
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
	int cmd_idx;
	int ret = STATUS_OK;
	char *str;
	struct cmd_unit *ucmd;

	if (ci == NULL) {
		PERROR("NULL param\n");
		ret = -EINVAL;
	} else {
		cmd_idx = ARG_CMD;
		if (cclass == INFO_CMDS)
			cmd_idx = ARG_NODE;
		str = cmd_param_tbl[cmd_idx];
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
			ret = -EINVAL;
		} else {
			*ci = i;
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
	int count = 0, i = 0;
	int idx = ARG_PARAM1;
	struct cmd_attributes *ca;
	int limit = 0;
	int lret;
	int ret = STATUS_OK;
	int scmds;
	int tot_cnt = 0;
	int val = 0;

	*scmd_idx = 0;
	*scmd_cnt = 0;
	ca = cmd_lookup_tbl[cmd_idx].cmd_attrib;
	scmds = cmd_cnt - ARG_PARAM1;
	cclass = ca->class;
	switch (cclass) {
	case INFO_CMDS:
		*scmd_cnt = 0;
		return STATUS_OK;
	case CTRL_CMDS:
		/* arguments validation */
		val = -1;
		ret = -EINVAL;
		while ((idx < cmd_cnt) && (count < ca->max_params)) {
			for (i = 0; i < MAX_SUB_CMDS; i++) {
				if (val >= 0) {
					tot_cnt++;
					scmd_idx[tot_cnt] = idx - ARG_CMD - val;
					break;
				} else if (ca->scmds[i] &&
					   (strcmp(ca->scmds[i],
						   cmd_param_tbl[idx]) == 0)) {
					scmd_idx[0] = i;
					val = idx - ARG_CMD;
					ret = STATUS_OK;
					break;
				}
				count++;
			}
			idx++;
		}
		ret = (count == 0) ? STATUS_OK : ret;	/* CMD w/o args */
		break;
	case IO_AXS_CMDS:
		limit = scmds;
		/* fallthrough - no break */
	case FIO_AXS_CMDS:
		/* affect limit so we don't fail converting file name */
		limit = (limit == 0) ? scmds - 1 : limit;
		ret = STATUS_OK;

		while (((idx - ARG_PARAM1) < limit) &&
		       (count < MAX_SUB_CMDS)) {
			lret = string2ul(cmd_param_tbl[idx],
					 (unsigned long *)&val);
			if (lret == STATUS_OK) {
				scmd_idx[count + SC_BAR] = val;
				count++;
			}
			S_LERR(ret, lret);
			idx++;
		}
		/* take into account non-numeric param -last one */
		tot_cnt = (scmds - limit) + count;
		break;
	case MAX_CMDS:
	default:
		S_LERR(ret, -EINVAL);
		break;
	}
	*scmd_cnt = (ret == STATUS_OK) ? tot_cnt : 0;
	return ret;
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
	int arg_idx;
	char e_msg[MAX_ERR_MSG] = "";
	struct vk_image image[] = {{.filename  = "",
				    .type = VK_IMAGE_TYPE_BOOT1},
				   {.filename  = "",
				    .type = VK_IMAGE_TYPE_BOOT2},
				  };
	int start_idx = 0, end_idx = 0;
	struct cmd_unit *ucmd;
	size_t size;

	ucmd = &cmd_lookup_tbl[cmd_idx];
	FPR_FN("Issue command %s\n", ucmd->cmd_name);
	/* only support li at this time */
	if (strcmp("li", ucmd->cmd_name) != 0) {
		PERROR("Unsupported load command %s\n",
		       ucmd->cmd_name);
		return -EINVAL;
	}
	size = sizeof(image[0].filename);
	switch (scmd_idx[0]) {
	/* boot1 */
	case LI_BOOT1:
		start_idx = LI_BOOT1;
		end_idx = LI_BOOT1;
		if (scmd_cnt == 1)
			strncpy(image[LI_BOOT1].filename,
				cmd_param_tbl[ARG_PARAM2],
				size);
		image[LI_BOOT1].filename[size - 1] = '\0';
		break;
	/* boot2 */
	case LI_BOOT2:
		start_idx = LI_BOOT2;
		end_idx = LI_BOOT2;
		if (scmd_cnt == 1)
			strncpy(image[LI_BOOT2].filename,
				cmd_param_tbl[ARG_PARAM2],
				size);
		image[LI_BOOT2].filename[size - 1] = '\0';
		break;
	/* boot1 + boot2 */
	case LI_BOOT1_BOOT2:
		start_idx = LI_BOOT1;
		end_idx = LI_BOOT2;
		if (scmd_cnt >= 1) {
			strncpy(image[LI_BOOT1].filename,
				cmd_param_tbl[ARG_PARAM2],
				size);
			strncpy(image[LI_BOOT2].filename,
				cmd_param_tbl[ARG_PARAM3],
				size);
		}
		image[LI_BOOT1].filename[size - 1] = '\0';
		image[LI_BOOT2].filename[size - 1] = '\0';
		break;
	default:
		return -EINVAL;
	}
	for (arg_idx = start_idx; arg_idx < end_idx + 1; arg_idx++) {
		int nr_elem = ARRAY_SIZE(image);
		int ret;

		if (arg_idx >= nr_elem) {
			PERROR("VK_IO fail index out of bounds\n");
			return -EINVAL;
		}
		ret = ioctl(fd,
			    VK_IOCTL_LOAD_IMAGE,
			    &image[arg_idx]);
		if (ret < 0) {
			PERROR("VK_IOCTL_LOAD_IMAGE Err:%d - %s\n",
			       errno, strerror(errno));
			return -errno;
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
	int ret;
	struct vk_reset reset;

	/* if not forced, check for boot status */
	if (strcmp(cmd_param_tbl[ARG_PARAM1],
		   attr_lookup_tbl[CMD_RESET].scmds[0])) {
		int device_node_len = 0;
		char pcie_res[MAX_SYS_PATH];
		int val;
		int scmd[SC_MAX];

		/* reset not allowed if UCODE is not even running */
		val = atoi(strstr(path, ".") + 1);
		device_node_len = sizeof(pcie_res);
		ret = sys_path_translate(val,
					 ND_NORMAL_PATH,
					 BOOT_STATUS_BAR_NUM,
					 CMD_READ_BIN,
					 pcie_res,
					 &device_node_len);
		if (ret <  0) {
			PERROR("Fail to get PCIe info: %s\n", path);
			return -EINVAL;
		}
		scmd[SC_OFFSET] = BOOT_STATUS_REG;
		val = cmd_io(0,
			     CMD_READ_BIN,
			     scmd,
			     2 /* 2 param */,
			     pcie_res);
		if (val || (scmd[SC_VAL] == BOOT_STATUS_UCODE_NOT_RUN)) {
			PERROR("Reset skipped - UCODE not running(0x%x)\n",
			       scmd[SC_VAL]);
			return -EINVAL;
		}
	}

	reset.arg1 = 0; /* input reset type */
	reset.arg2 = 0; /* clear returned arg from card */
	FPR_FN("Issue command %s\n",
	       cmd_lookup_tbl[cmd_idx].cmd_name);

	/* only sypport reset at this time */
	ret = strcmp("reset", cmd_lookup_tbl[cmd_idx].cmd_name);
	if (ret != 0 || scmd_cnt > 0) {
		PERROR("Unsupported control command %s\n",
		       cmd_lookup_tbl[cmd_idx].cmd_name);
		return -EINVAL;
	}
	/* we could use a generic IOCTL instead */
	ret = ioctl(fd, VK_IOCTL_RESET, &reset);
	if (ret < 0) {
		PERROR("VK_IOCTL_RESET failed 0x%x Dev: %s\n", errno, path);
		return -errno;
	}
	/*
	 * check if driver returns non-zero which indicates special ramdump
	 * or standalone mode, where rescan is needed.  Inform user.
	 */
	if (reset.arg2)
		FPR_FN("VK_IOCTL_RESET ramdump/standalone mode, PCIe rescan required!\n");

	return STATUS_OK;
}

/**
 * @brief cmd_ver command handler for version reporting
 *
 * @param[in] fd - device file descriptor
 * @param[in] cmd_idx command index
 * @param[in] scmd_idx pointer to array of sub_command values
 * @param[in] scmd_cnt number of values in the sub-command array
 * @param[in] path dev_node remapped path (may differ from user path)
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_ver(int fd,
		   int cmd_idx,
		   int *scmd_idx,
		   int scmd_cnt,
		   char *path)
{
	FPR_FN("%s version %s.%s.%s\n",
	       cmd_param_tbl[ARG_SELF],
	       PKG_VERSION_MAJOR,
	       PKG_VERSION_MINOR,
	       PKG_VERSION_PATCH);

	return STATUS_OK;
}

/**
 * @brief cmd_io command handler for io access sub-commands: rb, wb
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
	int *io_data = NULL;
	int io_file = 0;
	unsigned int len = 0;
	int li = 0;
	int lret;
	struct map_info lmap_info = { {0, 0},
				      -1,
				      NULL,
				      sysconf(_SC_PAGE_SIZE) };
	off_t offset;
	int ret = STATUS_OK;
	int sys_ps = 0;
	struct cmd_unit *ucmd;

	ucmd = &cmd_lookup_tbl[cmd_idx];
	if (scmd_cnt < 2) {
		PERROR("%s: invalid io command; cnt=%d\n",
		       ucmd->cmd_name,
		       scmd_cnt);
		return -EINVAL;
	}
	offset = scmd_idx[SC_OFFSET];
	lret = pcimem_init(path,
			   &lmap_info);
	if (lret < 0) {
		PERROR("Fail to init pcimem for %s err: %d\n",
		       path,
		       lret);
		S_LERR(ret, lret);
	} else {
		sys_ps = lmap_info.map_size;
		/* default: word access - same as align */
		len = align;
		lret = pcimem_map_base(&lmap_info,
				       offset,
				       align);
		if (lret < 0) {
			PERROR("Err mem map for %s\n",
			       path);
			S_LERR(ret, lret);
		}
		io_data = malloc(len);
		if (!io_data) {
			PERROR("Err mem alloc for %s\n",
			       path);
			S_LERR(ret, -EINVAL);
		}
	}
	/* force default path on previous errors */
	if (ret < 0)
		cmd_idx = -EINVAL;
	switch (cmd_idx) {
	case CMD_READ_BIN:
		/* pcimem api allows accessing multiple locations */
		/* vkcli supports one location only for now */
		lret = pcimem_read(&lmap_info,
				   offset,
				   len,
				   io_data,
				   align);
		if (lret < 0) {
			PERROR("%s: bad rd; err=0x%x\n",
			       ucmd->cmd_name,
			       lret);
		} else {
			FPR_FN("0x%04lX: 0x%0*X\n",
			       offset,
			       2 * align,
			       *io_data);
			scmd_idx[SC_VAL] = *io_data;
		}
		break;
	case CMD_WRITE_BIN:
		/* pcimem api allows accessing multiple locations */
		/* vkcli supports one location only for now */
		*io_data = scmd_idx[SC_VAL];
		lret = pcimem_write(&lmap_info,
				    offset,
				    len,
				    io_data,
				    align);
		if (lret < 0)
			PERROR("%s: bad wr; err=0x%x\n",
			       ucmd->cmd_name,
			       lret);
		break;
	default:
		PERROR("%scmd_io bypass for %s\n",
		       ucmd->cmd_name,
		       path);
	}
	S_LERR(ret, lret);
	if (io_data != NULL)
		free(io_data);
	FPR_FN("\taccess bar done\n");
	lret = pcimem_deinit(&lmap_info);
	S_LERR(ret, lret);
	return ret;
}

/**
 * @brief cmd_fio command handler for file io access sub-commands: rf, wf
 *
 * @param[in] fd - device file descriptor
 * @param[in] cmd_idx command index
 * @param[in] scmd_idx pointer to array of sub_command values
 * @param[in] scmd_cnt number of values in the sub-command array
 * @param[in] path dev_node remapped path (may differ from user path)
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int cmd_fio(int fd,
		   int cmd_idx,
		   int *scmd_idx,
		   int scmd_cnt,
		   char *path)
{
	/* default: 32 bit align */
	int align = ALIGN_32_BIT;
	char e_msg[MAX_ERR_MSG] = "";
	int *io_data = NULL;
	int io_file = 0;
	unsigned int len = 0;
	int li = 0;
	int lret;
	struct map_info lmap_info = { {0, 0},
				      -1,
				      NULL,
				      sysconf(_SC_PAGE_SIZE) };
	off_t offset;
	int ret = STATUS_OK;
	int sys_ps = 0;
	struct cmd_unit *ucmd;

	ucmd = &cmd_lookup_tbl[cmd_idx];
	if (scmd_cnt < 3) {
		PERROR("%s: invalid fio command; cnt=%d\n",
		       ucmd->cmd_name,
		       scmd_cnt);
		return -EINVAL;
	}
	offset = scmd_idx[SC_OFFSET];
	lret = pcimem_init(path,
			   &lmap_info);
	if (lret < 0) {
		PERROR("Fail to init pcimem for %s err: %d\n",
		       path,
		       lret);
		S_LERR(ret, lret);
	}
	sys_ps = lmap_info.map_size;
	/* default: word access - same as align */
	len = align;
	/* force default path on previous errors */
	if (ret < 0)
		cmd_idx = -EINVAL;
	switch (cmd_idx) {
	case CMD_READ_FILE:
		/* by convention.. second to last is length */
		len = scmd_idx[scmd_cnt - 1];
		io_data = malloc(len);
		if (!io_data) {
			PERROR("Err mem alloc for %s\n",
			       path);
			S_LERR(ret, -EINVAL);
		}
		lmap_info.map_size = len;
		lret = pcimem_map_base(&lmap_info,
				       offset,
				       align);
		if (lret < 0)
			PERROR("Err mem map for %s\n",
			       path);
		S_LERR(ret, lret);
		if (io_data && ret == STATUS_OK) {
			lret = pcimem_blk_read(&lmap_info,
					       offset,
					       len,
					       io_data,
					       align);
			S_LERR(ret, lret);
			if (lret < 0) {
				PERROR("%s: bad rd; err=0x%x\n",
				       ucmd->cmd_name,
				       lret);
			} else {
				li = ARG_CMD + scmd_cnt;
				lret = open(cmd_param_tbl[li],
					    O_CREAT |
					    O_SYNC  |
					    O_TRUNC |
					    O_WRONLY,
					    S_IRUSR |
					    S_IWUSR |
					    S_IRGRP |
					    S_IROTH);
				io_file = lret;
				if (lret < 0) {
					PERROR("Fail to open %s\n",
					       cmd_param_tbl[li]);
					S_LERR(ret, -errno);
				} else {
					lret = write(io_file,
						     io_data,
						     len);
					if (lret < 0) {
						PERROR("Fail write %s %d\n",
						       cmd_param_tbl[li],
						       errno);
						S_LERR(ret, -errno);
					}
				}
				if (io_file >= 0) {
					lret = close(io_file);
					S_LERR(lret, -errno);
				}
			}
		}
		break;
	case CMD_WRITE_FILE:
		li = ARG_CMD + scmd_cnt;
		lret = find_size(cmd_param_tbl[li], &len);
		if (lret != STATUS_OK) {
			PERROR("%s: bad file %s; err=0x%x\n",
			       ucmd->cmd_name,
			       cmd_param_tbl[li],
			       errno);
			S_LERR(ret, -errno);
			break;
		}
		io_data = malloc(len);
		if (io_data) {
			lret = open(cmd_param_tbl[li],
				    O_RDONLY);
			io_file = lret;
			if (lret < 0) {
				PERROR("Fail to open %s\n",
				       cmd_param_tbl[li]);
				S_LERR(ret, -errno);
			} else {
				lret = read(io_file, io_data, len);
				if (lret < 0) {
					PERROR("read file %s err: %d\n",
					       cmd_param_tbl[li],
					       ret);
					S_LERR(ret, -errno);
				} else {
					len = lret;
					lmap_info.map_size = len;
					lret = pcimem_map_base(&lmap_info,
							       offset,
							       align);
					S_LERR(ret, lret);
					if (lret < 0) {
						PERROR("Err mem map for %s\n",
						       path);
					} else {
						lret =
						pcimem_blk_write(&lmap_info,
								 offset,
								 len,
								 io_data,
								 align);
						if (lret < 0)
							PERROR("%s: blk wr;\n",
							       ucmd->cmd_name);
					}
				}
				S_LERR(ret, lret);
			}
			if (io_file >= 0) {
				lret = close(io_file);
				S_LERR(lret, -errno);
			}
		}
		break;
	default:
		PERROR("%scmd_fio bypass for %s\n",
		       ucmd->cmd_name,
		       path);
	}
	S_LERR(ret, lret);
	if (io_data != NULL)
		free(io_data);
	FPR_FN("\taccess bar done\n");
	lret = pcimem_deinit(&lmap_info);
	S_LERR(ret, lret);
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
			    char path[MAX_SYS_PATH])
{
	char e_msg[MAX_ERR_MSG] = "";
	int fd = -1;
	int path_len;
	int lret;
	int ret = STATUS_OK;

	if (!path)
		return -EINVAL;

	path_len = MAX_SYS_PATH;
	lret = cmd_node_lookup(node,
			       resource,
			       CMD_MODE_EXEC,
			       cmd_idx,
			       path,
			       &path_len,
			       &fd);
	S_LERR(ret, lret);
	if (ret < 0) {
		PERROR("error in node access %s; err=0x%x",
		       path, ret);
	} else {
		lret = cmd_lookup_tbl[cmd_idx].cmd_apply(fd,
							 cmd_idx,
							 scmd_idx,
							 scmd_cnt,
							 path);
		if (lret < 0) {
			PERROR("error in apply cmd %d; err=0x%x",
			       cmd_idx, lret);
		}
		S_LERR(ret, lret);
	}
	if (fd >= 0)
		lret = close(fd);
	S_LERR(ret, lret);
	return ret;
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
	int bar = 0;
	enum cmd_class cmdc;
	enum cmd_list cmd_idx = 0;
	int data[MAX_SUB_CMDS];
	char device_path[MAX_SYS_PATH] = "";
	int dev_path_len;
	char e_msg[MAX_ERR_MSG] = "";
	enum cmd_node n_id;
	int ret = STATUS_OK;
	int scmds_found = 0;
	if (fnode == ND_INVALID) {
		print_usage();
		return STATUS_OK;
	}
	memset(data, 0, sizeof(data));
	ret = cmd_get_class(scmd_cnt, &cmdc);
	if (ret < 0) {
		PERROR("%s: bad command for class %x; err=0x%x",
		       cmd_param_tbl[ARG_CMD],
		       cmdc,
		       ret);
		return ret;
	}
	ret = cmd_lookup(cmdc, &cmd_idx);
	if (ret < 0) {
		PERROR("%s: bad command; err=0x%x",
		       cmd_param_tbl[ARG_CMD],
		       ret);
		return ret;
	}
	n_id = cmd_lookup_tbl[cmd_idx].cmd_node;
	if (n_id == ND_HELP) {
		print_usage();
		return STATUS_OK;
	}
	ret = scmd_get_param(cmd_cnt,
			     cmd_idx,
			     data,
			     &scmds_found);
	if (ret < 0) {
		PERROR("bad command; %d %s err=0x%x",
		       cmd_idx,
		       device_path,
		       ret);
		return ret;
	}
	bar = (cmdc & (IO_AXS_CMDS | FIO_AXS_CMDS)) ?
	       data[SC_BAR] * 2 : 0;
	dev_path_len = sizeof(device_path);
	ret = cmd_node_lookup(fnode,
			      bar,
			      CMD_MODE_VERIFY,
			      cmd_idx,
			      device_path,
			      &dev_path_len,
			      NULL);
	if (ret < 0) {
		PERROR("bad node; %d %s err=0x%x",
		       fnode,
		       device_path,
		       errno);
		return ret;
	}
	ret =  handle_cmd_apply(fnode,
				bar,
				cmd_idx,
				data,
				scmds_found,
				device_path);
	FPR_FN("\tcommand done");
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
	int ret;
	int cmd_cnt, scmd_cnt;

	cmd_cnt = argc;
	scmd_cnt = cmd_cnt - ARG_CMD;
	ret = is_valid_cmd(cmd_cnt, scmd_cnt, argv, &node);
	if (ret != STATUS_OK) {
		PERROR("%s / %s: invalid command / node; err=0x%x",
		       argv[ARG_CMD],
		       argv[ARG_NODE],
		       ret);
		print_usage();
		return ret;
	}
	ret = cmd_handler(cmd_cnt, scmd_cnt, node);
	/* DO NOT REMOVE */
	/* Following line used as END marker by calling scripts */
	FPR_FN("\nClose\n");
	return ret;
}
