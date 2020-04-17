/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2020 Broadcom
 */

#ifndef _VCON_CHAN_INTF_H_
#define _VCON_CHAN_INTF_H_

/**
 * interface structure, and this has to match the VK side definition.
 * we only care about the spooled part.
 */
typedef struct _logger_buf {
	uint32_t marker;         /**< marker to indicate if VK has init */
	uint32_t cmd_off;        /**< offset of cmd buffer from start */

	/*-------------------------------------------------------------------*/
	uint32_t spool_nentries; /**< total of spool entries  */
	uint32_t spool_len;      /**< length of per spooled entry */
	uint32_t spool_off;      /**< offset of spooled buffer from beginning */
	uint32_t spool_idx;      /**< idx of the next spooled buffer */
} logger_buf;

#define VCON_BUF_BAR2_OFF	 0x3800000
#define VCON_MARKER		 0xbeefcafe

/* common file name max */
#define FNAME_LEN		64

#endif
