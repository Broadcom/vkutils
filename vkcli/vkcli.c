// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2018-2021 Broadcom.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
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
#define MAX_SCMDS		20
#define MAX_SYS_PATH		200
#define MAX_BAR_ALLOWED		2
#define RESOURCE_UNUSED		0

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

/**
 * @brief system info commands
 */
enum info_cmd_list {
	CMD_HELP, /**< help command */
	CMD_VER   /**< to display version */
};

/**
 * @brief operational commands
 */
enum exe_cmd_list {
	CMD_RESET, /**< soft reset */
	CMD_LOAD_IMAGE, /**< load images to card */
	CMD_RD_BAR, /**< 4-byte read in BAR0/1/2 */
	CMD_WR_BAR, /**< 4-byte write to BAR0/1/2 */
	CMD_RD_BAR_TO_FILE, /**< read from BAR to file */
	CMD_WR_BAR_FR_FILE, /**< read from file to BAR */
};

/* fixed position/order of command line arguments */
enum arg_index {
	ARG_SELF,
	ARG_NODE,
	ARG_SYS = ARG_NODE,
	ARG_SUBCMD,
	ARG_PARAM1,
	    ARG_SC_BAR = ARG_PARAM1,
	    ARG_SC_RESET_FORCE = ARG_PARAM1,
	    ARG_SC_LI_TYPE     = ARG_PARAM1,
	ARG_PARAM2,
	    ARG_SC_BAR_OFFSET  = ARG_PARAM2,
	ARG_PARAM3,
	    ARG_SC_BAR_VAL     = ARG_PARAM3,
	    ARG_SC_RF_LEN      = ARG_PARAM3,
	    ARG_SC_WF_FILE     = ARG_PARAM3,
	ARG_PARAM4,
	    ARG_SC_RF_FILE     = ARG_PARAM4,
	ARG_PARAM5,
	ARG_LAST,
	MAX_SUB_CMDS = (ARG_LAST - ARG_PARAM1)
};

#define MIN_ARGC 2
#define ARG2C(arg) ((arg) + 1)

enum li_method {
	LI_BOOT1 = 0,
	LI_BOOT2,
	LI_BOOT1_BOOT2
};

/*
 * sub command descriptor
 */
struct cmd_attributes {
	char *scmds[MAX_SCMDS];
	int min_params;
	int max_params;
};

/* command definition */
struct cmd_def {
	const char *name;
	const struct cmd_attributes attribs;
	int (*apply)(const struct cmd_def *cmd, int argc, char *argv[],
		     const int node_id);
};

/* forward declarations */
static int cmd_help(const struct cmd_def *cmd, int argc, char *argv[],
		    const int node_id);
static int cmd_ver(const struct cmd_def *cmd, int argc, char *argv[],
		   const int node_id);
static int cmd_reset(const struct cmd_def *cmd, int argc, char *argv[],
		     const int node_id);
static int cmd_li(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id);
static int cmd_rb(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id);
static int cmd_wb(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id);
static int cmd_rf(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id);
static int cmd_wf(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id);
static int bar_rw_access_internal(const int node_id, const int bar,
				  const int offset, void *data, const int len,
				  const char *cmdname, const bool is_read);
/* system help commands */
static const struct cmd_def info_lookup_tbl[] = {
	[CMD_HELP]  = {
		.name = "--help",
		.attribs = { {""}, 0, 0 },
		.apply = cmd_help },

	[CMD_VER] = {
		.name = "--version",
		.attribs = { {""}, 0, 0 },
		.apply = cmd_ver },
};

/*
 * command lookup table - supported subcommands
 */
