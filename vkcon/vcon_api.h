/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2019 Broadcom
 */

#ifndef _VCON_API_H_
#define _VCON_API_H_

/* start and end of virtual console cmd channel */
#define VCON_ENABLE		"enable"
#define VCON_DISABLE		"disable"

/* start and end of virtual console cmd channel */
#define VCON_COLOR_ON		"color_on"
#define VCON_COLOR_OFF		"color_off"

/* cmd buffer is 128 bytes, with 1 byte as header/control */
#define VCON_MAX_CMD_SIZE	127
#define VCON_CMD_CHAN_SIZE	(VCON_MAX_CMD_SIZE + 1)

/* device node path max size */
#define FNAME_LEN		64

int vcon_open_cmd_chan(void **ctx,
		       const char *dev_name,
		       size_t *mapped_size);
int vcon_close_cmd_chan(void *ctx);
int vcon_get_cmd_output(void *ctx,
			char *buf,
			const size_t buf_size);
int vcon_send_cmd(void *ctx, const char *cmd);

#endif
