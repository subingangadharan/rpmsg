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

/**
 * struct virtproc_info - rp_msg module state
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
struct virtproc_info {
	struct virtio_device *vdev;
	struct virtqueue *rvq, *svq;
	void *rbufs, *sbufs;
	int last_rbuf, last_sbuf;
	void *sim_base;
	spinlock_t svq_lock;
	int num_bufs;
	int buf_size;
	struct idr endpoints;
	spinlock_t endpoints_lock;
};

#define to_rpmsg_channel(d) container_of(d, struct rpmsg_channel, dev)
#define to_rpmsg_driver(d) container_of(d, struct rpmsg_driver, drv)

/*
 * Local addresses are dynamically allocated on-demand.
 * We do not dynamically assign addresses from the low 1024 range,
 * in order to reserve that address range for predefined services.
 */
#define RP_MSG_RESERVED_ADDRESSES	(1024)

/* Reserve address 500 for rpmsg devices creation service */
#define RPMSG_FACTORY_ADDR		(500)

/* show configuration fields */
#define rpmsg_show_attr(field, path, format_string)			\
static ssize_t								\
field##_show(struct device *dev,					\
			struct device_attribute *attr, char *buf)	\
{									\
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);		\
									\
	return sprintf(buf, format_string, rpdev->path);		\
}

rpmsg_show_attr(name, id.name, "%s\n");
rpmsg_show_attr(dst, dst, "0x%x\n");
rpmsg_show_attr(src, src, "0x%x\n");

/* unique (free running) numbering for rpmsg devices */
static unsigned int rpmsg_dev_index;

static ssize_t modalias_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);

	return sprintf(buf, RPMSG_DEVICE_MODALIAS_FMT "\n", rpdev->id.name);
}

static struct device_attribute rpmsg_dev_attrs[] = {
	__ATTR_RO(name),
	__ATTR_RO(modalias),
	__ATTR_RO(dst),
	__ATTR_RO(src),
	__ATTR_NULL
};

static inline int rpmsg_id_match(const struct rpmsg_channel *rpdev,
				  const struct rpmsg_device_id *id)
{
	if (strncmp(id->name, rpdev->id.name, RPMSG_NAME_SIZE))
		return 0;

	return 1;
}

static int rpmsg_dev_match(struct device *dev, struct device_driver *drv)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(drv);
	const struct rpmsg_device_id *ids = rpdrv->id_table;
	unsigned int i;

	for (i = 0; ids[i].name[0]; i++) {
		if (rpmsg_id_match(rpdev, &ids[i]))
			return 1;
	}

	return 0;
}

static int rpmsg_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);

	return add_uevent_var(env, "MODALIAS=" RPMSG_DEVICE_MODALIAS_FMT,
					rpdev->id.name);
}

/* assign a new local address, and bind it to the user's callback function */
struct rpmsg_endpoint *rpmsg_create_ept(struct rpmsg_channel *rpdev,
		void (*cb)(struct rpmsg_channel *, void *, int, void *, u32),
		void *priv, u32 addr)
{
	int err, tmpaddr, request;
	struct rpmsg_endpoint *ept;
	struct virtproc_info *vrp = rpdev->vrp;

	if (!idr_pre_get(&vrp->endpoints, GFP_KERNEL))
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

	spin_lock(&vrp->endpoints_lock);

	/* dynamically assign a new address outside the reseved range */
	err = idr_get_new_above(&vrp->endpoints, ept, request, &tmpaddr);
	if (err) {
		dev_err(&rpdev->dev, "idr_get_new_above failed: %d\n", err);
		goto free_ept;
	}

	if (addr != RPMSG_ADDR_ANY && tmpaddr != addr) {
		dev_err(&rpdev->dev, "address 0x%x already in use\n", addr);
		goto rem_idr;
	}

	ept->addr = tmpaddr;

	spin_unlock(&vrp->endpoints_lock);

	return ept;

rem_idr:
	idr_remove(&vrp->endpoints, request);
free_ept:
	spin_unlock(&vrp->endpoints_lock);
	kfree(ept);
	return NULL;
}
EXPORT_SYMBOL_GPL(rpmsg_create_ept);

void rpmsg_destroy_ept(struct rpmsg_endpoint *ept)
{
	struct virtproc_info *vrp = ept->rpdev->vrp;

	spin_lock(&vrp->endpoints_lock);
	idr_remove(&vrp->endpoints, ept->addr);
	spin_unlock(&vrp->endpoints_lock);

	kfree(ept);
}
EXPORT_SYMBOL_GPL(rpmsg_destroy_ept);

static int rpmsg_dev_probe(struct device *dev)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(rpdev->dev.driver);
	struct rpmsg_endpoint *ept;
	int err;

	ept = rpmsg_create_ept(rpdev, rpdrv->callback, NULL, rpdev->src);
	if (!ept) {
		dev_err(dev, "failed to create endpoint\n");
		err = -ENOMEM;
		goto out;
	}

	rpdev->ept = ept;
	rpdev->src = ept->addr;

	err = rpdrv->probe(rpdev);
	if (err) {
		dev_err(dev, "%s: failed: %d\n", __func__, err);
		rpmsg_destroy_ept(ept);
	}

