/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-in for Rockchip's vendor-only <linux/rockchip/cpu.h>.
 *
 * The vendor (BSP 5.10) rockchip_thermal driver we backport uses it only for
 * two SoC-variant checks (px30s / rk3308bs).  Neither variant exists in this
 * tree, so both queries simply answer "no" and the driver falls back to the
 * plain px30 / rk3308 data - exactly the behaviour we want.
 */
#ifndef _LINUX_ROCKCHIP_CPU_H
#define _LINUX_ROCKCHIP_CPU_H

#include <linux/types.h>

static inline bool soc_is_px30s(void)   { return false; }
static inline bool soc_is_rk3308bs(void) { return false; }

#endif /* _LINUX_ROCKCHIP_CPU_H */
