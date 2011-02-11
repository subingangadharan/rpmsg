/*
 * OMAP Remote Processor control driver
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * Mark Grosen <mgrosen@ti.com>
 * Fernando Guzman Lugo <fernando.lugo@ti.com>
 * Armando Uribe De Leon <x0095078@ti.com>
 * Robert Tivy <rtivy@ti.com>
 * Ohad Ben-Cohen <ohad@wizery.com>
 * Brian Swetland <swetland@google.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
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

#include <plat/remoteproc.h>

#define OMAP_RPROC_DEBUGFS_BUF_SIZE	(512)

/* list of available remote processors on this board */
static LIST_HEAD(rprocs);
static DEFINE_SPINLOCK(rprocs_lock);

/* debugfs parent dir */
static struct dentry *omap_rproc_dbg;

static int omap_rproc_format_buf(char __user *userbuf, size_t count,
				    loff_t *ppos, const void *src, int size)
{
	char buf[OMAP_RPROC_DEBUGFS_BUF_SIZE];
	int len;

	len = min(size, OMAP_RPROC_DEBUGFS_BUF_SIZE - 1);

	memcpy(buf, src, len);

	buf[len] = '\n';

	return simple_read_from_buffer(userbuf, count, ppos, buf, len + 1);
}

static int omap_rproc_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

#define DEBUGFS_READONLY_FILE(name, value, len)				\
static ssize_t name## _omap_rproc_read(struct file *file,		\
		char __user *userbuf, size_t count, loff_t *ppos)	\
{									\
	struct omap_rproc *rproc = file->private_data;			\
	return omap_rproc_format_buf(userbuf, count, ppos, value, len);	\
}									\
									\
static const struct file_operations name ##_omap_rproc_ops = {		\
	.read = name ##_omap_rproc_read,				\
	.open = omap_rproc_open,					\
	.llseek	= generic_file_llseek,					\
};

