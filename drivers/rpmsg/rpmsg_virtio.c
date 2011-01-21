/*
 * Virtio-based remote processor messaging bus
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

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/rpmsg.h>
#include <linux/idr.h>
#include <linux/radix-tree.h>

#include "rpmsg_internal.h"

struct rpmsg_hdr {
	u16 len;
	u16 flags;
	u32 src;
	u32 dst;
	u32 unused;
	u8 data[0];
} __packed;

/*
 * Local addresses are dynamically allocated on-demand.
 * We do not dynamically assign addresses from the low 1024 range,
 * in order to reserve that address range for predefined services.
 */
#define RP_MSG_RESERVED_ADDRESSES	(1024)

/* Reserve address 500 for rpmsg devices creation service */
#define RPMSG_FACTORY_ADDR		(500)

/* Reserve address 60 for the OMX connection service */
#define RPMSG_OMX_ADDR		(60)

/* assign a new local address, and bind it to the user's callback function */
struct rpmsg_endpoint *rpmsg_create_ept(struct rpmsg_channel *rpdev,
		void (*cb)(struct rpmsg_channel *, void *, int, void *, u32),
		void *priv, u32 addr)
{
	int err, tmpaddr, request;
	struct rpmsg_endpoint *ept;
	struct rpmsg_rproc *rp = rpdev->rp;

	if (!idr_pre_get(&rp->endpoints, GFP_KERNEL))
		return NULL;

	ept = kzalloc(sizeof(*ept), GFP_KERNEL);
	if (!ept) {
		dev_err(&rpdev->dev, "failed to kzalloc a new ept\n");
		return NULL;
	}

	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;

	request = addr == RPMSG_ADDR_ANY ? RP_MSG_RESERVED_ADDRESSES : addr;

	spin_lock(&rp->endpoints_lock);

	/* dynamically assign a new address outside the reseved range */
	err = idr_get_new_above(&rp->endpoints, ept, request, &tmpaddr);
	if (err) {
		dev_err(&rpdev->dev, "idr_get_new_above failed: %d\n", err);
		goto free_ept;
	}

	if (addr != RPMSG_ADDR_ANY && tmpaddr != addr) {
		dev_err(&rpdev->dev, "address 0x%x already in use\n", addr);
		goto rem_idr;
	}

	ept->addr = tmpaddr;

	spin_unlock(&rp->endpoints_lock);

	return ept;

rem_idr:
	idr_remove(&rp->endpoints, request);
free_ept:
	spin_unlock(&rp->endpoints_lock);
	kfree(ept);
	return NULL;
}
EXPORT_SYMBOL_GPL(rpmsg_create_ept);

void rpmsg_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct rpmsg_rproc *rp = ept->rpdev->rp;

	spin_lock(&rp->endpoints_lock);
	idr_remove(&rp->endpoints, ept->addr);
	spin_unlock(&rp->endpoints_lock);

	kfree(ept);
}
EXPORT_SYMBOL_GPL(rpmsg_destroy_ept);

/* horrible buf "allocator" that is just enough for now */
static void *get_a_buf(struct rpmsg_rproc *rp)
{
	unsigned int len;

	/* either pick the next unused buffer */
	if (rp->last_sbuf < rp->num_bufs / 2)
		return rp->sbufs + rp->buf_size * rp->last_sbuf++;
	/* or recycle a used one */
	else
		return virtqueue_get_buf(rp->svq, &len);
}

int rpmsg_send_offchannel(struct rpmsg_channel *rpdev, u32 src, u32 dst,
					void *data, int len)
{
	struct rpmsg_rproc *rp = rpdev->rp;
	struct scatterlist sg;
	struct rpmsg_hdr *msg;
	int err;
	unsigned long offset;
	void *sim_addr;

	if (src == RPMSG_ADDR_ANY || dst == RPMSG_ADDR_ANY) {
		dev_err(&rpdev->dev, "invalid address (src 0x%x, dst 0x%x)\n",
				src, dst);
		return -EINVAL;
	}

	/* payloads sizes are currently limited */
	if (len > rp->buf_size - sizeof(struct rpmsg_hdr)) {
		dev_err(&rpdev->dev, "message is too big (%d)\n", len);
		return -EMSGSIZE;
	}

	/* grab a buffer. todo: add blocking support in case no buf is free */
	msg = get_a_buf(rp);
	if (!msg)
		return -ENOMEM;

	msg->len = len;
	msg->flags = 0;
	msg->src = src;
	msg->dst = dst;
	msg->unused = 0;
	memcpy(msg->data, data, len);

	pr_debug("From: 0x%x, To: 0x%x, Len: %d, Flags: %d, Unused: %d\n",
					msg->src, msg->dst, msg->len,
					msg->flags, msg->unused);
	print_hex_dump(KERN_DEBUG, "rpmsg_virtio TX: ", DUMP_PREFIX_NONE, 16, 1,
					msg, sizeof(*msg) + msg->len, true);

	offset = ((unsigned long) msg) - ((unsigned long) rp->rbufs);
	sim_addr = rp->sim_base + offset;
	sg_init_one(&sg, sim_addr, sizeof(*msg) + len);

	/* protect svq from simultaneous concurrent manipulations */
	spin_lock(&rp->svq_lock);

	/* add message to the remote processor's virtqueue */
	err = virtqueue_add_buf_gfp(rp->svq, &sg, 1, 0, msg, GFP_KERNEL);
	if (err < 0) {
		pr_err("failed to add a virtqueue buffer: %d\n", err);
		goto out;
	}

	/* tell the remote processor it has a pending message to read */
	virtqueue_kick(rp->svq);

	err = 0;
out:
	spin_unlock(&rp->svq_lock);
	return err;
}
EXPORT_SYMBOL_GPL(rpmsg_send_offchannel);

