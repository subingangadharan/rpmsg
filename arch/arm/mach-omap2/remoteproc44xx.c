/*
 * Remote processor machine-specific module for OMAP4
 *
 * Copyright (C) 2011 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#define pr_fmt(fmt)    "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <plat/remoteproc.h>
#include <plat/dmtimer.h>
#include <plat/iommu.h>

#include <plat/omap_device.h>
#include <plat/omap_hwmod.h>

#define PAGE_SIZE_4KB                   0x1000
#define PAGE_SIZE_64KB                  0x10000
#define PAGE_SIZE_1MB                   0x100000
#define PAGE_SIZE_16MB                  0x1000000

/* Define the Peripheral PAs and their Ducati VAs. */
#define L4_PERIPHERAL_L4CFG             0x4A000000
#define DUCATI_PERIPHERAL_L4CFG         0xAA000000
#define TESLA_PERIPHERAL_L4CFG          0x4A000000

#define L4_PERIPHERAL_L4PER             0x48000000
#define DUCATI_PERIPHERAL_L4PER         0xA8000000
#define TESLA_PERIPHERAL_L4PER          0x48000000

#define L4_PERIPHERAL_L4EMU             0x54000000
#define DUCATI_PERIPHERAL_L4EMU         0xB4000000

#define L3_IVAHD_CONFIG                 0x5A000000
#define DUCATI_IVAHD_CONFIG             0xBA000000
#define TESLA_IVAHD_CONFIG              0xBA000000

#define L3_IVAHD_SL2                    0x5B000000
#define DUCATI_IVAHD_SL2                0xBB000000
#define TELSA_IVAHD_SL2                 0xBB000000

#define L3_TILER_MODE_0_1_ADDR          0x60000000
#define DUCATI_TILER_MODE_0_1_ADDR      0x60000000
#define DUCATI_TILER_MODE_0_1_LEN       0x10000000
#define TESLA_TILER_MODE_0_1_ADDR       0x60000000
#define TESLA_TILER_MODE_0_1_LEN        0x10000000

#define L3_TILER_MODE_2_ADDR            0x70000000
#define DUCATI_TILER_MODE_2_ADDR        0x70000000
#define TESLA_TILER_MODE_2_ADDR         0x70000000

#define L3_TILER_MODE_3_ADDR            0x78000000
#define DUCATI_TILER_MODE_3_ADDR        0x78000000
#define DUCATI_TILER_MODE_3_LEN         0x8000000
#define TESLA_TILER_MODE_3_ADDR         0x78000000
#define TESLA_TILER_MODE_3_LEN          0x8000000


#define DUCATI_MEM_CONST_SYSM3_ADDR     0x80000000
#define DUCATI_MEM_CONST_SYSM3_LEN      0x40000

#define DUCATI_MEM_IPC_HEAP0_ADDR       0xA0000000
#define DUCATI_MEM_IPC_HEAP0_LEN        0x54000
#define TESLA_MEM_IPC_HEAP0_ADDR        0x30000000

static const struct rproc_mem_entry ipu_memory_maps[] = {
	{DUCATI_BASEIMAGE_PHYSICAL_ADDRESS, DUCATI_MEM_IPC_HEAP0_ADDR,
		PAGE_SIZE_1MB},
	{0x9D000000, 0, PAGE_SIZE_16MB},
	{0x9E000000, DUCATI_MEM_CONST_SYSM3_ADDR, PAGE_SIZE_16MB},
	{0x9F000000, DUCATI_MEM_CONST_SYSM3_ADDR + PAGE_SIZE_16MB,
		 PAGE_SIZE_16MB},
	{L3_TILER_MODE_0_1_ADDR, DUCATI_TILER_MODE_0_1_ADDR,
		(PAGE_SIZE_16MB * 16)},
	{L3_TILER_MODE_2_ADDR, DUCATI_TILER_MODE_2_ADDR,
		(PAGE_SIZE_16MB * 8)},
	{L3_TILER_MODE_3_ADDR, DUCATI_TILER_MODE_3_ADDR,
		(PAGE_SIZE_16MB * 8)},
	{L4_PERIPHERAL_L4CFG, DUCATI_PERIPHERAL_L4CFG, PAGE_SIZE_16MB},
	{L4_PERIPHERAL_L4PER, DUCATI_PERIPHERAL_L4PER, PAGE_SIZE_16MB},
	{L3_IVAHD_CONFIG, DUCATI_IVAHD_CONFIG, PAGE_SIZE_16MB},
	{L3_IVAHD_SL2, DUCATI_IVAHD_SL2, PAGE_SIZE_16MB},
	{L4_PERIPHERAL_L4EMU, DUCATI_PERIPHERAL_L4EMU, PAGE_SIZE_16MB},
	{ }
};

static inline u32 iotlb_set_entry(struct iotlb_entry *e, u32 da, u32 pa,
								u32 pgsz)
{
	memset(e, 0, sizeof(*e));

	e->da           = da;
	e->pa           = pa;
	e->valid        = 1;
	e->pgsz         = pgsz;
	e->endian       = MMU_RAM_ENDIAN_LITTLE;
	e->elsz         = MMU_RAM_ELSZ_32;
	e->mixed        = 0;

	return iopgsz_to_bytes(e->pgsz);
}

