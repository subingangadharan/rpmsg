/*
 * Remote processor messaging transport (OMAP platform-specific bits)
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Authors: Ohad Ben-Cohen <ohad@wizery.com>
 *          Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/interrupt.h>
#include <linux/virtio_ring.h>
#include <linux/rpmsg.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/memblock.h>
#include <linux/remoteproc.h>
#include <asm/io.h>

#include <plat/mailbox.h>
#include <plat/dsp.h>

/*
 * enum - Predefined Mailbox Messages
 *
 * @RP_MBOX_READY: informs the M3's that we're up and running. will be
 * followed by another mailbox message that carries the A9's virtual address
 * of the shared buffer. This would allow the A9's drivers to send virtual
 * addresses of the buffers.
 *
 * @RP_MBOX_PENDING_MSG: informs the receiver that there is an inbound
 * message waiting in its own receive-side vring. please note that currently
 * this message is optional: alternatively, one can explicitly send the index
 * of the triggered virtqueue itself. the preferred approach will be decided
 * as we progress and experiment with those design ideas.
 *
 * @RP_MBOX_CRASH: this message is sent upon a BIOS exception
 *
 * @RP_MBOX_ECHO_REQUEST: a mailbox-level "ping" message.
 *
 * @RP_MBOX_ECHO_REPLY: a mailbox-level reply to a "ping"
 *
 * @RP_MBOX_ABORT_REQUEST: a "please crash" request, used for testing the
 * recovery mechanism (to some extent). will trigger a @RP_MBOX_CRASH reply.
 */
enum {
	RP_MBOX_READY		= 0xFFFFFF00,
	RP_MBOX_PENDING_MSG	= 0xFFFFFF01,
	RP_MBOX_CRASH		= 0xFFFFFF02,
	RP_MBOX_ECHO_REQUEST	= 0xFFFFFF03,
	RP_MBOX_ECHO_REPLY	= 0xFFFFFF04,
	RP_MBOX_ABORT_REQUEST	= 0xFFFFFF05,
};

struct omap_rpmsg_vproc {
	struct virtio_device vdev;
	unsigned int vring[2]; /* A9 owns first vring, M3-core0 owns the 2nd */
	unsigned int buf_addr;
	unsigned int buf_size; /* must be page-aligned */
	void *buf_mapped;
	char *mbox_name;
	char *rproc_name;
	struct omap_mbox *mbox;
	struct rproc *rproc;
	struct notifier_block nb;
	struct virtqueue *vq[2];
	int base_vq_id;
	int num_of_vqs;
	struct rpmsg_channel_hdr *hardcoded_chnls;
};

#define to_omap_rpdev(vd) container_of(vd, struct omap_rpmsg_vproc, vdev)

struct omap_rpmsg_vq_info {
	__u16 num;	/* number of entries in the virtio_ring */
	__u16 vq_id;	/* a globaly unique index of this virtqueue */
	void *addr;	/* address where we mapped the virtio ring */
	struct omap_rpmsg_vproc *rpdev;
};

/*
 * For now, allocate 256 buffers of 512 bytes for each side. each buffer
 * will have 16B for the msg header and 496B for the payload.
 * This will require a total space of 256KB for the buffers themselves, and
 * 3 pages for every vring (the size of the vring depends on the number of
 * buffers it supports).
 */
#define RP_MSG_NUM_BUFS		(512)
#define RP_MSG_BUF_SIZE		(512)
#define RP_MSG_BUFS_SPACE	(RP_MSG_NUM_BUFS * RP_MSG_BUF_SIZE)

/*
 * The alignment to use between consumer and producer parts of vring.
 * Note: this is part of the "wire" protocol. If you change this, you need
 * to update your BIOS image as well
 */
#define RP_MSG_VRING_ALIGN	(4096)
/* With 256 buffers, our vring will occupy 3 pages */
#define RP_MSG_RING_SIZE	((DIV_ROUND_UP(vring_size(RP_MSG_NUM_BUFS/2, \
				RP_MSG_VRING_ALIGN), PAGE_SIZE)) * PAGE_SIZE)

