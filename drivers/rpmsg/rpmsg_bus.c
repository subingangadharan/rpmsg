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

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/rpmsg.h>

#include "rpmsg_internal.h"

#define to_rpmsg_channel(d) container_of(d, struct rpmsg_channel, dev)
#define to_rpmsg_driver(d) container_of(d, struct rpmsg_driver, drv)

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

/* unique numbering for rpmsg devices */
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

struct rpmsg_channel *rpmsg_create_channel(struct rpmsg_rproc *rp,
				char *name, u32 src, u32 dst)
{
	struct rpmsg_channel *rpdev;
	int ret;

	rpdev = kzalloc(sizeof(struct rpmsg_channel), GFP_KERNEL);
	if (!rpdev) {
		pr_err("kzalloc failed\n");
		return NULL;
	}

	rpdev->rp = rp;
	rpdev->src = src;
	rpdev->dst = dst;
	strncpy(rpdev->id.name, name, RPMSG_NAME_SIZE);

	dev_set_name(&rpdev->dev, "rpmsg%d", rpmsg_dev_index++);

	rpdev->dev.parent = &rp->vdev->dev;
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

int __init rpmsg_bus_init(void)
{
	int ret;

	ret = bus_register(&rpmsg_bus);
	if (ret)
		pr_err("failed to register rpmsg bus: %d\n", ret);

	return ret;
}
/* should initialize before device_initcall */
//subsys_initcall(init);

void __exit rpmsg_bus_fini(void)
{
	bus_unregister(&rpmsg_bus);
}
//module_exit(fini);

MODULE_DESCRIPTION("Remote processor messaging bus");
MODULE_LICENSE("GPL v2");