static const struct cmd_def cmd_lookup_tbl[] = {
	[CMD_RESET] = {
		.name = "reset",
		.attribs = { {"force"}, 0 /* min after subcmd */, 1 /* max */ },
		.apply = cmd_reset },

	[CMD_LOAD_IMAGE] = {
		.name = "li",
		.attribs = { { [LI_BOOT1] = "boot1",
			       [LI_BOOT2] = "boot2",
			       [LI_BOOT1_BOOT2] = "-" },
			       1, 3 },
		.apply = cmd_li },

	[CMD_RD_BAR]  = {
		.name = "rb",
		.attribs = { {""}, 2, 2 },
		.apply = cmd_rb },

	[CMD_WR_BAR] = {
		.name = "wb",
		.attribs = { {""}, 3, 3 },
		.apply = cmd_wb },

	[CMD_RD_BAR_TO_FILE] = {
		.name = "rf",
		.attribs = { {""}, 4, 4 },
		.apply = cmd_rf },

	[CMD_WR_BAR_FR_FILE] = {
		.name = "wf",
		.attribs = { {""}, 3, 3 },
		.apply = cmd_wf },
};

struct node_path {
#define NODE_PATH_MAX 2
	char *fmt;
	char *names[NODE_PATH_MAX];
};

/* const paths definitions */
static const struct node_path drv_path = {
	.fmt = "%s.%d",
	.names = {DEV_DRV_NAME, DEV_LEGACY_DRV_NAME},
};

static const struct node_path sys_path = {
	.fmt = "%s.%d/%s%d",
	.names = {DEV_SYSFS_NAME, NULL},
};

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
 * @brief string2l strtoul wrapper function
 *
 * @param[in] str string to convert
 * @param[out] return_value value
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int string2l(char *str, int *return_value)
{
	char *endptr = NULL;

	if (str == NULL || return_value == NULL)
		return -EINVAL;
	*return_value = strtol(str, &endptr, 0);
	if (endptr == str)
		return -EFAULT;
	if ((*return_value == LONG_MAX ||
	     *return_value == LONG_MIN))
		return -errno;

	return STATUS_OK;
}

/**
 * @brief matched_attrib_scmds match token against one of the
 *        supported scmds.
 *
 * @param[in] attrib pointer to attribute to be checked against
 * @param[in] token value of token to match
 * @param[out] idx returned idx in the scmds list
 *
 * @return true if matched, else false
 */