/* provide drivers with platform-specific details */
static void omap_rpmsg_get(struct virtio_device *vdev, unsigned int request,
		   void *buf, unsigned len)
{
	struct omap_rpmsg_vproc *rpdev = to_omap_rpdev(vdev);
	void *base;
	int num_bufs, buf_size;

	/* todo: remove WARN_ON, do sane length validations */
	switch (request) {
	case VPROC_BUF_ADDR:
		WARN_ON(len != sizeof(rpdev->buf_mapped));
		memcpy(buf, &rpdev->buf_mapped, min(len, sizeof(void *)));
		break;
	case VPROC_SIM_BASE:
		WARN_ON(len != sizeof(base));
		/*
		 * calculate a simulated base address to make virtio's
		 * virt_to_page() happy.
		 */
		base = __va(rpdev->buf_addr);
		memcpy(buf, &base, len);
		break;
	case VPROC_BUF_NUM:
		num_bufs = RP_MSG_NUM_BUFS;
		memcpy(buf, &num_bufs, min(len, sizeof(num_bufs)));
		break;
	case VPROC_BUF_SZ:
		buf_size = RP_MSG_BUF_SIZE;
		memcpy(buf, &buf_size, min(len, sizeof(buf_size)));
		break;
	case VPROC_HC_CHANNELS:
		WARN_ON(len != sizeof(rpdev->hardcoded_chnls));
		memcpy(buf, &rpdev->hardcoded_chnls, min(len, sizeof(void *)));
		break;
	default:
		pr_err("invalid request: %d\n", request);
	}
}

/* kick the remote processor, and let it know which virtqueue to poke at */
static void omap_rpmsg_notify(struct virtqueue *vq)
{
	struct omap_rpmsg_vq_info *rpvq = vq->priv;
	int ret;

	pr_debug("sending mailbox msg: %d\n", rpvq->vq_id);
	/* send the index of the triggered virtqueue as the mailbox payload */
	ret = omap_mbox_msg_send(rpvq->rpdev->mbox, rpvq->vq_id);
	if (ret)
		pr_err("ugh, omap_mbox_msg_send() failed: %d\n", ret);
}

static int omap_rpmsg_mbox_callback(struct notifier_block *this,
					unsigned long index, void *data)
{
	mbox_msg_t msg = (mbox_msg_t) data;
	struct omap_rpmsg_vproc *rpdev;

	rpdev = container_of(this, struct omap_rpmsg_vproc, nb);

	pr_debug("mbox msg: 0x%x\n", msg);

	switch (msg) {
	case RP_MBOX_CRASH:
		pr_err("%s has just crashed !\n", rpdev->rproc_name);
		/* todo: smarter error handling here */
		break;
	case RP_MBOX_ECHO_REPLY:
		pr_info("received echo reply from %s !\n", rpdev->rproc_name);
		break;
	case RP_MBOX_PENDING_MSG:
		/*
		 * a new inbound message is waiting in our own vring (index 0).
		 * Let's pretend the message explicitly contained the vring
		 * index number and handle it generically
		 */
		msg = rpdev->base_vq_id;
		/* intentional fall-through */
	default:
		/* ignore vq indices which are clearly not for us */
		if (msg < rpdev->base_vq_id)
			break;

		msg -= rpdev->base_vq_id;

		/*
		 * Currently both PENDING_MSG and explicit-virtqueue-index
		 * messaging are supported.
		 * Whatever approach is taken, at this point 'msg' contains
		 * the index of the vring which was just triggered.
		 */
		if (msg < rpdev->num_of_vqs)
			vring_interrupt(msg, rpdev->vq[msg]);
	}

	return NOTIFY_DONE;
}

static struct virtqueue *rp_find_vq(struct virtio_device *vdev,
				    unsigned index,
				    void (*callback)(struct virtqueue *vq),
				    const char *name)
{
	struct omap_rpmsg_vproc *rpdev = to_omap_rpdev(vdev);
	struct omap_rpmsg_vq_info *rpvq;
	struct virtqueue *vq;
	int err;

	rpvq = kmalloc(sizeof(*rpvq), GFP_KERNEL);
	if (!rpvq)
		return ERR_PTR(-ENOMEM);

	/* map the vring using uncacheable memory (which is ioremap's default,
	 * but let's make it explicit) and cast away sparse's complaints */
	rpvq->addr = (__force void *) ioremap_nocache(rpdev->vring[index],
							RP_MSG_RING_SIZE);
	if (!rpvq->addr) {
		err = -ENOMEM;
		goto free_rpvq;
	}

	memset(rpvq->addr, 0, RP_MSG_RING_SIZE);

	pr_debug("vring%d: phys 0x%x, virt 0x%x\n", index, rpdev->vring[index],
					(unsigned int) rpvq->addr);

	vq = vring_new_virtqueue(RP_MSG_NUM_BUFS/2, RP_MSG_VRING_ALIGN, vdev,
				rpvq->addr, omap_rpmsg_notify, callback, name);
	if (!vq) {
		pr_err("vring_new_virtqueue failed\n");
		err = -ENOMEM;
		goto unmap_vring;
	}

	rpdev->vq[index] = vq;
	vq->priv = rpvq;
	/* this globally identifies the id of the vring */
	rpvq->vq_id = rpdev->base_vq_id + index;
	rpvq->rpdev = rpdev;

	return vq;

unmap_vring:
	iounmap((__force void __iomem *)rpvq->addr);
free_rpvq:
	kfree(rpvq);
	return ERR_PTR(err);
}