#define DEBUGFS_ADD(name)						\
	debugfs_create_file(#name, 0400, rproc->dbg_dir,		\
			rproc, &name## _omap_rproc_ops)

DEBUGFS_READONLY_FILE(trace0, rproc->trace_buf0, rproc->trace_len0);
DEBUGFS_READONLY_FILE(trace1, rproc->trace_buf1, rproc->trace_len1);
DEBUGFS_READONLY_FILE(name, rproc->name, strlen(rproc->name));

static struct omap_rproc *omap_find_rproc_by_name(const char *name)
{
	struct omap_rproc *rproc;
	struct list_head *tmp;

	spin_lock(&rprocs_lock);

	list_for_each(tmp, &rprocs) {
		rproc = list_entry(tmp, struct omap_rproc, next);
		if (!strcmp(rproc->name, name))
			break;
		rproc = NULL;
	}

	spin_unlock(&rprocs_lock);

	return rproc;
}

/* Convert a device (virtual) address to a physical address */
static u32 omap_rproc_da_to_pa(const struct rproc_mem_entry *maps, u32 va)
{
	int i;
	u32 offset;

	for (i = 0; maps[i].size; i++) {
		const struct rproc_mem_entry *me = &maps[i];

		if (va >= me->da && va < (me->da + me->size)) {
			offset = va - me->da;
			return me->pa + offset;
		}
	}

	return 0;
}

static void omap_rproc_start(struct omap_rproc *rproc)
{
	struct omap_rproc_platform_data *pdata = rproc->dev->platform_data;
	struct device *dev = rproc->dev;
	int err;

	err = mutex_lock_interruptible(&rproc->lock);
	if (err) {
		dev_err(dev, "can't lock remote processor %d\n", err);
		return;
	}

	err = pdata->ops->start(rproc->dev, 0);
	if (err) {
		dev_err(dev, "can't start rproc %s: %d\n", rproc->name, err);
		goto unlock_mutext;
	}

	rproc->state = OMAP_RPROC_RUNNING;

	dev_info(dev, "started remote processor %s\n", rproc->name);

unlock_mutext:
	mutex_unlock(&rproc->lock);
}

static void
omap_rproc_handle_resources(struct omap_rproc *rproc, void *data, int len)
{
	struct omap_fw_resource *rsc = data;
	struct device *dev = rproc->dev;
	struct omap_rproc_platform_data *pdata = dev->platform_data;
	u32 pa, offset, base;
	void *ptr;

	while (len > sizeof(*rsc)) {
		pa = omap_rproc_da_to_pa(pdata->memory_maps, rsc->da);

		dev_dbg(dev, "resource: type %d, da 0x%x, pa 0x%x, len %d"
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
		default:
			/* we don't support much right now. so use dbg lvl */
			dev_dbg(dev, "unsupported resource type %d\n",
							rsc->type);
			break;
		}

		rsc++;
		len -= sizeof(*rsc);
	}
}

static void omap_rproc_loader_cont(const struct firmware *fw, void *context)
{
	struct omap_rproc *rproc = context;
	struct device *dev = rproc->dev;
	struct omap_rproc_platform_data *pdata = dev->platform_data;
	const char *fwfile = pdata->firmware;
	u32 left;
	struct omap_fw_format *image;
	struct omap_fw_section *section;
	int ret;

	if (!fw) {
		dev_err(dev, "%s: failed to load %s\n", __func__, fwfile);
		goto complete_fw;
	}

	dev_info(dev, "Loaded BIOS image %s, size %d\n", fwfile, fw->size);

	/* make sure this image is sane */
	if (fw->size < sizeof(struct omap_fw_format)) {
		dev_err(dev, "Image is too small\n");
		goto out;
	}

	image = (struct omap_fw_format *)fw->data;

	if (memcmp(image->magic, "TIFW", 4)) {
		dev_err(dev, "Image is corrupted (no magic)\n");
		goto out;
	}

	dev_info(dev, "BIOS image version is %d\n", image->version);


	/* now process the image, section by section */
	section = (struct omap_fw_section *)(image->header + image->header_len);

	left = fw->size - sizeof(struct omap_fw_format) - image->header_len;

	while (left > sizeof(struct omap_fw_section)) {
		u32 offset, base, da, pa, len, type;
		void *ptr;

		da = section->da;
		len = section->len;
		type = section->type;
		dev_dbg(dev, "section: type %d da 0x%x len 0x%x\n",
						       type, da, len);

		left -= sizeof(struct omap_fw_section);
		if (left < section->len) {
			dev_err(dev, "BIOS image is truncated\n");
			ret = -EINVAL;
			goto out;
		}

		pa = omap_rproc_da_to_pa(pdata->memory_maps, da);
		if (!pa) {
			dev_err(dev, "invalid da (0x%x) in %s\n", da, fwfile);
			ret = -EINVAL;
			goto out;
		}

		offset = pa & 0xFFF;
		base = pa & 0xFFFFF000;
		dev_dbg(dev, "da 0x%x pa 0x%x len 0x%x\n", da, pa, len);

		ptr = ioremap(base, __ALIGN_MASK(offset + len, 0xFFF));
		if (!ptr) {
			dev_err(dev, "can't ioremap 0x%x (%s)\n", base, fwfile);
			ret = -ENOMEM;
			goto out;
		}

		memcpy(ptr + offset, section->content, len);

		iounmap(ptr);

		/* a resource table needs special handling */
		if (section->type == FW_RESOURCE)
			omap_rproc_handle_resources(rproc, ptr + offset, len);

		section = (struct omap_fw_section *)(section->content + len);
		left -= len;
	}

	omap_rproc_start(rproc);

out:
	release_firmware(fw);
complete_fw:
	/* allow all contexts calling omap_rproc_put() to proceed */
	complete_all(&rproc->firmware_loading_complete);
}

static int omap_rproc_loader(struct omap_rproc *rproc)
{
	struct omap_rproc_platform_data *pdata = rproc->dev->platform_data;
	const char *fwfile = pdata->firmware;
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
			dev, GFP_KERNEL, rproc, omap_rproc_loader_cont);
	if (ret < 0) {
		dev_err(dev, "request_firmware_nowait failed: %d\n", ret);
		return ret;
	}

	return 0;
}

