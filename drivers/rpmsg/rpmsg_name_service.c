/*
 * Remote processor messaging - name service
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 *
 * Ohad Ben-Cohen <ohad@wizery.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg_name_service.h>

static void rpmsg_ns_cb(struct rpmsg_channel *rpdev, void *data, int len,
						void *priv, u32 src)
{
	struct rpmsg_ns_msg *msg = data;
	struct rpmsg_channel *newch;
	struct rpmsg_channel_info chinfo;
	int ret;

	print_hex_dump(KERN_DEBUG, __func__, DUMP_PREFIX_NONE, 16, 1,
						data, len, true);

	if (len != sizeof(*msg)) {
		dev_err(&rpdev->dev, "malformed ns msg (%d)\n", len);
		return;
	}

	/* don't trust remote processor for null terminating the name */
	msg->name[RPMSG_NAME_SIZE - 1] = '\0';

	dev_info(&rpdev->dev, "%s: %s service %s addr %d\n", __func__,
			msg->flags & RPMSG_NS_DESTROY ? "destroy" : "create",
			msg->name, msg->addr);

	strncpy(chinfo.name, msg->name, sizeof(chinfo.name));
	chinfo.src = RPMSG_ADDR_ANY;
	chinfo.dst = msg->addr;

	if (msg->flags & RPMSG_NS_DESTROY) {
		ret = rpmsg_destroy_channel(rpdev->vrp, &chinfo);
		if (ret)
			dev_err(&rpdev->dev, "destroy failed: %d\n", ret);
	} else {
		newch = rpmsg_create_channel(rpdev->vrp, &chinfo);
		if (!newch)
			dev_err(&rpdev->dev, "rpmsg_create_channel failed\n");
	}
}

int rpmsg_ns_publish(struct rpmsg_channel *rpdev)
{
	struct rpmsg_channel_info chinfo;
	int ret;

	strncpy(chinfo.name, rpdev->id.name, RPMSG_NAME_SIZE);
	chinfo.src = rpdev->src;
	chinfo.dst = RPMSG_ADDR_ANY;

	ret = rpmsg_sendto(rpdev, &chinfo, sizeof(chinfo), 53);
	if (ret) {
		dev_err(&rpdev->dev, "rpmsg_send failed: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(rpmsg_ns_publish);

static int rpmsg_ns_probe(struct rpmsg_channel *rpdev)
{
	int ret;

	dev_info(&rpdev->dev, "nameservice channel: 0x%x -> 0x%x!\n",
			rpdev->src, rpdev->dst);

	/* tell remote name service we're up */
	ret = rpmsg_send(rpdev, "UP!", 3);
	if (ret) {
		pr_err("rpmsg_send failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static void __devexit rpmsg_ns_remove(struct rpmsg_channel *rpdev)
{
	dev_info(&rpdev->dev, "rpmsg ns driver is removed\n");
}

static struct rpmsg_device_id rpmsg_driver_ns_id_table[] = {
	{ .name	= "rpmsg-name-service" },
	{ },
};
MODULE_DEVICE_TABLE(platform, rpmsg_driver_ns_id_table);

static struct rpmsg_driver rpmsg_ns_server = {
	.drv.name	= KBUILD_MODNAME,
	.drv.owner	= THIS_MODULE,
	.id_table	= rpmsg_driver_ns_id_table,
	.probe		= rpmsg_ns_probe,
	.callback	= rpmsg_ns_cb,
	.remove		= __devexit_p(rpmsg_ns_remove),
};

static int __init init(void)
{
	return register_rpmsg_driver(&rpmsg_ns_server);
}

static void __exit fini(void)
{
	unregister_rpmsg_driver(&rpmsg_ns_server);
}
module_init(init);
module_exit(fini);

MODULE_DESCRIPTION("rpmsg name service driver");
MODULE_LICENSE("GPL v2");
