#ifndef _LINUX_RPMSG_H
#define _LINUX_RPMSG_H
#include <linux/types.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>

/* driver requests */
enum {
	VIRTIO_IPC_BUF_ADDR,
	VIRTIO_IPC_BUF_NUM,
	VIRTIO_IPC_BUF_SZ,
	VIRTIO_IPC_SIM_BASE,
	VIRTIO_IPC_PROC_ID, /* processor id 0 is reserved for loopback */
};

#define RPMSG_ADDR_ANY		0xFFFFFFFF

/**
 * rpmsg_channel - representation of a point-to-point rpmsg channel
 * @rp: the remote processor this channel connects to
 * @dev: underlying device
 * @id: the device type identification (used to match an rpmsg driver)
 * @src: local address of this channel
 * @dst: destination address that belongs to the remote service
 * @priv: private pointer for the driver's use.
 */
struct rpmsg_channel {
	struct rpmsg_rproc *rp;
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

struct rpmsg_endpoint *rpmsg_create_ept(struct rpmsg_channel *,
		void (*cb)(struct rpmsg_channel *, void *, int, void *, u32),
		void *priv, u32 addr);
void rpmsg_destroy_ept(struct rpmsg_endpoint *);
int rpmsg_send(struct rpmsg_channel *rpdev, void *data, int len);
int rpmsg_sendto(struct rpmsg_channel *rpdev, void *data, int len, u32 dst);
int rpmsg_send_offchannel(struct rpmsg_channel *, u32, u32, void *, int);

int register_rpmsg_device(struct rpmsg_channel *dev);
void unregister_rpmsg_device(struct rpmsg_channel *dev);

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

int register_rpmsg_driver(struct rpmsg_driver *drv);
void unregister_rpmsg_driver(struct rpmsg_driver *drv);

#endif /* _LINUX_RPMSG_H */
