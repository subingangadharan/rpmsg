/*
 * OMX offloading remote processor driver
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#ifndef RPMSG_OMX_H
#define RPMSG_OMX_H

#include <linux/ioctl.h>

#define OMX_IOC_MAGIC	'X'

#define OMX_IOCCONNECT	_IOW(OMX_IOC_MAGIC, 1, char *)

#define OMX_IOC_MAXNR	(1)

struct omx_conn_req {
	char name[48];
} __packed;

#endif /* RPMSG_OMX_H */
