# SPDX-License-Identifier: Apache-2.0
include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI PON configuration
LUCI_URL:=https://github.com/pbs05/openwrt-pon-userspace/tree/main/luci-app-pon
LUCI_DESCRIPTION:=PON configuration and status
LUCI_DEPENDS:=+luci-base +airoha-ponctl +airoha-pond +pon-board-identity \
	+mtd +ubi-utils +nand-utils +jshn +jsonfilter
LUCI_PKGARCH:=all
PKG_VERSION:=0.1.0
PKG_RELEASE:=3
PKG_LICENSE:=Apache-2.0 GPL-2.0-only

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage: feed scanner marker; luci.mk generates the package rules.