static bool matched_attrib_scmds(const struct cmd_attributes *attrib,
				 const char *token,
				 int *idx)
{
	int i;

	i = 0;
	for (i = 0; i < ARRAY_SIZE(attrib->scmds); i++) {
		if (attrib->scmds[i] &&
		    (strcmp(attrib->scmds[i], token) == 0)) {
			if (idx)
				*idx = i;
			return true;
		}
	}
	return false;
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
		PERROR("I/O error on file: %s, err=%d(%s)\n",
		       f_name, -errno, strerror(errno));
	} else {
		lret = ftell(fp);
		if (lret < 0)
			PERROR("I/O error on file: %s, err=%d(%s)\n",
			       f_name, -errno, strerror(errno));
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
 * @brief cmd_sysfs_open open the sysfs path node
 *
 * @param[in] path node_path structure for the opening
 * @param[in] node_id dev id
 * @param[in] resource num in case of BAR etc., not
 *            significant if for device node
 * @param[out] returned name of dev_node for later use
 *
 * @return fd file descriptor for the opened path or negative error
 */
static int cmd_sysfs_open(const struct node_path *path,
			  const int node_id,
			  const int resource,
			  char dev_node[MAX_SYS_PATH])
{
	int i;
	char e_msg[MAX_ERR_MSG] = "";
	int fd;
	int ret;

	/*
	 * try each path name in array. There are 2 formats in the
	 * path, and we formulate the path by always passing in 4
	 * parameters.  In case of format only need 2, the remaining
	 * 2 would be pushed on stack but not used.
	 */
	for (i = 0; i < ARRAY_SIZE(path->names); i++) {
		if (!path->names[i])
			continue;

		ret = snprintf(dev_node, MAX_SYS_PATH, path->fmt,
			       path->names[i], node_id,
			       DEV_SYS_RESOURCE, resource);
		if (ret >= MAX_SYS_PATH) {
			PERROR("Error formating sysfs path: %s - node %d\n",
			       path->names[i], node_id);
			continue;
		}

		fd = open(dev_node, O_RDWR);
		if (fd >= 0) {
			FPR_FN("Open %s\n", dev_node);
			return fd;
		}
	}
	return -EINVAL;
}

/**
 * @brief is_valid_cmd analyze / validate command line
 *
 * @param[in] argc
 * @param[in] argv
 * @param[out] entry command definition pointer if cmd is found and valid
 * @param[out] node_id device node id for the command decoded
 *
 * @return STATUS_OK on success, error code otherwise
 */
static int is_valid_cmd(int argc,
			char *argv[],
			const struct cmd_def **entry,
			int *node_id)
{
	char e_msg[MAX_ERR_MSG] = "";
	int i;
	char node_str[128]; /* just make it long enough */
	char limits[] = " .";
	int lret;
	int ret = STATUS_OK;
	char *str = NULL;
	int value;
	int sub_argc;
	const struct cmd_def *def;

	/* minimal parameters required */
	if (argc < MIN_ARGC) {
		PERROR("%s: insufficient arguments, min %d!",
		       argv[ARG_SELF], MIN_ARGC);
		return -EINVAL;
	}

	*entry = NULL;
	*node_id = -1;
	/* first check sys commands */
	for (i = 0; i < ARRAY_SIZE(info_lookup_tbl); i++) {
		def = &info_lookup_tbl[i];
		if (strcmp(def->name, argv[ARG_SYS]) == 0) {
			/* has to be no more parameter after this */
			if (argc == ARG2C(ARG_SYS)) {
				*entry = def;
				return STATUS_OK;
			} else {
				return -EINVAL;
			}
		}
	}

	/* if the command does not belong to sys but no more arg, it is error */
	if (argc < ARG2C(ARG_SUBCMD)) {
		PERROR("%s: insufficient sub-cmd arguments - min %d!",
		       argv[ARG_SELF], ARG2C(ARG_SUBCMD));
		return -EINVAL;
	}

	/* next check the command in the supported list */
	for (i = 0; i < ARRAY_SIZE(cmd_lookup_tbl); i++) {
		def = &cmd_lookup_tbl[i];
		if (strcmp(def->name, argv[ARG_SUBCMD]) == 0) {
			*entry = def;
			break;
		}
	}
	if (!*entry)
		return -EINVAL;

	/* check for parameter range */
	sub_argc = argc - ARG_SUBCMD - 1;
	if ((sub_argc < def->attribs.min_params) ||
	    (sub_argc > def->attribs.max_params)) {
		PERROR("%s: Invalid parameter nr: %d [min %d max %d]\n",
		       def->name, sub_argc,
		       def->attribs.min_params, def->attribs.max_params);
		return -EINVAL;
	}

	/* check for node info, make a copy as str routines may modify */
	strncpy(node_str, argv[ARG_NODE], sizeof(node_str));
	node_str[sizeof(node_str) - 1] = '\0';
	str = node_str;

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
	lret = string2l(str, &value);
	S_LERR(ret, lret);
	if (ret == STATUS_OK) {
		if ((value >= 0) && (value < MAX_CARDS_PER_HOST))
			*node_id = value;
		else
			ret = -ERANGE;
	}
	return ret;
}

/**
 * @brief cmd_li command handler for loading images
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] devid device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_li(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id)
{
	char e_msg[MAX_ERR_MSG] = "";
	int arg_idx, start_idx, end_idx;
	int li_type, size;
	int ret;
	struct vk_image image[] = {
		[LI_BOOT1] = { .filename  = "",
			       .type = VK_IMAGE_TYPE_BOOT1 },
		[LI_BOOT2] = { .filename  = "",
			       .type = VK_IMAGE_TYPE_BOOT2 },
	};
	struct img_ind {
		bool use_default;
		int arg_num;
	} ind[] = { [LI_BOOT1] = {true, 0},
		    [LI_BOOT2] = {true, 0},
	};
	int fd;
	char dev_node[MAX_SYS_PATH] = "";

	/* first check if subcmds match one of those */
	if (!matched_attrib_scmds(&cmd->attribs, argv[ARG_SC_LI_TYPE],
				  &li_type)) {
		PERROR("Image Type %s not supported!", argv[ARG_SC_LI_TYPE]);
		return -EINVAL;
	}

	/*
	 * check and see how many and which to download + check for parameter
	 * error.
	 */
	size = sizeof(image[0].filename);
	ret = 0;
	switch (li_type) {
	case LI_BOOT1:
	case LI_BOOT2:
		start_idx = li_type;
		end_idx = li_type;
		if (argc == ARG2C(ARG_PARAM2)) {
			/* user has entered param2 as the name */
			ind[li_type].use_default = false;
			ind[li_type].arg_num = ARG_PARAM2;
		} else if (argc > ARG2C(ARG_PARAM2)) {/* too many parameter */
			ret = -ERANGE;
		}
		break;
	/* boot1 + boot2 */
	case LI_BOOT1_BOOT2:
		start_idx = LI_BOOT1;
		end_idx = LI_BOOT2;
		switch (argc) {
		case (ARG2C(ARG_PARAM1)):
			/* "vkcli <node_id> li -" use all defaults */
			break;
		case (ARG2C(ARG_PARAM3)):
			/* 3 parameters, user has input boot1 + boot2 */
			ind[LI_BOOT2].use_default = false;
			ind[LI_BOOT2].arg_num = ARG_PARAM3;
		case (ARG2C(ARG_PARAM2)): /* only first image, assume boot1 */
			ind[LI_BOOT1].use_default = false;
			ind[LI_BOOT1].arg_num = ARG_PARAM2;
			break;
		default:
			ret = -ERANGE;
		}
		break;
	default:
		ret = -EINVAL;
	}
	if (ret < 0) {
		PERROR("%s: type names error; err=%d(%s)\n",
		       cmd->name, ret, strerror(-ret));
		return ret;
	}

	/* open the device */
	fd = cmd_sysfs_open(&drv_path, node_id, RESOURCE_UNUSED,
			    dev_node);
	if (fd < 0) {
		PERROR("Fails to open node %d device %s err=%d(%s)\n",
		       node_id, dev_node, errno, strerror(errno));
		return -errno;
	}

	FPR_FN("Issue command %s\n", cmd->name);
	for (arg_idx = start_idx; arg_idx <= end_idx; arg_idx++) {
		if (!ind[arg_idx].use_default) {
			strncpy(image[arg_idx].filename,
				argv[ind[arg_idx].arg_num], size);
			image[arg_idx].filename[size - 1] = '\0';
		}

		ret = ioctl(fd,
			    VK_IOCTL_LOAD_IMAGE,
			    &image[arg_idx]);
		if (ret < 0) {
			PERROR("VK_IOCTL_LOAD_IMAGE %s: err=%d(%s)\n",
			       image[arg_idx].filename,
			       -errno, strerror(errno));
			close(fd);
			return -errno;
		}
	}
	close(fd);
	return ret;
}