struct omap_rproc *omap_rproc_get(const char *name)
{
	struct omap_rproc_platform_data *pdata;
	struct omap_rproc *rproc, *ret = NULL;
	struct device *dev;
	int err;

	rproc = omap_find_rproc_by_name(name);
	if (!rproc) {
		pr_err("can't find remote processor %s\n", name);
		return NULL;
	}

	dev = rproc->dev;
	pdata = rproc->dev->platform_data;

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

	/* omap_rproc_put() calls should wait until async loader completes */
	init_completion(&rproc->firmware_loading_complete);

	err = omap_rproc_loader(rproc);
	if (err) {
		dev_err(dev, "failed to load rproc %s\n", rproc->name);
		complete_all(&rproc->firmware_loading_complete);
		rproc->count--;
		goto unlock_mutext;
	}

	rproc->state = OMAP_RPROC_LOADING;
	ret = rproc;

unlock_mutext:
	mutex_unlock(&rproc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(omap_rproc_get);

void omap_rproc_put(struct omap_rproc *rproc)
{
	struct omap_rproc_platform_data *pdata = rproc->dev->platform_data;
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
	if (rproc->state == OMAP_RPROC_RUNNING) {
		ret = pdata->ops->stop(rproc->dev);
		if (ret) {
			dev_err(dev, "can't stop rproc %s: %d\n", rproc->name,
									ret);
			goto out;
		}
	}

	rproc->state = OMAP_RPROC_OFFLINE;

	dev_info(dev, "stopped remote processor %s\n", rproc->name);

out:
	mutex_unlock(&rproc->lock);

}
EXPORT_SYMBOL_GPL(omap_rproc_put);

static int omap_rproc_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct omap_rproc_platform_data *pdata = dev->platform_data;
	struct omap_rproc *rproc;

	if (!pdata || !pdata->name || !pdata->ops)
		return -EINVAL;

	rproc = kzalloc(sizeof(struct omap_rproc), GFP_KERNEL);
	if (!rproc) {
		dev_err(dev, "%s: kzalloc failed\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	platform_set_drvdata(pdev, rproc);

	rproc->dev = dev;
	rproc->name = pdata->name;

	mutex_init(&rproc->lock);

	rproc->state = OMAP_RPROC_OFFLINE;

	spin_lock(&rprocs_lock);
	list_add_tail(&rproc->next, &rprocs);
	spin_unlock(&rprocs_lock);

	dev_info(dev, "%s is available\n", pdata->name);

	if (!omap_rproc_dbg)
		goto out;

	rproc->dbg_dir = debugfs_create_dir(dev_name(&pdev->dev),
							omap_rproc_dbg);
	if (!rproc->dbg_dir)
		dev_err(&pdev->dev, "can't create debugfs dir\n");

	DEBUGFS_ADD(name);

	return 0;

out:
	return ret;
}

static int __devexit omap_rproc_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct omap_rproc_platform_data *pdata = dev->platform_data;
	struct omap_rproc *rproc = platform_get_drvdata(pdev);

	dev_info(dev, "removing %s\n", pdata->name);

	if (rproc->dbg_dir)
		debugfs_remove_recursive(rproc->dbg_dir);

	spin_lock(&rprocs_lock);
	list_del(&rproc->next);
	spin_unlock(&rprocs_lock);

	kfree(rproc);

	return 0;
}

static struct platform_driver omap_rproc_driver = {
	.probe = omap_rproc_probe,
	.remove = __devexit_p(omap_rproc_remove),
	.driver = {
		.name = "omap-rproc",
		.owner = THIS_MODULE,
	},
};

static int __init omap_rproc_init(void)
{
	if (debugfs_initialized()) {
		omap_rproc_dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
		if (!omap_rproc_dbg)
			pr_err("can't create debugfs dir\n");
	}

	return platform_driver_register(&omap_rproc_driver);
}
module_init(omap_rproc_init);

static void __exit omap_rproc_exit(void)
{
	platform_driver_unregister(&omap_rproc_driver);

	if (omap_rproc_dbg)
		debugfs_remove(omap_rproc_dbg);
}
module_exit(omap_rproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OMAP Remote Processor control driver");