out:
	return err;
}

static int rpmsg_dev_remove(struct device *dev)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);
	struct rpmsg_driver *rpdrv = to_rpmsg_driver(rpdev->dev.driver);

	rpdrv->remove(rpdev);

	rpmsg_destroy_ept(rpdev->ept);

	return 0;
}

static struct bus_type rpmsg_bus = {
	.name		= "rpmsg",
	.match		= rpmsg_dev_match,
	.dev_attrs	= rpmsg_dev_attrs,
	.uevent		= rpmsg_uevent,
	.probe		= rpmsg_dev_probe,
	.remove		= rpmsg_dev_remove,
};

int register_rpmsg_driver(struct rpmsg_driver *rpdrv)
{
	rpdrv->drv.bus = &rpmsg_bus;
	return driver_register(&rpdrv->drv);
}
EXPORT_SYMBOL_GPL(register_rpmsg_driver);

void unregister_rpmsg_driver(struct rpmsg_driver *rpdrv)
{
	driver_unregister(&rpdrv->drv);
}
EXPORT_SYMBOL_GPL(unregister_rpmsg_driver);

static void rpmsg_release_device(struct device *dev)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);

	kfree(rpdev);
}

struct rpmsg_channel *rpmsg_create_channel(struct virtproc_info *vrp,
				char *name, u32 src, u32 dst)
{
	struct rpmsg_channel *rpdev;
	int ret;

	rpdev = kzalloc(sizeof(struct rpmsg_channel), GFP_KERNEL);
	if (!rpdev) {
		pr_err("kzalloc failed\n");
		return NULL;
	}

	rpdev->vrp = vrp;
	rpdev->src = src;
	rpdev->dst = dst;
	strncpy(rpdev->id.name, name, RPMSG_NAME_SIZE);

	dev_set_name(&rpdev->dev, "rpmsg%d", rpmsg_dev_index++);

	rpdev->dev.parent = &vrp->vdev->dev;
	rpdev->dev.bus = &rpmsg_bus;
	rpdev->dev.release = rpmsg_release_device;

	ret = device_register(&rpdev->dev);
	if (ret) {
		pr_err("failed to register dev rpmsg:%s\n", name);
		kfree(rpdev);
		return NULL;
	}

	return rpdev;
}
EXPORT_SYMBOL_GPL(rpmsg_create_channel);

void rpmsg_destroy_channel(struct rpmsg_channel *rpdev)
{
	device_unregister(&rpdev->dev);
}
EXPORT_SYMBOL_GPL(rpmsg_destroy_channel);

/* minimal buf "allocator" that is just enough for now */
static void *get_a_buf(struct virtproc_info *vrp)
{
	unsigned int len;

	/* either pick the next unused buffer */
	if (vrp->last_sbuf < vrp->num_bufs / 2)
		return vrp->sbufs + vrp->buf_size * vrp->last_sbuf++;
	/* or recycle a used one */
	else
		return virtqueue_get_buf(vrp->svq, &len);
}

int rpmsg_send_offchannel(struct rpmsg_channel *rpdev, u32 src, u32 dst,
					void *data, int len)
{
	struct virtproc_info *vrp = rpdev->vrp;
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
	if (len > vrp->buf_size - sizeof(struct rpmsg_hdr)) {
		dev_err(&rpdev->dev, "message is too big (%d)\n", len);
		return -EMSGSIZE;
	}

	/* grab a buffer. todo: add blocking support in case no buf is free */
	msg = get_a_buf(vrp);
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

	offset = ((unsigned long) msg) - ((unsigned long) vrp->rbufs);
	sim_addr = vrp->sim_base + offset;
	sg_init_one(&sg, sim_addr, sizeof(*msg) + len);

	/* protect svq from simultaneous concurrent manipulations */
	spin_lock(&vrp->svq_lock);

	/* add message to the remote processor's virtqueue */
	err = virtqueue_add_buf_gfp(vrp->svq, &sg, 1, 0, msg, GFP_KERNEL);
	if (err < 0) {
		pr_err("failed to add a virtqueue buffer: %d\n", err);
		goto out;
	}

	/* tell the remote processor it has a pending message to read */
	virtqueue_kick(vrp->svq);

	err = 0;
out:
	spin_unlock(&vrp->svq_lock);
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
	struct virtproc_info *vrp = rvq->vdev->priv;
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
	spin_lock(&vrp->endpoints_lock);
	ept = idr_find(&vrp->endpoints, msg->dst);
	spin_unlock(&vrp->endpoints_lock);

	if (ept && ept->cb)
		ept->cb(ept->rpdev, msg->data, msg->len, ept->priv, msg->src);
	else
		pr_warn("msg received with no recepient\n");

	/* add the buffer back to the remote processor's virtqueue */
	offset = ((unsigned long) msg) - ((unsigned long) vrp->rbufs);
	sim_addr = vrp->sim_base + offset;
	sg_init_one(&sg, sim_addr, sizeof(*msg) + len);

	err = virtqueue_add_buf_gfp(vrp->rvq, &sg, 0, 1, msg, GFP_KERNEL);
	if (err < 0) {
		pr_err("failed to add a virtqueue buffer: %d\n", err);
		return;
	}

	/* tell the remote processor we added another available rx buffer */
	virtqueue_kick(vrp->rvq);
}