int rpmsg_send(struct rpmsg_channel *rpdev, void *data, int len)
{
	return rpmsg_send_offchannel(rpdev, rpdev->src, rpdev->dst, data, len);
}
EXPORT_SYMBOL_GPL(rpmsg_send);

int rpmsg_sendto(struct rpmsg_channel *rpdev, void *data, int len, u32 dst)
{
	return rpmsg_send_offchannel(rpdev, rpdev->src, dst, data, len);
}
EXPORT_SYMBOL_GPL(rpmsg_sendto);

static void rpmsg_recv_done(struct virtqueue *rvq)
{
	struct rpmsg_hdr *msg;
	unsigned int len;
	struct rpmsg_endpoint *ept;
	struct scatterlist sg;
	unsigned long offset;
	void *sim_addr;
	struct rpmsg_rproc *rp = rvq->vdev->priv;
	int err;

	msg = virtqueue_get_buf(rvq, &len);
	if (!msg) {
		pr_err("uhm, incoming signal, but no used buffer ?\n");
		return;
	}

	pr_debug("From: 0x%x, To: 0x%x, Len: %d, Flags: %d, Unused: %d\n",
					msg->src, msg->dst, msg->len,
					msg->flags, msg->unused);
	print_hex_dump(KERN_DEBUG, "rpmsg_virtio RX: ", DUMP_PREFIX_NONE, 16, 1,
					msg, sizeof(*msg) + msg->len, true);

	/* fetch the callback of the appropriate user */
	spin_lock(&rp->endpoints_lock);
	ept = idr_find(&rp->endpoints, msg->dst);
	spin_unlock(&rp->endpoints_lock);

	if (ept && ept->cb)
		ept->cb(ept->rpdev, msg->data, msg->len, ept->priv, msg->src);
	else
		pr_warn("msg received with no recepient\n");

	/* add the buffer back to the remote processor's virtqueue */
	offset = ((unsigned long) msg) - ((unsigned long) rp->rbufs);
	sim_addr = rp->sim_base + offset;
	sg_init_one(&sg, sim_addr, sizeof(*msg) + len);

	err = virtqueue_add_buf_gfp(rp->rvq, &sg, 0, 1, msg, GFP_KERNEL);
	if (err < 0) {
		pr_err("failed to add a virtqueue buffer: %d\n", err);
		return;
	}

	/* tell the remote processor we added another available rx buffer */
	virtqueue_kick(rp->rvq);
}

static void rpmsg_xmit_done(struct virtqueue *svq)
{
	pr_warn("BIOS did not obey virtqueue_disable_cb(rp->svq)\n");
}

