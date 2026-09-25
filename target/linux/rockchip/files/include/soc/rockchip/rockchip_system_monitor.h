/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-in for the vendor include/soc/rockchip/rockchip_system_monitor.h.
 *
 * The RKNPU driver keeps a pointer to struct monitor_dev_info in its driver
 * data, but only the vendor devfreq/thermal glue dereferences it - and that
 * glue (rknpu_devfreq.c) is not part of this port.  A forward declaration is
 * therefore all that is needed.
 */
#ifndef __SOC_ROCKCHIP_SYSTEM_MONITOR_H
#define __SOC_ROCKCHIP_SYSTEM_MONITOR_H

struct device;

struct monitor_dev_info;
struct monitor_dev_profile;

#endif /* __SOC_ROCKCHIP_SYSTEM_MONITOR_H */