/**
 * @brief cmd_reset command handler for reset command
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] devid device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_reset(const struct cmd_def *cmd, int argc, char *argv[],
		     const int node_id)
{
	char e_msg[MAX_ERR_MSG] = "";
	int ret;
	int fd;
	struct vk_reset reset;
	char dev_node[MAX_SYS_PATH] = "";

	fd = cmd_sysfs_open(&drv_path, node_id, RESOURCE_UNUSED,
			    dev_node);
	if (fd < 0) {
		PERROR("Fails to open node %d device %s err=%d(%s)\n",
		       node_id, dev_node, errno, strerror(errno));
		return -errno;
	}

	/* if not forced, check for boot status */
	if ((argc == ARG2C(ARG_SUBCMD)) || /* if no additional parameter */
	    (argc >= ARG2C(ARG_SC_RESET_FORCE)) &&
	    !matched_attrib_scmds(&cmd->attribs,
				  argv[ARG_SC_RESET_FORCE], NULL)) {
		uint32_t data;

		ret = bar_rw_access_internal(node_id, BOOT_STATUS_BAR_NUM,
					     BOOT_STATUS_REG,
					     &data,
					     sizeof(data),
					     cmd->name, true);
		if (ret != STATUS_OK) {
			PERROR("%s: error access status reg 0x%x",
			       cmd->name, BOOT_STATUS_REG);
			goto err;
		} else if (data == BOOT_STATUS_UCODE_NOT_RUN) {
			PERROR("Reset skipped - UCODE not running(0x%x)\n",
			       data);
			ret = -EPERM;
			goto err;
		}
	}

	reset.arg1 = 0; /* input reset type */
	reset.arg2 = 0; /* clear returned arg from card */
	FPR_FN("Issue command %s\n", cmd->name);

	/* we could use a generic IOCTL instead */
	ret = ioctl(fd, VK_IOCTL_RESET, &reset);
	if (ret < 0) {
		PERROR("VK_IOCTL_RESET failed %d Dev: %s\n", -errno, dev_node);
		ret = -errno;
		goto err;
	}
	/*
	 * check if driver returns non-zero which indicates special ramdump
	 * or standalone mode, where rescan is needed.  Inform user.
	 */
	if (reset.arg2)
		FPR_FN("VK_IOCTL_RESET ramdump/standalone mode, PCIe rescan required!\n");
