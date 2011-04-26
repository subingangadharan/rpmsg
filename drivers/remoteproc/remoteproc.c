/*
 * Remote Processor Framework
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Mark Grosen <mgrosen@ti.com>
 * Brian Swetland <swetland@google.com>
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 * Robert Tivy <rtivy@ti.com>
 * Armando Uribe De Leon <x0095078@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/remoteproc.h>

/* list of available remote processors on this board */
static LIST_HEAD(rprocs);
static DEFINE_SPINLOCK(rprocs_lock);

/* debugfs parent dir */
static struct dentry *rproc_dbg;

static ssize_t rproc_format_trace_buf(char __user *userbuf, size_t count,
				    loff_t *ppos, const void *src, int size)
{
        const char *buf = (const char *) src;
        int i;

        /* find the end of trace buffer - does not account for wrapping */
        for (i = 0; i < size && buf[i]; i++);

        return simple_read_from_buffer(userbuf, count, ppos, src, i);
}

static ssize_t rproc_name_read(struct file *filp, char __user *userbuf,
						size_t count, loff_t *ppos)
{
	struct rproc *rproc = filp->private_data;
	char buf[RPROC_MAX_NAME + 1];
	int i;

	i = sprintf(buf, "%s\n", rproc->name);

	return simple_read_from_buffer(userbuf, count, ppos, buf, i);
}

static int rproc_open_generic(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

#define DEBUGFS_READONLY_FILE(name, value, len)				\
static ssize_t name## _rproc_read(struct file *filp,			\
		char __user *userbuf, size_t count, loff_t *ppos)	\
{									\
	struct rproc *rproc = filp->private_data;			\
	return rproc_format_trace_buf(userbuf, count, ppos, value, len);\
}									\
									\
static const struct file_operations name ##_rproc_ops = {		\
	.read = name ##_rproc_read,					\
	.open = rproc_open_generic,					\
	.llseek	= generic_file_llseek,					\
};

static const struct file_operations rproc_name_ops = {
	.read = rproc_name_read,
	.open = rproc_open_generic,
	.llseek	= generic_file_llseek,
};

DEBUGFS_READONLY_FILE(trace0, rproc->trace_buf0, rproc->trace_len0);
DEBUGFS_READONLY_FILE(trace1, rproc->trace_buf1, rproc->trace_len1);

#define DEBUGFS_ADD(name)						\
	debugfs_create_file(#name, 0400, rproc->dbg_dir,		\
			rproc, &name## _rproc_ops)

/**
 * __find_rproc_by_name - find a registered remote processor by name
 * @name: name of the remote processor
 *
 * Internal function that returns the rproc @name, or NULL if @name does
 * not exists.
 */
static struct rproc *__find_rproc_by_name(const char *name)
{
	struct rproc *rproc;
	struct list_head *tmp;

	spin_lock(&rprocs_lock);

	list_for_each(tmp, &rprocs) {
		rproc = list_entry(tmp, struct rproc, next);
		if (!strcmp(rproc->name, name))
			break;
		rproc = NULL;
	}

	spin_unlock(&rprocs_lock);

	return rproc;
}

/**
 * __rproc_da_to_pa - convert a device (virtual) address to its physical address
 * @maps: the remote processor's memory mappings array
 * @da: a device address (as seen by the remote processor)
 *
 * This function converts @da to its physical address (pa) by going through
 * @maps, looking for a mapping that contains @da, and then calculating the
 * appropriate pa.
 *
 * Returns the appropriate pa, or 0 if @da isn't mapped yet to the
 * remote processor's view.
 * Note: address 0 is not mappable on ARM, and therefore can be used as
 * an error value here. if support is added beyond
 * todo: use ERR_PTR
 */
static u32 rproc_da_to_pa(const struct rproc_mem_entry *maps, u32 da)
{
	int i;
	u32 offset;

	for (i = 0; maps[i].size; i++) {
		const struct rproc_mem_entry *me = &maps[i];

		if (da >= me->da && da < (me->da + me->size)) {
			offset = da - me->da;
			pr_debug("%s: matched mem entry no. %d\n", __func__, i);
			return me->pa + offset;
		}
	}

	return 0;
}

static void rproc_start(struct rproc *rproc, u64 bootaddr)
{
	struct device *dev = rproc->dev;
	int err;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %d\n", err);
		return;
	}

	err = rproc->ops->start(rproc, bootaddr);
	if (err) {
		dev_err(dev, "can't start rproc %s: %d\n", rproc->name, err);
		goto unlock_mutext;
	}

	rproc->state = RPROC_RUNNING;

	dev_info(dev, "remote processor %s is now up\n", rproc->name);

unlock_mutext:
	mutex_unlock(&rproc->lock);
}

