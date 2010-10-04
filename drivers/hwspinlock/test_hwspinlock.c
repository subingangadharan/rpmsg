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

void bare_hwspin_unlock(struct hwspinlock *hwlock)
{
	if (WARN_ON(!hwlock))
		return;

	/*
	 * the memory barrier induced by the spin_unlock below is too late.
	 * the other core is going to access memory soon after it will take
	 * the hwspinlock, and by then we want to be sure our memory operations
	 * are already visible.
	 */
	mb();

	hwlock->ops->unlock(hwlock);
}

int bare_hwspin_trylock(struct hwspinlock *hwlock)
{
	int ret;

	if (WARN_ON(!hwlock))
		return -EINVAL;

	ret = hwlock->ops->trylock(hwlock);
	if (!ret) {
		return -EBUSY;
	}

	/* see above for explanation of this barrier */
	mb();

	return 0;
}

int
bare_hwspin_lock_timeout(struct hwspinlock *hwlock, signed long to)
{
	unsigned long expire;
	if (WARN_ON(!hwlock))
		return -EINVAL;

	if (to < 0) {
		dev_err(hwlock->dev, "%s: wrong timeout value %lx\n",
							__func__, to);
		return -EINVAL;
	}

	expire = to + jiffies;
	pr_info("%s: jiffies %lu expire %lu timeout %ld\n", __func__, jiffies,
								expire, to);

	for (;;) {
		/*
		 * This spin_lock_irqsave serves two purposes:
		 *
		 * 1. Disable local interrupts and preemption, in order to
		 *    minimize the period of time in which the hwspinlock
		 *    is taken (caller will not preempted). This is
		 *    important in order to minimize the possible polling on
		 *    the hardware interconnect by a remote user of this lock.
		 * 2. Make the hwspinlock SMP-safe (so we can take it from
		 *    additional contexts on the local host).
		 */

		if (hwlock->ops->trylock(hwlock))
			break;

	//pr_info("%s: jiffies %lu timeout %lu\n", __func__, jiffies, timeout);

		if (to != MAX_SCHEDULE_TIMEOUT &&
				time_is_before_eq_jiffies(expire)) {
			pr_info("%s: jiffies %lu expire %lu timeout %ld\n",
					__func__, jiffies, expire, to);
			return -ETIMEDOUT;
		}

		if (hwlock->ops->relax)
			hwlock->ops->relax(hwlock);
	}

	/*
	 * the implicit memory barrier of the spinlock above is not
	 * enough; we can be sure the other core's memory operations
	 * are visible to us only _after_ we take the hwspinlock
	 */
	mb();

	return 0;
}

static int test_single_timeout(void)
{
	struct hwspinlock *hwlock;
	int ret;
	unsigned long flags;
	unsigned long flags2;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}
	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}

	ret = hwspin_lock_timeout_irqsave(hwlock, msecs_to_jiffies(50), &flags);
	if (ret) {
		pr_err("%s: failed to take the lock!\n", __func__);
		return -1;
	}
	if (!irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}

	ret = hwspin_trylock_irqsave(hwlock, &flags2);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock_irqrestore(hwlock, &flags);