static int proc44_map(struct iommu *obj, u32 da, u32 pa, u32 size)
{
	struct iotlb_entry e;
	u32 all_bits, i;
	u32 pg_size[] = {SZ_16M, SZ_1M, SZ_64K, SZ_4K};
	int size_flag[] = {MMU_CAM_PGSZ_16M, MMU_CAM_PGSZ_1M,
		MMU_CAM_PGSZ_64K, MMU_CAM_PGSZ_4K};

	while (size) {
		/*
		 * To find the max. page size with which both PA & VA are
		 * aligned
		 */
		all_bits = pa | da;
		for (i = 0; i < 4; i++) {
			if ((size >= pg_size[i]) &&
				((all_bits & (pg_size[i] - 1)) == 0)) {
				break;
			}
		}
		iotlb_set_entry(&e, da, pa, size_flag[i]);
		iopgtable_store_entry(obj, &e);
		size -= pg_size[i];
		da += pg_size[i];
		pa += pg_size[i];
	}
	return 0;
}

static inline int proc44x_start(struct device *dev, u32 start_addr)
{
	struct omap_rproc_platform_data *pdata = dev->platform_data;
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc *rproc = platform_get_drvdata(pdev);
	int ret, i;

	rproc->iommu = iommu_get(pdata->iommu_name);
	if (IS_ERR_OR_NULL(rproc->iommu)) {
		dev_err(dev, "iommu_get error: %ld\n", PTR_ERR(rproc->iommu));
		return PTR_ERR(rproc->iommu);
	}

	/* temporary workaround */
	clk_enable(rproc->iommu->clk);

	for (i = 0; pdata->memory_maps[i].size; i++) {
		const struct rproc_mem_entry *me = &pdata->memory_maps[i];

		proc44_map(rproc->iommu, me->da, me->pa, me->size);
	}

	ret = omap_device_enable(pdev);
	if (ret)
		return ret;

	return 0;
}

static inline int proc44x_stop(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_rproc *rproc = platform_get_drvdata(pdev);
	int ret;

	ret = omap_device_shutdown(pdev);
	if (ret)
		dev_err(dev, "failed to shutdown: %d\n", ret);

	iommu_put(rproc->iommu);

	clk_disable(rproc->iommu->clk);

	return ret;
}

static struct omap_rproc_ops omap4_gen_ops = {
	.start = proc44x_start,
	.stop = proc44x_stop,
};

static struct omap_rproc_platform_data omap4_rproc_data[] = {
	{
		.name		= "dsp",
		.iommu_name	= "tesla",
		.ops		= &omap4_gen_ops,
		.firmware	= "tesla-dsp.bin",
		.oh_name	= "dsp_c0",
	},
	{
		.name		= "ipu",
		.iommu_name	= "ducati",
		.ops		= &omap4_gen_ops,
		.firmware	= "ducati-m3.bin",
		.oh_name	= "ipu_c0",
		.oh_name_opt	= "ipu_c1",
		.memory_maps	= ipu_memory_maps,
		.trace_pa	= 0x9e000000,
	},
};

static struct omap_device_pm_latency omap_rproc_latency[] = {
	{
		.deactivate_func = omap_device_idle_hwmods,
		.activate_func = omap_device_enable_hwmods,
		.flags = OMAP_DEVICE_LATENCY_AUTO_ADJUST,
	},
};

static int __init omap4_rproc_init(void)
{
	const char *pdev_name = "omap-rproc";
	struct omap_hwmod *oh[2];
	struct omap_device *od;
	int i, ret = 0, oh_count;

	/* names like ipu_cx/dsp_cx might show up on other OMAPs, too */
	if (!cpu_is_omap44xx())
		return 0;

	for (i = 0; i < ARRAY_SIZE(omap4_rproc_data); i++) {
		char *oh_name = omap4_rproc_data[i].oh_name;
		char *oh_name_opt = omap4_rproc_data[i].oh_name_opt;
		oh_count = 0;

		oh[0] = omap_hwmod_lookup(oh_name);
		if (!oh[0]) {
			pr_err("could not look up %s\n", oh_name);
			continue;
		}
		oh_count++;

		if (oh_name_opt) {
			oh[1] = omap_hwmod_lookup(oh_name_opt);
			if (!oh[1]) {
				pr_err("could not look up %s\n", oh_name_opt);
				continue;
			}
			oh_count++;
		}

		od = omap_device_build_ss(pdev_name, i, oh, oh_count,
					&omap4_rproc_data[i],
					sizeof(struct omap_rproc_platform_data),
					omap_rproc_latency,
					ARRAY_SIZE(omap_rproc_latency),
					false);
		if (IS_ERR(od)) {
			pr_err("Could not build omap_device for %s:%s\n",
							pdev_name, oh_name);
			ret = PTR_ERR(od);
		}
	}

	return ret;
}
device_initcall(omap4_rproc_init);