static void omap_rpmsg_del_vqs(struct virtio_device *vdev)
{
	struct virtqueue *vq, *n;
	struct omap_rpmsg_vproc *rpdev = to_omap_rpdev(vdev);

	list_for_each_entry_safe(vq, n, &vdev->vqs, list) {
		struct omap_rpmsg_vq_info *rpvq = vq->priv;
		vring_del_virtqueue(vq);
		kfree(rpvq);
	}

	if (rpdev->mbox)
		omap_mbox_put(rpdev->mbox, &rpdev->nb);

	if (rpdev->rproc)
		rproc_put(rpdev->rproc);
}

static int omap_rpmsg_find_vqs(struct virtio_device *vdev, unsigned nvqs,
		       struct virtqueue *vqs[],
		       vq_callback_t *callbacks[],
		       const char *names[])
{
	struct omap_rpmsg_vproc *rpdev = to_omap_rpdev(vdev);
	int i, err;

	/* assume a single remote processor for now (and therefore 2 vqs) */
	if (nvqs != 2)
		return -EINVAL;

	for (i = 0; i < nvqs; ++i) {
		vqs[i] = rp_find_vq(vdev, i, callbacks[i], names[i]);
		if (IS_ERR(vqs[i])) {
			err = PTR_ERR(vqs[i]);
			goto error;
		}
	}

	rpdev->num_of_vqs = nvqs;

	/* can be used as normal memory, so we cast away sparse's complaints */
	rpdev->buf_mapped = (__force void *) ioremap_nocache(rpdev->buf_addr,
							rpdev->buf_size);
	if (!rpdev->buf_mapped) {
		pr_err("ioremap failed\n");
		err = -ENOMEM;
		goto error;
	}

	/* use mailbox's notifiers. later that can be optimized */
	rpdev->nb.notifier_call = omap_rpmsg_mbox_callback;
	rpdev->mbox = omap_mbox_get(rpdev->mbox_name, &rpdev->nb);
	if (IS_ERR(rpdev->mbox)) {
		pr_err("failed to get mailbox %s\n", rpdev->mbox_name);
		err = -EINVAL;
		goto unmap_buf;
	}

	pr_debug("buf: phys 0x%x, virt 0x%x\n", rpdev->buf_addr,
					(unsigned int) rpdev->buf_mapped);

	/* tell the M3 we're ready. hmm. do we really need this msg */
	err = omap_mbox_msg_send(rpdev->mbox, RP_MBOX_READY);
	if (err) {
		pr_err("ugh, omap_mbox_msg_send() failed: %d\n", err);
		goto put_mbox;
	}

	/* send it the physical address of the mapped buffer + vrings, */
	/* this should be moved to the resource table logic */
	err = omap_mbox_msg_send(rpdev->mbox, (mbox_msg_t) rpdev->buf_addr);
	if (err) {
		pr_err("ugh, omap_mbox_msg_send() failed: %d\n", err);
		goto put_mbox;
	}

	/* ping the remote processor. this is only for fun (i.e. sanity);
	 * there is no functional effect whatsoever */
	err = omap_mbox_msg_send(rpdev->mbox, RP_MBOX_ECHO_REQUEST);
	if (err) {
		pr_err("ugh, omap_mbox_msg_send() failed: %d\n", err);
		goto put_mbox;
	}

	/* load the firmware, and take the M3 out of reset */
	rpdev->rproc = rproc_get(rpdev->rproc_name);
	if (!rpdev->rproc) {
		pr_err("failed to get rproc %s\n", rpdev->rproc_name);
		err = -EINVAL;
	}

	return 0;

put_mbox:
	omap_mbox_put(rpdev->mbox, &rpdev->nb);
unmap_buf:
	iounmap((__force void __iomem *)rpdev->buf_mapped);
error:
	omap_rpmsg_del_vqs(vdev);
	return err;
}