static int rpmsg_probe(struct virtio_device *vdev)
{
	vq_callback_t *callbacks[] = { rpmsg_recv_done, rpmsg_xmit_done };
	const char *names[] = { "input", "output" };
	struct virtqueue *vqs[2];
	struct rpmsg_rproc *rp;
	void *addr;
	int err, i, id, num_bufs, buf_size, total_buf_size;

	rp = kzalloc(sizeof(*rp), GFP_KERNEL);
	if (!rp)
		return -ENOMEM;

	rp->vdev = vdev;

	idr_init(&rp->endpoints);
	spin_lock_init(&rp->endpoints_lock);
	spin_lock_init(&rp->svq_lock);

	/* We expect two virtqueues, receive then send */
	err = vdev->config->find_vqs(vdev, 2, vqs, callbacks, names);
	if (err)
		goto free_vi;

	rp->rvq = vqs[0];
	rp->svq = vqs[1];

	/* Platform must supply the id of this remote processor device.
	 * consider changing this to an optional virtio feature */
	vdev->config->get(vdev, VIRTIO_IPC_PROC_ID, &id, sizeof(id));

	rp->id = id;

	/* Platform must supply pre-allocated uncached buffers for now */
	vdev->config->get(vdev, VIRTIO_IPC_BUF_ADDR, &addr, sizeof(addr));
	vdev->config->get(vdev, VIRTIO_IPC_BUF_NUM, &num_bufs,
							sizeof(num_bufs));
	vdev->config->get(vdev, VIRTIO_IPC_BUF_SZ, &buf_size, sizeof(buf_size));

	total_buf_size = num_bufs * buf_size;

	dev_dbg(&vdev->dev, "%d buffers, size %d, addr 0x%x, total 0x%x\n",
		num_bufs, buf_size, (unsigned int) addr, total_buf_size);

	rp->num_bufs = num_bufs;
	rp->buf_size = buf_size;
	rp->rbufs = addr;
	rp->sbufs = addr + total_buf_size / 2;

	/* simulated addr base to make virt_to_page happy. consider using
	 * virtio features for that */
	vdev->config->get(vdev, VIRTIO_IPC_SIM_BASE, &rp->sim_base,
							sizeof(rp->sim_base));

	/* set up the receive buffers */
	/* note: those RP_MSG_* macros should be retrieved from the platform
	 * and not compiled into the driver... */
	for (i = 0; i < num_bufs/2; i++) {
		struct scatterlist sg;
		void *tmpaddr = rp->rbufs + i * buf_size;
		void *simaddr = rp->sim_base + i * buf_size;

		sg_init_one(&sg, simaddr, buf_size);
		err = virtqueue_add_buf_gfp(rp->rvq, &sg, 0, 1, tmpaddr,
								GFP_KERNEL);
		WARN_ON(err < 0); /* sanity check; this can't happen */
	}

	/* tell the remote processor it can start sending data */
	virtqueue_kick(rp->rvq);

	/* suppress "tx-complete" interrupts */
	virtqueue_disable_cb(rp->svq);

	vdev->priv = rp;

	dev_info(&vdev->dev, "rpmsg backend dev %d probed successfully\n", id);

	/* manual hack: create rpmsg devices */
	if (id == 0) {
		rp->rpcli = rpmsg_create_channel(rp, "rpmsg-client-sample", RPMSG_ADDR_ANY, 50);
		rp->rpser = rpmsg_create_channel(rp, "rpmsg-server-sample", 137, RPMSG_ADDR_ANY);
		rp->rpomx = rpmsg_create_channel(rp, "rpmsg-omx", RPMSG_ADDR_ANY, RPMSG_OMX_ADDR);
	} else if (id == 1) {
		rp->rpcli = rpmsg_create_channel(rp, "rpmsg-client-sample", RPMSG_ADDR_ANY, 51);
		rp->rpomx = rpmsg_create_channel(rp, "rpmsg-omx", RPMSG_ADDR_ANY, RPMSG_OMX_ADDR);
	}

	return 0;

free_vi:
	kfree(rp);
	return err;
}

static void __devexit rpmsg_remove(struct virtio_device *vdev)
{
	struct rpmsg_rproc *rp = vdev->priv;

	/* cheap hack */
	if (rp->id == 0) {
		rpmsg_destroy_channel(rp->rpcli);
		rpmsg_destroy_channel(rp->rpser);
		rpmsg_destroy_channel(rp->rpomx);
	} else if (rp->id == 1) {
		rpmsg_destroy_channel(rp->rpcli);
		rpmsg_destroy_channel(rp->rpomx);
	}

	vdev->config->del_vqs(rp->vdev);

	idr_remove_all(&rp->endpoints);
	idr_destroy(&rp->endpoints);
	kfree(rp);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_RPMSG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_ipc_driver = {
	.driver.name	= KBUILD_MODNAME,
	.driver.owner	= THIS_MODULE,
	.id_table	= id_table,
	.probe		= rpmsg_probe,
	.remove		= __devexit_p(rpmsg_remove),
};

/* tmp hack */
int __init rpmsg_bus_init(void);
void __exit rpmsg_bus_fini(void);

static int __init init(void)
{
	rpmsg_bus_init(); /* clean me up */
	return register_virtio_driver(&virtio_ipc_driver);
}
module_init(init);

static void __exit fini(void)
{
	unregister_virtio_driver(&virtio_ipc_driver);
	rpmsg_bus_fini();
}
module_exit(fini);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio-based remote processor messaging bus");
MODULE_LICENSE("GPL v2");
