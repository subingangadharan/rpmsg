/*
 * Copyright (C) 2010 Texas Instruments, Inc.
 *
 * Author: Ohad Ben-Cohen <ohad@wizery.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */


#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/radix-tree.h>
#include <linux/hwspinlock.h>
#include <linux/pm_runtime.h>
#include <linux/hardirq.h>

#include "hwspinlock_internal.h"

struct hwspinlock *hwlock;

static int __init test_hwspinlock_init(void)
{
	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	pr_info("%s: requested lock %d\n", __func__, hwspin_lock_get_id(hwlock));

	return 0;
}
module_init(test_hwspinlock_init);

static void __exit test_hwspinlock_exit(void)
{
	int ret;

	ret = hwspin_lock_free(hwlock);
	if (ret)
		pr_err("%s: free failed: %d\n", __func__, ret);
	else
		pr_info("%s: freed lock %d\n", __func__, hwspin_lock_get_id(hwlock));
	return;
}
module_exit(test_hwspinlock_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Test module for the common hardware spinlock interface");
MODULE_AUTHOR("Ohad Ben-Cohen <ohad@wizery.com>");
