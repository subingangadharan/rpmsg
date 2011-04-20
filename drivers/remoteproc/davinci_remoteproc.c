/*
 * Remote processor machine-specific module for Davinci
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

static inline int davinci_rproc_start(struct rproc *rproc, u32 start_addr)
{
	struct device *dev = rproc->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct rproc_platform_data *pdata = dev->platform_data;

	/* empty stub */

	return 0;
}

static inline int davinci_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev;
	struct platform_device *pdev = to_platform_device(dev);

	/* empty stub */

	return 0;
}

static struct rproc_ops davinci_rproc_ops = {
	.start = davinci_rproc_start,
	.stop = davinci_rproc_stop,
};

static int davinci_rproc_probe(struct platform_device *pdev)
{
	struct rproc_platform_data *pdata = pdev->dev.platform_data;

	return rproc_register(&pdev->dev, pdata->name, &davinci_rproc_ops,
				pdata->firmware, pdata->memory_maps);
}

static int __devexit davinci_rproc_remove(struct platform_device *pdev)
{
	struct rproc_platform_data *pdata = pdev->dev.platform_data;

	return rproc_unregister(pdata->name);
}

static struct platform_driver davinci_rproc_driver = {
	.probe = davinci_rproc_probe,
	.remove = __devexit_p(davinci_rproc_remove),
	.driver = {
		.name = "davinci-rproc",
		.owner = THIS_MODULE,
	},
};

static int __init davinci_rproc_init(void)
{
	return platform_driver_register(&davinci_rproc_driver);
}
module_init(davinci_rproc_init);

static void __exit davinci_rproc_exit(void)
{
	platform_driver_unregister(&davinci_rproc_driver);
}
module_exit(davinci_rproc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Davinci Remote Processor control driver");