err:
	close(fd);
	return ret;
}

/**
 * @brief cmd_help command handler for help command
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] devid device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_help(const struct cmd_def *cmd, int argc, char *argv[],
		    const int node_id)
{
	print_usage();
	return STATUS_OK;
}

/**
 * @brief cmd_ver command handler for version command
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] devid device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_ver(const struct cmd_def *cmd, int argc, char *argv[],
		   const int node_id)
{
	FPR_FN("%s version %s.%s.%s+%s\n",
	       argv[ARG_SELF],
	       PKG_VERSION_MAJOR,
	       PKG_VERSION_MINOR,
	       PKG_VERSION_PATCH,
	       PKG_VERSION_META);
	return STATUS_OK;
}

/**
 * @brief bar_common_decode to decode common parameters used by bar access
 *
 * @param[in] argv argument list
 * @param[out] bar region number
 * @param[out] offset bar offset value requested
 *
 * @return STATUS_OK on success, else error
 */
static int bar_common_decode(char *argv[],
			     int *bar, int *offset)
{
	char e_msg[MAX_ERR_MSG] = "";
	int ret;

	ret = string2l(argv[ARG_SC_BAR], bar);
	if (ret != STATUS_OK)
		return ret;
	if ((*bar < 0) || (*bar > MAX_BAR_ALLOWED)) {
		PERROR("Bar num %d not in range [0 %d]\n",
		       *bar, MAX_BAR_ALLOWED);
		return -ERANGE;
	}
	ret = string2l(argv[ARG_SC_BAR_OFFSET], offset);
	if (ret != STATUS_OK)
		return ret;

	return STATUS_OK;
}

/**
 * @brief bar_access_init initialize a bar region for access
 *
 * @param[in] map_info bar map info handle
 * @param[in] node_id device node id
 * @param[in] bar region number
 * @param[in] offset in the bar region
 * @param[in] len of access
 * @param[align] align size in bytes
 *
 * @return STATUS_OK on success, else error
 */