static u64
rproc_handle_resources(struct rproc *rproc, struct fw_resource *rsc, int len)
{
	struct device *dev = rproc->dev;
	u32 pa, offset, base;
	u64 da, bootaddr = 0;
	void *ptr;

	while (len >= sizeof(*rsc)) {
		da = rsc->da;
		if (da > UINT_MAX)
			dev_warn(dev, "too big a da !\n");

		pa = rproc_da_to_pa(rproc->memory_maps, da);
		if (!pa)
			dev_dbg(dev, "zero pa ! change to PTR_ERR\n");

		dev_dbg(dev, "resource: type %d, da 0x%llx, pa 0x%x, len 0x%x"
			", reserved %d, name %s\n", rsc->type, rsc->da, pa,
			rsc->len, rsc->reserved, rsc->name);

		if (rsc->reserved)
			dev_warn(dev, "rsc %s: nonzero reserved\n", rsc->name);

		switch (rsc->type) {
		case RSC_TRACE:
			if (rproc->trace_buf0 && rproc->trace_buf1) {
				dev_warn(dev, "skipping extra trace rsc %s\n",
						rsc->name);
				break;
			}
			offset = pa & 0xFFF;
			base = pa & 0xFFFFF000;

			ptr = ioremap_nocache(base,
					__ALIGN_MASK(offset + rsc->len, 0xFFF));
			if (!ptr)
				dev_err(dev, "can't ioremap trace buffer %s\n",
								rsc->name);

			if (!rproc->trace_buf0) {
				rproc->trace_len0 = rsc->len;
				rproc->trace_buf0 = ptr;
				DEBUGFS_ADD(trace0);
			} else {
				rproc->trace_len1 = rsc->len;
				rproc->trace_buf1 = ptr;
				DEBUGFS_ADD(trace1);
			}
			break;
		case RSC_BOOTADDR:
			bootaddr = da;
			break;
		default:
			/* we don't support much right now. so use dbg lvl */
			dev_dbg(dev, "unsupported resource type %d\n",
							rsc->type);
			break;
		}

		rsc++;
		len -= sizeof(*rsc);
	}

	return bootaddr;
}

static void rproc_loader_cont(const struct firmware *fw, void *context)
{
	struct rproc *rproc = context;
	struct device *dev = rproc->dev;
	const char *fwfile = rproc->firmware;
	u32 left;
	u64 bootaddr = 0;
	struct fw_header *image;
	struct fw_section *section;
	int ret;

	if (!fw) {
		dev_err(dev, "%s: failed to load %s\n", __func__, fwfile);
		goto complete_fw;
	}

	dev_info(dev, "Loaded BIOS image %s, size %d\n", fwfile, fw->size);

	/* make sure this image is sane */
	if (fw->size < sizeof(struct fw_header)) {
		dev_err(dev, "Image is too small\n");
		goto out;
	}

	image = (struct fw_header *) fw->data;

	if (memcmp(image->magic, "RPRC", 4)) {
		dev_err(dev, "Image is corrupted (bad magic)\n");
		goto out;
	}

	dev_info(dev, "BIOS image version is %d\n", image->version);

	/* now process the image, section by section */
	section = (struct fw_section *)(image->header + image->header_len);

	left = fw->size - sizeof(struct fw_header) - image->header_len;

	while (left > sizeof(struct fw_section)) {
		u32 pa, len, type;
		u64 da;
		void *ptr;

		da = section->da;
		len = section->len;
		type = section->type;

		dev_dbg(dev, "section: type %d da 0x%llx len 0x%x\n",
								type, da, len);

		if (da > UINT_MAX)
			dev_warn(dev, "too big a da !\n");

		left -= sizeof(struct fw_section);
		if (left < section->len) {
			dev_err(dev, "BIOS image is truncated\n");
			ret = -EINVAL;
			goto out;
		}

		pa = rproc_da_to_pa(rproc->memory_maps, da);
		if (!pa) {
			dev_err(dev, "invalid da (0x%llx) in %s\n", da, fwfile);
			ret = -EINVAL;
			goto out;
		}

		dev_dbg(dev, "da 0x%llx pa 0x%x len 0x%x\n", da, pa, len);

		ptr = ioremap(pa, len);
		if (!ptr) {
			dev_err(dev, "can't ioremap 0x%x (%s)\n", pa, fwfile);
			ret = -ENOMEM;
			goto out;
		}

		memcpy(ptr, section->content, len);

		/* a resource table needs special handling */
		if (section->type == FW_RESOURCE)
			bootaddr = rproc_handle_resources(rproc,
					(struct fw_resource *) ptr, len);

		iounmap(ptr);

		section = (struct fw_section *)(section->content + len);
		left -= len;
	}

	rproc_start(rproc, bootaddr);

out:
	release_firmware(fw);
complete_fw:
	/* allow all contexts calling rproc_put() to proceed */
	complete_all(&rproc->firmware_loading_complete);
}

