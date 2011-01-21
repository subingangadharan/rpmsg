/*
 * Remote-processor messaging bus
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

#ifndef _DRIVERS_RPMSG_INTERNAL_H
#define _DRIVERS_RPMSG_INTERNAL_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/idr.h>
#include <linux/rpmsg.h>

/**
 * struct rpmsg_rproc - rp_msg module state
 * @vdev:	the virtio device
 * @rvq:	RX virtqueue (from pov of local processor)
 * @svq:	TX virtqueue (from pov of local processor)
 * @rbufs:	address of RX buffers
 * @sbufs:	address of TX buffers
 * ... keep documenting ...
 * @svq_lock:	protects the TX virtqueue, to allow several concurrent senders
 * @id:		remote processor id
 *
 * This structure stores the rp_msg state of a given virtio device (i.e.
 * one specific remote processor).
 */
struct rpmsg_rproc {
	struct virtio_device *vdev;
	struct virtqueue *rvq, *svq;
	void *rbufs, *sbufs;
	int last_rbuf, last_sbuf;
	void *sim_base;
	spinlock_t svq_lock;
	int id;
	int num_bufs;
	int buf_size;
	struct idr endpoints;
	spinlock_t endpoints_lock;
	struct rpmsg_channel *rpcli;
	struct rpmsg_channel *rpser;
	struct rpmsg_channel *rpomx;
};

struct rpmsg_channel *rpmsg_create_channel(struct rpmsg_rproc *rp,
				char *name, u32 src, u32 dst);
void rpmsg_destroy_channel(struct rpmsg_channel *rpdev);

#endif /* _DRIVERS_RPMSG_INTERNAL_H */