static int bar_access_init(struct map_info *map,
			   const int node_id,
			   const int bar,
			   const int offset,
			   const int len,
			   const int align)
{
	char e_msg[MAX_ERR_MSG] = "";
	int ret;
	char dev_node[MAX_SYS_PATH] = "";
	int fd;
	int sys_ps;

	/* try to open the sysfs, and get the proper path */
	fd = cmd_sysfs_open(&sys_path, node_id, bar * 2, dev_node);
	if (fd < 0)
		return fd;
	/* file exists, check is done, close file */
	close(fd);

	memset(map, 0, sizeof(*map));
	map->fd = -1;
	map->map_size = sysconf(_SC_PAGE_SIZE);
	ret = pcimem_init(dev_node, map);
	if (ret < 0) {
		PERROR("Fail to init pcimem for %s err: %d(%s)\n",
		       dev_node, ret, strerror(-ret));
	} else {
		sys_ps = map->map_size;
		/* if default page size not big enough, update */
		if (sys_ps < len)
			map->map_size = len;
		ret = pcimem_map_base(map, offset, align);
		if (ret < 0)
			PERROR("Err mem map for %s\n", dev_node);
	}
	return ret;
}

 /**
  * @brief bar_rw_access_internal routine to perform rw access with len
  *
  * @param[in] node_id device node
  * @param[in] bar region number
  * @param[in] offset bar offset to read/write
  * @param[in/out] data pointer to value to be written or retrieve
  * @param[in] len length of rd/wr in bytes
  * @param[in] cmdname for logging
  * @param[in] is_read true for read, else write
  *
  * @return STATUS_OK on success, else error
  */
static int bar_rw_access_internal(const int node_id, const int bar,
				  const int offset, void *data, const int len,
				  const char *cmdname, const bool is_read)
{
	char e_msg[MAX_ERR_MSG] = "";
	int ret, lret;
	int align = ALIGN_32_BIT;
	struct map_info lmap_info;

	ret = bar_access_init(&lmap_info, node_id, bar, offset, len, align);
	if (ret != STATUS_OK)
		goto err;

	if (is_read) {
		ret = pcimem_blk_read(&lmap_info, offset, len, data, align);
		if (ret < 0) {
			PERROR("%s: bad rd; err=%d(%s)\n",
			       cmdname, ret, strerror(-ret));
		} else {
			/* log if single register access */
			if (len == ALIGN_32_BIT)
				FPR_FN("0x%04X: 0x%0*X\n", offset,
				       2 * ALIGN_32_BIT, *(uint32_t *)data);
		}
	} else {
		ret = pcimem_blk_write(&lmap_info, offset, len, data, align);
		if (ret < 0)
			PERROR("%s: bad wr; err=%d(%s)\n",
			       cmdname, ret, strerror(-ret));
	}
	lret = pcimem_deinit(&lmap_info);
	if (lret != STATUS_OK)
		PERROR("%s: failure to deinit mmap\n", cmdname);
err:
	FPR_FN("\taccess_bar done\n");
	return ret;
}

/**
 * @brief cmd_rb command handler for reading from the bar
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] devid device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_rb(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id)
{
	int bar, offset;
	uint32_t data;
	int ret;

	ret = bar_common_decode(argv, &bar, &offset);
	if (ret != STATUS_OK)
		return ret;

	ret = bar_rw_access_internal(node_id, bar, offset,
				     &data, sizeof(data), cmd->name, true);
	return ret;
}

/**
 * @brief cmd_wb command handler for writing to the bar
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] node_id device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_wb(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id)
{
	int bar, offset;
	int ret;
	int data;

	ret = bar_common_decode(argv, &bar, &offset);
	if (ret != STATUS_OK)
		return ret;

	ret = string2l(argv[ARG_SC_BAR_VAL], &data);
	if (ret != STATUS_OK)
		return ret;

	ret = bar_rw_access_internal(node_id, bar, offset,
				     &data, sizeof(data), cmd->name, false);
	return ret;
}

/**
 * @brief cmd_rf command handler for reading from bar to file
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] devid device id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_rf(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id)
{
	char e_msg[MAX_ERR_MSG] = "";
	int bar, offset;
	int len;
	int *io_data;
	int ret;
	int fd;
	char *fname = argv[ARG_SC_RF_FILE];

	ret = bar_common_decode(argv, &bar, &offset);
	if (ret != STATUS_OK)
		return ret;

	ret = string2l(argv[ARG_SC_RF_LEN], &len);
	if ((ret != STATUS_OK) || !len)
		return -ERANGE;

	/* open output file & allocate mem */
	io_data = malloc(len);
	if (!io_data) {
		PERROR("Err mem alloc %d for cmd %s\n", len, cmd->name);
		return -ENOMEM;
	}
	ret = bar_rw_access_internal(node_id, bar, offset,
				     io_data, len,
				     cmd->name, true /* read */);
	if (ret != STATUS_OK) {
		PERROR("%s: error reading 0x%x(%d) from bar %d, offset 0x%x\n",
		       cmd->name, len, len, bar, offset);
		goto err;
	}
	/* open file for output */
	fd = open(fname,
		  O_CREAT | O_SYNC  | O_TRUNC | O_WRONLY,
		  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		PERROR("%s: error opening output file %s\n",
		       cmd->name, fname);
		goto err;
	}
	ret = write(fd, io_data, len);
	if (ret < 0)
		PERROR("%s: Fail write %s %d\n",
		       cmd->name, fname, len);
	close(fd);