static int rproc_loader(struct rproc *rproc)
{
	const char *fwfile = rproc->firmware;
	struct device *dev = rproc->dev;
	int ret;

	if (!fwfile) {
		dev_err(dev, "%s: no firmware to load\n", __func__);
		return -EINVAL;
	}

	/*
	 * allow building remoteproc as built-in kernel code, without
	 * hanging the boot process
	 */
	ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_HOTPLUG, fwfile,
			dev, GFP_KERNEL, rproc, rproc_loader_cont);
	if (ret < 0) {
		dev_err(dev, "request_firmware_nowait failed: %d\n", ret);
		return ret;
	}

	return 0;
}

struct rproc *rproc_get(const char *name)
{
	struct rproc *rproc, *ret = NULL;
	struct device *dev;
	int err;

	rproc = __find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return NULL;
	}

	dev = rproc->dev;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %s\n", name);
		return NULL;
	}

	/* if the remote proc is loading or already powered up, bail out */
	if (rproc->count++) {
		dev_info(dev, "%s is already (being) powered up\n", name);
		ret = rproc;
		goto unlock_mutext;
	}

	/* rproc_put() calls should wait until async loader completes */
	init_completion(&rproc->firmware_loading_complete);

	dev_info(dev, "powering up %s\n", name);

	err = rproc_loader(rproc);
	if (err) {
		dev_err(dev, "failed to load rproc %s\n", rproc->name);
		complete_all(&rproc->firmware_loading_complete);
		rproc->count--;
		goto unlock_mutext;
	}

	rproc->state = RPROC_LOADING;
	ret = rproc;

unlock_mutext:
	mutex_unlock(&rproc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_get);

void rproc_put(struct rproc *rproc)
{
	struct device *dev = rproc->dev;
	int ret;

	/* make sure rproc is not loading now */
	wait_for_completion(&rproc->firmware_loading_complete);

	ret = mutex_lock_interruptible(&rproc->lock);
	if (ret) {
		dev_err(dev, "can't lock rproc %s: %d\n", rproc->name, ret);
		return;
	}

	/* if the remote proc is still needed, bail out */
	if (--rproc->count)
		goto out;

	if (rproc->trace_buf0)
		iounmap(rproc->trace_buf0);
	if (rproc->trace_buf1)
		iounmap(rproc->trace_buf1);

	rproc->trace_buf0 = rproc->trace_buf1 = NULL;

	/*
	 * make sure rproc is really running before powering it off.
	 * this is important, because the fw loading might have failed.
	 */
	if (rproc->state == RPROC_RUNNING) {
		ret = rproc->ops->stop(rproc);
		if (ret) {
			dev_err(dev, "can't stop rproc %s: %d\n", rproc->name,
									ret);
			goto out;
		}
	}

	rproc->state = RPROC_OFFLINE;

	dev_info(dev, "stopped remote processor %s\n", rproc->name);

out:
	mutex_unlock(&rproc->lock);

}
EXPORT_SYMBOL_GPL(rproc_put);

int rproc_register(struct device *dev, const char *name,
				struct rproc_ops *ops, char *firmware,
				const struct rproc_mem_entry *memory_maps)
{
	struct rproc *rproc;

	if (!dev || !name || !ops)
		return -EINVAL;

	rproc = kzalloc(sizeof(struct rproc), GFP_KERNEL);
	if (!rproc) {
		dev_err(dev, "%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	rproc->dev = dev;
	rproc->name = name;
	rproc->ops = ops;
	rproc->firmware = firmware;
	rproc->memory_maps = memory_maps;

	mutex_init(&rproc->lock);

	rproc->state = RPROC_OFFLINE;

	spin_lock(&rprocs_lock);
	list_add_tail(&rproc->next, &rprocs);
	spin_unlock(&rprocs_lock);

	dev_info(dev, "%s is available\n", name);

	if (!rproc_dbg)
		goto out;

	rproc->dbg_dir = debugfs_create_dir(dev_name(dev), rproc_dbg);
	if (!rproc->dbg_dir) {
		dev_err(dev, "can't create debugfs dir\n");
		goto out;
	}

	debugfs_create_file("name", 0400, rproc->dbg_dir, rproc,
							&rproc_name_ops);

out:
	return 0;
}
EXPORT_SYMBOL_GPL(rproc_register);

int rproc_unregister(const char *name)
{
	struct rproc *rproc;

	rproc = __find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return -EINVAL;
	}

	dev_info(rproc->dev, "removing %s\n", name);

	if (rproc->dbg_dir)
		debugfs_remove_recursive(rproc->dbg_dir);

	//todo: take care of ref count
	spin_lock(&rprocs_lock);
	list_del(&rproc->next);
	spin_unlock(&rprocs_lock);

	kfree(rproc);

	return 0;
}
EXPORT_SYMBOL_GPL(rproc_unregister);

static int __init remoteproc_init(void)
{
	if (debugfs_initialized()) {
		rproc_dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
		if (!rproc_dbg)
			pr_err("can't create debugfs dir\n");
	}

	return 0;
}
module_init(remoteproc_init);

static void __exit remoteproc_exit(void)
{
	if (rproc_dbg)
		debugfs_remove(rproc_dbg);
}
module_exit(remoteproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Generic Remote Processor Framework");
