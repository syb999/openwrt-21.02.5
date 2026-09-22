/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *
 * This header provides constants for the phy framework
 *
 * Copyright (C) 2014 STMicroelectronics
 * Author: Gabriel Fernandez <gabriel.fernandez@st.com>
 */

#ifndef _DT_BINDINGS_PHY
#define _DT_BINDINGS_PHY

#define PHY_NONE		0
#define PHY_TYPE_SATA		1
#define PHY_TYPE_PCIE		2
#define PHY_TYPE_USB2		3
#define PHY_TYPE_USB3		4
#define PHY_TYPE_UFS		5
/*
 * The four types below are used by the Rockchip combo PHY drivers, which
 * refer to the same PHY_TYPE_SGMII/QSGMII names that were only added
 * upstream after 5.4.  Values are taken from the Rockchip 5.10 BSP so that
 * driver and devicetree agree.
 */
#define PHY_TYPE_DP		6
#define PHY_TYPE_XPCS		7
#define PHY_TYPE_SGMII		8
#define PHY_TYPE_QSGMII		9

#endif /* _DT_BINDINGS_PHY */
