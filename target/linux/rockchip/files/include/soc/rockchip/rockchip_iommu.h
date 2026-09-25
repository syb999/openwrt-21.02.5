/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-in for the vendor include/soc/rockchip/rockchip_iommu.h.
 *
 * The RKNPU driver (back-ported from the vendor 5.10 tree) is the only user
 * here and it needs exactly one helper: rockchip_iommu_is_enabled().  The
 * mainline rk-iommu driver in this kernel enables the IOMMU as soon as a
 * domain is attached to the master device, so "is the IOMMU enabled" can
 * simply be answered by asking whether a domain is attached.
 */
#ifndef __SOC_ROCKCHIP_IOMMU_H
#define __SOC_ROCKCHIP_IOMMU_H

#include <linux/device.h>
#include <linux/iommu.h>

static inline bool rockchip_iommu_is_enabled(struct device *dev)
{
	return iommu_get_domain_for_dev(dev) != NULL;
}

#endif /* __SOC_ROCKCHIP_IOMMU_H */
