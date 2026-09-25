// SPDX-License-Identifier: GPL-2.0
/*
 * devfreq compatibility layer for the RKNPU driver back-ported to this 5.4
 * kernel.
 *
 * rknpu_devfreq.c is not built here: it needs the vendor OPP/DVFS framework
 * (rockchip_opp_select, IPA, system monitor), none of which is part of this
 * port.  Its header only falls back to inline stubs when CONFIG_PM_DEVFREQ is
 * disabled, and this kernel has it enabled, so the six exported helpers are
 * provided here instead:
 *
 *   - lock/unlock: in the vendor driver these serialise voltage/rate changes
 *     against devfreq's own callbacks.  Nothing changes rate or voltage now,
 *     so they are no-ops.
 *   - init/remove: no devfreq device or OPP table is registered.
 *   - runtime suspend/resume: with no OPP table the NPU keeps the fixed rate
 *     the SoC dtsi assigns (600 MHz) and the fixed vdd_npu rail, so there is
 *     nothing to save or restore.  They succeed so that the PM core sees a
 *     clean runtime-PM transition.
 *
 * Wiring DVFS back in means porting the vendor framework and swapping this
 * file for drivers/rknpu/rknpu_devfreq.c in the Makefile.
 */

#include <linux/errno.h>

/* rknpu_devfreq.h expects struct rknpu_device to be known already. */
#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
}

void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
}

int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	return -EOPNOTSUPP;
}

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
}

int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	return 0;
}

int rknpu_devfreq_runtime_resume(struct device *dev)
{
	return 0;
}
