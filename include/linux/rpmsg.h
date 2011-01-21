#ifndef _LINUX_RPMSG_H
#define _LINUX_RPMSG_H
/*
 * This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers.
 */

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>

/**
 * struct rpmsg_hdr -
 *
 * ... keep documenting ...
 */
struct rpmsg_hdr {
	u16 len;
	u16 flags;
	u32 src;
	u32 dst;
	u32 unused;
	u8 data[0];
} __packed;

struct rpmsg_channel_hdr {
	char name[RPMSG_NAME_SIZE];
	u32 src;
	u32 dst;
} __packed;

/* driver requests */
enum {
	VPROC_BUF_ADDR,
	VPROC_BUF_NUM,
	VPROC_BUF_SZ,
	VPROC_SIM_BASE,
	VPROC_HC_CHANNELS,
};

#define RPMSG_ADDR_ANY		0xFFFFFFFF

struct virtproc_info;

/**
 * rpmsg_channel - representation of a point-to-point rpmsg channel
 * @vrp: the remote processor this channel connects to
 * @dev: underlying device
 * @id: the device type identification (used to match an rpmsg driver)
 * @src: local address of this channel
 * @dst: destination address of the remote service
 * @priv: private pointer for the driver's use.
 */
struct rpmsg_channel {
	struct virtproc_info *vrp;
	struct device dev;
	struct rpmsg_device_id id;
	u32 src;
	u32 dst;
	void *priv;
	struct rpmsg_endpoint *ept;
};

/**
 * struct rpmsg_endpoint
 *
 * @rpdev:
 * @cb:
 * @src: local rpmsg address
 * @priv:
 */
struct rpmsg_endpoint {
	struct rpmsg_channel *rpdev;
	void (*cb)(struct rpmsg_channel *, void *, int, void *, u32);
	u32 addr;
	void *priv;
};

/**
 * rpmsg_driver - operations for a rpmsg I/O driver
 * @driver: underlying device driver (populate name and owner).
 * @id_table: the ids serviced by this driver.
 * @probe: the function to call when a device is found.  Returns 0 or -errno.
 * @remove: the function when a device is removed.
 * @callback: invoked when a message is received on the channel
 */
struct rpmsg_driver {
	struct device_driver drv;
	const struct rpmsg_device_id *id_table;
	int (*probe)(struct rpmsg_channel *dev);
	void (*remove)(struct rpmsg_channel *dev);
	void (*callback)(struct rpmsg_channel *, void *, int, void *, u32);
};

struct rpmsg_endpoint *rpmsg_create_ept(struct rpmsg_channel *,
		void (*cb)(struct rpmsg_channel *, void *, int, void *, u32),
		void *priv, u32 addr);
void rpmsg_destroy_ept(struct rpmsg_endpoint *);
int rpmsg_send(struct rpmsg_channel *rpdev, void *data, int len);
int rpmsg_sendto(struct rpmsg_channel *rpdev, void *data, int len, u32 dst);
int rpmsg_send_offchannel(struct rpmsg_channel *, u32, u32, void *, int);

int register_rpmsg_device(struct rpmsg_channel *dev);
void unregister_rpmsg_device(struct rpmsg_channel *dev);

int register_rpmsg_driver(struct rpmsg_driver *drv);
void unregister_rpmsg_driver(struct rpmsg_driver *drv);

#endif /* _LINUX_RPMSG_H */
