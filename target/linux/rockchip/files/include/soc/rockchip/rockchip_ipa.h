/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-in for the vendor include/soc/rockchip/rockchip_ipa.h.
 *
 * The IPA (leakage/static power) model is only used by the vendor devfreq
 * glue, which is not part of this port, so the RKNPU driver just needs the
 * type to exist for the pointer in its driver data.
 */
#ifndef __SOC_ROCKCHIP_IPA_H
#define __SOC_ROCKCHIP_IPA_H

struct device;

struct ipa_power_model_data;

#endif /* __SOC_ROCKCHIP_IPA_H */