err:
	free(io_data);
	return ret;
}

/**
 * @brief cmd_wf command handler for writing to the bar from file
 *
 * @param[in] cmd pointer to definition
 * @param[in] argc num of args
 * @param[in] argv list of arguments
 * @param[in] node_id device node id formulated during validation
 *
 * @return STATUS_OK on success, error code etherwise
 */
static int cmd_wf(const struct cmd_def *cmd, int argc, char *argv[],
		  const int node_id)
{
	char e_msg[MAX_ERR_MSG] = "";
	int bar, offset;
	int len;
	int *io_data;
	int ret;
	int fd;
	char *fname = argv[ARG_SC_WF_FILE];

	ret = bar_common_decode(argv, &bar, &offset);
	if (ret != STATUS_OK)
		return ret;

	/* find size of file */
	ret = find_size(fname, &len);
	if (ret != STATUS_OK) {
		PERROR("%s: bad file %s; err=%d(%s)\n",
		       cmd->name, fname, -errno, strerror(errno));
		return ret;
	}

	/* allocate mem */
	io_data = malloc(len);
	if (!io_data) {
		PERROR("Err mem alloc %d bytes for cmd %s\n", len, cmd->name);
		return -ENOMEM;
	}

	/* open file for input */
	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		PERROR("%s: error opening output file %s: err=%d(%s)\n",
		       cmd->name, fname, -errno, strerror(errno));
		ret = fd;
		goto err;
	}
	ret = read(fd, io_data, len);
	if (ret < 0) {
		PERROR("%s: read file %s: err=%d(%s)\n",
		       cmd->name, fname, -errno, strerror(errno));
		goto free_fd;
	}
	ret = bar_rw_access_internal(node_id, bar, offset,
				     io_data, len,
				     cmd->name, false /* write */);
	if (ret != STATUS_OK) {
		PERROR("%s: error writing 0x%x(%d) to bar %d, offset 0x%x\n",
		       cmd->name, len, len, bar, offset);
		goto free_fd;
	}
free_fd:
	close(fd);
err:
	free(io_data);
	return ret;
}

/**
 * @brief main standard entry point
 */
int main(int argc, char *argv[])
{
	char e_msg[MAX_ERR_MSG] = "";
	int ret;
	const struct cmd_def *cmd;
	int node_id;

	ret = is_valid_cmd(argc, argv, &cmd, &node_id);
	if (ret != STATUS_OK) {
		PERROR("\"%s\" / \"%s\": sub command / node; err=%d(%s)",
		       argc < ARG2C(ARG_SUBCMD) ? "n/a" : argv[ARG_SUBCMD],
		       argc < ARG2C(ARG_NODE) ? "n/a" : argv[ARG_NODE],
		       ret, strerror(-ret));
		print_usage();
		return ret;
	}
	ret = cmd->apply(cmd, argc, argv, node_id);
	if (ret < 0)
		PERROR("error in apply cmd %s; err=%d(%s)",
		       cmd->name, ret, strerror(-ret));

	FPR_FN("\tcommand done\n");
	/* DO NOT REMOVE */
	/* Following line used as END marker by calling scripts */
	FPR_FN("Close\n");
	return ret;
}
