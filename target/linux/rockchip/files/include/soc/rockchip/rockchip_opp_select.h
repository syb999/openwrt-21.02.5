/*
 * Minimal stand-in for the vendor include/soc/rockchip/rockchip_opp_select.h.
 *
 * Only the types are provided: the RKNPU driver embeds struct rockchip_opp_info
 * in its own driver data, but the vendor OPP/DVFS framework
 * (drivers/soc/rockchip/rockchip_opp_select.c, with leakage/pvtpll handling)
 * is not part of this port.  rknpu is therefore built without its devfreq
 * half, so these structures are never dereferenced - they exist so the
 * back-ported sources compile unchanged.
 */
#ifndef __SOC_ROCKCHIP_OPP_SELECT_H
#define __SOC_ROCKCHIP_OPP_SELECT_H

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/types.h>

struct device_node;
struct rockchip_opp_info;

struct volt_rm_table {
	int volt;
	int rm;
};

struct pvtpll_opp_table {
	unsigned long rate;
	unsigned long u_volt;
	unsigned long u_volt_min;
	unsigned long u_volt_max;
	unsigned long u_volt_mem;
	unsigned long u_volt_mem_min;
	unsigned long u_volt_mem_max;
};

struct rockchip_opp_data {
	int (*get_soc_info)(struct device *dev, struct device_node *np,
			    int *bin, int *process);
	int (*set_soc_info)(struct device *dev, struct device_node *np,
			    int bin, int process, int volt_sel);
	int (*set_read_margin)(struct device *dev,
			       struct rockchip_opp_info *opp_info,
			       u32 rm);
};

struct rockchip_opp_info {
	struct device *dev;
	struct pvtpll_opp_table *opp_table;
	const struct rockchip_opp_data *data;
	struct volt_rm_table *volt_rm_tbl;
	struct regmap *grf;
	struct regmap *dsu_grf;
	struct clk_bulk_data *clks;
	struct clk *scmi_clk;
	/* The threshold frequency for set intermediate rate */
	unsigned long intermediate_threshold_freq;
	unsigned int pvtpll_avg_offset;
	unsigned int pvtpll_min_rate;
	unsigned int pvtpll_volt_step;
	int num_clks;
	/* The read margin for low voltage */
	u32 low_rm;
	u32 current_rm;
	u32 target_rm;
	u32 pvtpll_clk_id;
	bool pvtpll_smc;
	bool pvtpll_low_temp;
};

#endif /* __SOC_ROCKCHIP_OPP_SELECT_H */