static void rpmsg_xmit_done(struct virtqueue *svq)
{
	pr_warn("BIOS did not obey virtqueue_disable_cb(vrp->svq)\n");
}

static int rpmsg_probe(struct virtio_device *vdev)
{
	vq_callback_t *callbacks[] = { rpmsg_recv_done, rpmsg_xmit_done };
	const char *names[] = { "input", "output" };
	struct virtqueue *vqs[2];
	struct virtproc_info *vrp;
	void *addr;
	int err, i, num_bufs, buf_size, total_buf_size;
	struct rpmsg_channel_hdr *ch;

	vrp = kzalloc(sizeof(*vrp), GFP_KERNEL);
	if (!vrp)
		return -ENOMEM;

	vrp->vdev = vdev;

	idr_init(&vrp->endpoints);
	spin_lock_init(&vrp->endpoints_lock);
	spin_lock_init(&vrp->svq_lock);

	/* We expect two virtqueues, receive then send */
	err = vdev->config->find_vqs(vdev, 2, vqs, callbacks, names);
	if (err)
		goto free_vi;

	vrp->rvq = vqs[0];
	vrp->svq = vqs[1];

	/* Platform must supply pre-allocated uncached buffers for now */
	vdev->config->get(vdev, VPROC_BUF_ADDR, &addr, sizeof(addr));
	vdev->config->get(vdev, VPROC_BUF_NUM, &num_bufs,
							sizeof(num_bufs));
	vdev->config->get(vdev, VPROC_BUF_SZ, &buf_size, sizeof(buf_size));

	total_buf_size = num_bufs * buf_size;

	dev_dbg(&vdev->dev, "%d buffers, size %d, addr 0x%x, total 0x%x\n",
		num_bufs, buf_size, (unsigned int) addr, total_buf_size);

	vrp->num_bufs = num_bufs;
	vrp->buf_size = buf_size;
	vrp->rbufs = addr;
	vrp->sbufs = addr + total_buf_size / 2;

	/* simulated addr base to make virt_to_page happy. consider using
	 * virtio features for that */
	vdev->config->get(vdev, VPROC_SIM_BASE, &vrp->sim_base,
							sizeof(vrp->sim_base));

	/* set up the receive buffers */
	/* note: those RP_MSG_* macros should be retrieved from the platform
	 * and not compiled into the driver... */
	for (i = 0; i < num_bufs/2; i++) {
		struct scatterlist sg;
		void *tmpaddr = vrp->rbufs + i * buf_size;
		void *simaddr = vrp->sim_base + i * buf_size;

		sg_init_one(&sg, simaddr, buf_size);
		err = virtqueue_add_buf_gfp(vrp->rvq, &sg, 0, 1, tmpaddr,
								GFP_KERNEL);
		WARN_ON(err < 0); /* sanity check; this can't happen */
	}

	/* tell the remote processor it can start sending data */
	virtqueue_kick(vrp->rvq);

	/* suppress "tx-complete" interrupts */
	virtqueue_disable_cb(vrp->svq);

	vdev->priv = vrp;

	dev_info(&vdev->dev, "rpmsg backend virtproc probed successfully\n");

	/* look for platform-specific hardcoded channels */
	vdev->config->get(vdev, VPROC_HC_CHANNELS, &ch, sizeof(ch));

	for (i = 0; ch && ch[i].name[0]; i++)
		rpmsg_create_channel(vrp, ch[i].name, ch[i].src, ch[i].dst);

	return 0;

free_vi:
	kfree(vrp);
	return err;
}

static int rpmsg_remove_device(struct device *dev, void *data)
{
	struct rpmsg_channel *rpdev = to_rpmsg_channel(dev);

	rpmsg_destroy_channel(rpdev);

	return 0;
}

static void __devexit rpmsg_remove(struct virtio_device *vdev)
{
	struct virtproc_info *vrp = vdev->priv;
	int ret;

	ret = device_for_each_child(&vdev->dev, NULL, rpmsg_remove_device);
	if (ret)
		dev_warn(&vdev->dev, "can't remove rpmsg device: %d\n", ret);

	vdev->config->del_vqs(vrp->vdev);

	idr_remove_all(&vrp->endpoints);
	idr_destroy(&vrp->endpoints);
	kfree(vrp);
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

static int __init init(void)
{
	int ret;

	ret = bus_register(&rpmsg_bus);
	if (ret) {
		pr_err("failed to register rpmsg bus: %d\n", ret);
		return ret;
	}

	return register_virtio_driver(&virtio_ipc_driver);
}
module_init(init);

static void __exit fini(void)
{
	unregister_virtio_driver(&virtio_ipc_driver);
	bus_unregister(&rpmsg_bus);
}
module_exit(fini);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio-based remote processor messaging bus");
MODULE_LICENSE("GPL v2");