//	if (ret) {
//		pr_err("%s: ulock failed: %d\n", __func__, ret);
//		return -1;
//	}
	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}

	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_timeout_zero(void)
{
	struct hwspinlock *hwlock;
	int ret;
	unsigned long flags2;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_timeout(hwlock, 0);
	if (ret) {
		pr_err("%s: failed to take the lock!\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	//relevant only if kernel is compiled with smp support
//	if (!in_atomic()) {
//		pr_err("%s: not in atomic!\n", __func__);
//		return -1;
//	}

//	pr_err("%s: should see stack trace:\n", __func__);
//	might_sleep();
//	pr_err("%s: did you see one ?\n", __func__);

	ret = hwspin_trylock_irqsave(hwlock, &flags2);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
							__func__);
		return -1;
	}
	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}

	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
							__func__);
		return -1;
	}

	hwspin_unlock(hwlock);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_deadlock(void)
{
	struct hwspinlock *hwlock;
	int ret;

	pr_info("%s\n", __func__);

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = bare_hwspin_trylock(hwlock);
	if (ret) {
		pr_err("%s: surprisingly failed to take the lock!\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	pr_info("%s: beginning deadlock !\n", __func__);
	pr_info("%s: jiffies %lu msecs_to_jiffies %lu\n", __func__, jiffies,
						msecs_to_jiffies(1000));
	ret = bare_hwspin_lock_timeout(hwlock, msecs_to_jiffies(1000));
	if (ret != -ETIMEDOUT) {
		pr_err("%s: surprisingly weird error code %d\n", __func__, ret);
		return -1;
	}
	pr_info("%s: e/o deadlock !\n", __func__);

	bare_hwspin_unlock(hwlock);

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_lock_irqsave(void)
{
	struct hwspinlock *hwlock;
	int ret;
	unsigned long flags;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_timeout_irqsave(hwlock, MAX_SCHEDULE_TIMEOUT, &flags);
	if (ret) {
		pr_err("%s: lock failed: %d\n", __func__, ret);
		return -1;
	}
	if (!irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}

//	if (!in_atomic()) {
//		pr_err("%s: not in atomic!\n", __func__);
//		return -1;
//	}

	//pr_err("%s: should see stack trace:\n", __func__);
//	might_sleep();
//	pr_err("%s: did you see one ?\n", __func__);

	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock_irqrestore(hwlock, &flags);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_lock_irq(void)
{
	struct hwspinlock *hwlock;
	int ret;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_timeout_irq(hwlock, MAX_SCHEDULE_TIMEOUT);
	if (ret) {
		pr_err("%s: lock failed: %d\n", __func__, ret);
		return -1;
	}

//	if (!in_atomic()) {
//		pr_err("%s: not in atomic!\n", __func__);
//		return -1;
//	}

//	pr_err("%s: should see stack trace:\n", __func__);
//	might_sleep();
//	pr_err("%s: did you see one ?\n", __func__);

	//schedule();

	if (!irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}

	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock_irq(hwlock);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_lock(void)
{
	struct hwspinlock *hwlock;
	int ret;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_timeout(hwlock, MAX_SCHEDULE_TIMEOUT);
	if (ret) {
		pr_err("%s: lock failed: %d\n", __func__, ret);
		return -1;
	}

//	if (!in_atomic()) {
//		pr_err("%s: not in atomic!\n", __func__);
//		return -1;
//	}

//	pr_err("%s: should see stack trace:\n", __func__);
//	might_sleep();
//	pr_err("%s: did you see one ?\n", __func__);

	if (irqs_disabled()) {
		pr_err("%s: disabled!\n", __func__);
		return -1;
	}
	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock(hwlock);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_trylock_irqsave(void)
{
	struct hwspinlock *hwlock;
	int ret;
	unsigned long flags;
	unsigned long flags2;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_trylock_irqsave(hwlock, &flags);
	if (ret) {
		pr_err("%s: failed to take the lock!\n", __func__);
		return -1;
	}

	if (!irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}
	ret = hwspin_trylock_irqsave(hwlock, &flags2);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock_irqrestore(hwlock, &flags);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	return 0;
}

static int test_single_trylock_irq(void)
{
	struct hwspinlock *hwlock;
	int ret;
	unsigned long flags2;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_trylock_irq(hwlock);
	if (ret) {
		pr_err("%s: failed to take the lock!\n", __func__);
		return -1;
	}

	if (!irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}
	ret = hwspin_trylock_irqsave(hwlock, &flags2);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock_irq(hwlock);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int test_single_trylock(void)
{
	struct hwspinlock *hwlock;
	int ret;
	unsigned long flags2;

	hwlock = hwspin_lock_request();
	if (!hwlock) {
		pr_err("%s: request failed\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_trylock(hwlock);
	if (ret) {
		pr_err("%s: failed to take the lock!\n", __func__);
		return -1;
	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = bare_hwspin_trylock(hwlock);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}
	ret = hwspin_trylock_irqsave(hwlock, &flags2);
	if (!ret) {
		pr_err("%s: surprisingly managed to take the lock again!\n",
								__func__);
		return -1;
	}

	hwspin_unlock(hwlock);
//	if (ret) {
//		pr_err("%s: unlock failed: %d\n", __func__, ret);
//		return -1;
//	}

	if (irqs_disabled()) {
		pr_err("%s: not disabled!\n", __func__);
		return -1;
	}
	ret = hwspin_lock_free(hwlock);
	if (ret) {
		pr_err("%s: free failed: %d\n", __func__, ret);
		return -1;
	}

	return 0;
}

static int stress_request_specific_free(void)
{
	struct hwspinlock *hwlock[33];
	int i, ret;

	for (i = 0; i < 32; i++) {
		hwlock[i] = hwspin_lock_request_specific(i);
		if (!hwlock[i]) {
			pr_err("%s: request failed\n", __func__);
			return -1;
		}
	}

	hwlock[32] = hwspin_lock_request();
	if (hwlock[i]) {
		pr_info("%s: request succeeded unexpedtadly: %p, id %d\n",
				__func__, hwlock[i],
				hwspin_lock_get_id(hwlock[i]));
		return -1;
	}

	for (i = 0; i < 32; i++) {
		hwlock[32] = hwspin_lock_request_specific(i);
		if (hwlock[32]) {
			pr_info("%s: surprising request succeeded: %p, id %d\n",
					__func__, hwlock[32],
				hwspin_lock_get_id(hwlock[32]));
			return -1;
		}
	}

	for (i = 0; i < 32; i++) {
		ret = hwspin_lock_free(hwlock[i]);
		if (ret) {
			pr_err("%s: free failed: %d\n", __func__, ret);
			return -1;
		}
	}

	return 0;
}

static int stress_request_free(void)
{
	struct hwspinlock *hwlock[33];
	int i, ret;

	for (i = 0; i < 32; i++) {
		hwlock[i] = hwspin_lock_request();
		if (!hwlock[i]) {
			pr_err("%s: request failed\n", __func__);
			return -1;
		}
	}

	hwlock[32] = hwspin_lock_request();
	if (hwlock[i]) {
		pr_info("%s: request succeeded unexpedtadly: %p, id %d\n",
					__func__, hwlock[i],
				hwspin_lock_get_id(hwlock[i]));
		return -1;
	}

	for (i = 0; i < 32; i++) {
		hwlock[32] = hwspin_lock_request_specific(i);
		if (hwlock[32]) {
			pr_info("%s: surprising request succeeded: %p, id %d\n",
					__func__, hwlock[32],
				hwspin_lock_get_id(hwlock[32]));
			return -1;
		}
	}

	for (i = 0; i < 32; i++) {
		ret = hwspin_lock_free(hwlock[i]);
		if (ret) {
			pr_err("%s: free failed: %d\n", __func__, ret);
			return -1;
		}
	}

	return 0;
}

static int stress_many_requests(void)
{
	int i;
	int ret;

	pr_info("%s\n", __func__);

	for (i = 0; i < 1000; i++) {
		ret = stress_request_free();
		if (ret)
			goto err;
		ret = stress_request_specific_free();
		if (ret)
			goto err;
		ret = test_single_trylock();
		if (ret)
			goto err;
		ret = test_single_lock();
		if (ret)
			goto err;
		ret = test_single_trylock_irqsave();
		if (ret)
			goto err;
		ret = test_single_lock_irqsave();
		if (ret)
			goto err;
		ret = test_single_trylock_irq();
		if (ret)
			goto err;
		ret = test_single_lock_irq();
		if (ret)
			goto err;
		ret = test_single_timeout_zero();
		if (ret)
			goto err;
		ret = test_single_timeout();
		if (ret)
			goto err;
	}

	pr_info("%s: success :) i=%d\n", __func__, i);
	return 0;
err:
	pr_err("%s: failed :/ i=%d\n", __func__, i);
	return ret;
}

static int __init test_hwspinlock_init(void)
{
	int ret;

	ret = stress_many_requests();
	if (ret)
		goto err;

	ret = test_single_deadlock();
	if (ret)
		goto err;

	ret = stress_many_requests();
	if (ret)
		goto err;

	ret = test_single_deadlock();
	if (ret)
		goto err;

	ret = stress_many_requests();
	if (ret)
		goto err;

	ret = test_single_deadlock();
	if (ret)
		goto err;

	ret = stress_many_requests();
	if (ret)
		goto err;

	ret = test_single_deadlock();
	if (ret)
		goto err;

	pr_info("%s: test suite succeeded! Yay!\n", __func__);
	return 0;
err:
	pr_err("%s: test suite failed!\n", __func__);
	return ret;
}
module_init(test_hwspinlock_init);

static void __exit test_hwspinlock_exit(void)
{
	return;
}
module_exit(test_hwspinlock_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Test module for the common hardware spinlock interface");
MODULE_AUTHOR("Ohad Ben-Cohen <ohad@wizery.com>");
