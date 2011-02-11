/*
 * OMAP Remote Processor driver
 *
 * Copyright (C) 2011 Texas Instruments Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef REMOTEPROC_H
#define REMOTEPROC_H

/**
 * The following enums and structures define the binary format of the images
 * we load and run the remote processors with.
 *
 * The binary format is as follows:
 *
 * struct {
 *     char magic[4] = { 'T', 'I', 'F', 'W' };
 *     u32 version;
 *     u32 header_len;
 *     char header[...] = { header_len bytes of unformatted, textual header };
 *     struct sections {
 *         u32 type;
 *         u32 da;
 *         u32 len;
 *         u8 content[...] = { len bytes of binary data };
 *     } [ no limit on number of sections ];
 * } __packed;
 */
enum omap_fw_resource_type {
	RSC_MEMORY	= 0,
	RSC_DEVICE	= 1,
	RSC_IRQ		= 2,
	RSC_SERVICE	= 3,
	RSC_TRACE	= 4,
	RSC_BOOTADDR	= 5,
	RSC_END		= 6,
};

enum omap_fw_section_type {
	FW_RESOURCE	= 0,
	FW_TEXT		= 1,
	FW_DATA		= 2,
};

struct omap_fw_resource {
	u32 type;
	u32 da;
	u32 len;
	u32 reserved;
	u8 name[48];
} __packed;

struct omap_fw_section {
	u32 type;
	u32 da;
	u32 len;
	char content[0];
} __packed;

struct omap_fw_format {
	char magic[4];
	u32 version;
	u32 header_len;
	char header[0];
} __packed;

/**
 * struct rproc_mem_entry - descriptor of a remote memory region
 *
 * @pa:		physical address of this region
 * @da:		the virtual address of this region, as seen by the remote
 *		processor (aka device address)
 * @size:	size of this memory region
 */
struct rproc_mem_entry {
	u32 pa;
	u32 da;
	u32 size;
};

struct omap_rproc;

#define DUCATI_BASEIMAGE_PHYSICAL_ADDRESS    0x9CF00000
#define TESLA_BASEIMAGE_PHYSICAL_ADDRESS     0x9CC00000

struct omap_rproc_ops {
	int (*start)(struct device *dev, u32 start_addr);
	int (*stop)(struct device *dev);
	int (*get_state)(struct device *dev);
};

struct omap_rproc_clk_t {
	void *clk_handle;
	const char *dev_id;
	const char *con_id;
};

/*
 * enum - remote processor states
 *
 * @OMAP_RPROC_OFFLINE: needs firmware load and init to exit this state.
 *
 * @OMAP_RPROC_SUSPENDED: needs to be woken up to receive a message.
 *
 * @OMAP_RPROC_RUNNING: does not need to be woken up to receive a message,
 * may send a request to be placed in SUSPENDED.
 *
 * @OMAP_RPROC_LOADING: asynchronous firmware loading has started
 *
 * @OMAP_RPROC_CRASHED: needs to be logged, connections torn down, resources
 * released, and returned to OFFLINE.
 */
enum {
	OMAP_RPROC_OFFLINE,
	OMAP_RPROC_SUSPENDED,
	OMAP_RPROC_RUNNING,
	OMAP_RPROC_LOADING,
	OMAP_RPROC_CRASHED,
};

struct omap_rproc_common_args {
	int status;
};

struct omap_rproc_platform_data {
	struct omap_rproc_ops *ops;
	char *name;
	char *iommu_name;
	char *oh_name;
	char *oh_name_opt;
	char *firmware;
	const struct rproc_mem_entry *memory_maps;
	u32 trace_pa;
};

struct omap_rproc {
	struct list_head next;
	const char *name;
	struct device *dev;
	struct iommu *iommu;
	int count;
	int state;
	struct mutex lock;
	struct dentry *dbg_dir;
	char *trace_buf0, *trace_buf1;
	int trace_len0, trace_len1;
	struct completion firmware_loading_complete;
};

struct omap_rproc_start_args {
	u32 start_addr;
};

struct omap_rproc_reg_event_args {
	struct omap_rproc_common_args cargs;
	u16 pro_id;
	int fd;
	u32 event;
};

struct omap_rproc *omap_rproc_get(const char *name);
void omap_rproc_put(struct omap_rproc *rproc);

#endif /* REMOTEPROC_H */