/*
 * no real use case for these handlers right now, but virtio expects us to
 * provide them and otherwise crashes horribly.
 */
static u8 omap_rpmsg_get_status(struct virtio_device *vdev)
{
	return 0;
}

static void omap_rpmsg_set_status(struct virtio_device *vdev, u8 status)
{
}

static void omap_rpmsg_reset(struct virtio_device *vdev)
{
}

static u32 omap_rpmsg_get_features(struct virtio_device *vdev)
{
	return 0;
}

static void omap_rpmsg_finalize_features(struct virtio_device *vdev)
{
}

static void omap_rpmsg_vproc_release(struct device *dev)
{
}

static struct virtio_config_ops omap_rpmsg_config_ops = {
	.get_features	= omap_rpmsg_get_features,
	.finalize_features = omap_rpmsg_finalize_features,
	.get		= omap_rpmsg_get,
	.find_vqs	= omap_rpmsg_find_vqs,
	.del_vqs	= omap_rpmsg_del_vqs,
	.reset		= omap_rpmsg_reset,
	.set_status	= omap_rpmsg_set_status,
	.get_status	= omap_rpmsg_get_status,
};

struct rpmsg_channel_hdr omap_ipuc0_hardcoded_chnls[] = {
	{ "rpmsg-client-sample", RPMSG_ADDR_ANY, 50 },
	{ "rpmsg-server-sample", 137, RPMSG_ADDR_ANY },
	{ "rpmsg-omx", RPMSG_ADDR_ANY, 60 },
	{ },
};

struct rpmsg_channel_hdr omap_ipuc1_hardcoded_chnls[] = {
	{ "rpmsg-client-sample", RPMSG_ADDR_ANY, 51 },
	{ "rpmsg-omx", RPMSG_ADDR_ANY, 60 },
	{ },
};

static struct omap_rpmsg_vproc omap_rpmsg_vprocs[] = {
	/* rpmsg ipu_c0 backend */
	{
		.vdev.id.device	= VIRTIO_ID_RPMSG,
		.vdev.config	= &omap_rpmsg_config_ops,
		.mbox_name	= "mailbox-1",
		.rproc_name	= "ipu",
		.base_vq_id	= 0,
		.hardcoded_chnls = omap_ipuc0_hardcoded_chnls,
	},
	/* rpmsg ipu_c1 backend */
	{
		.vdev.id.device	= VIRTIO_ID_RPMSG,
		.vdev.config	= &omap_rpmsg_config_ops,
		.mbox_name	= "mailbox-1",
		.rproc_name	= "ipu",
		.base_vq_id	= 2,
		.hardcoded_chnls = omap_ipuc1_hardcoded_chnls,
	},
};

static int __init omap_rpmsg_ini(void)
{
	int i, ret = 0;
	phys_addr_t paddr = omap_dsp_get_mempool_base();
	phys_addr_t psize = omap_dsp_get_mempool_size();

	for (i = 0; i < ARRAY_SIZE(omap_rpmsg_vprocs); i++) {
		struct omap_rpmsg_vproc *rpdev = &omap_rpmsg_vprocs[i];

		/* clean this up: alignments, ring size calc, next rpdev */
		rpdev->buf_addr = paddr;
		rpdev->buf_size = RP_MSG_BUFS_SPACE;
		rpdev->vring[0] = paddr + RP_MSG_BUFS_SPACE;
		rpdev->vring[1] = paddr + RP_MSG_BUFS_SPACE + 0x3000;
		paddr += 0x50000;
		psize -= 0x50000;

		pr_debug("rpdev%d: buf 0x%x, vring0 0x%x, vring1 0x%x\n", i,
			rpdev->buf_addr, rpdev->vring[0], rpdev->vring[1]);

		rpdev->vdev.dev.release = omap_rpmsg_vproc_release;

		ret = register_virtio_device(&rpdev->vdev);
		if (ret) {
			pr_err("failed to register rpdev: %d\n", ret);
			break;
		}
	}

	return ret;
}
module_init(omap_rpmsg_ini);

static void __exit omap_rpmsg_fini(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(omap_rpmsg_vprocs); i++) {
		struct omap_rpmsg_vproc *rpdev = &omap_rpmsg_vprocs[i];

		unregister_virtio_device(&rpdev->vdev);
	}
}
module_exit(omap_rpmsg_fini);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OMAP Remote processor messaging virtio device");
