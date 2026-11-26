# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-pon-debug
PKG_VERSION:=0.1.0
PKG_RELEASE:=3
PKG_URL:=https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-pon-debug

PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/airoha-pon-debug
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha PON diagnostic collector
  # 注意: 本包的 pcap 抓包功能依赖 tcpdump, 但 XG-040G-MF 的 UBI fit 卷只有 120 LEBs
# (15,237,120 字节), 镜像必须装得下才能 sysupgrade ⇒ 暂时不选 tcpdump(+libpcap, 压缩后约 336KB)。
# 等用恢复页第①步重建 UBI 把 fit 卷放大后, 把 +tcpdump 加回来即可。
  DEPENDS:=+airoha-ponctl +airoha-pond +tcpdump +ip-full +ip-bridge +ethtool
endef

define Package/airoha-pon-debug/description
 Collects PON status, logs and packet captures.
endef

define Build/Compile
endef

define Package/airoha-pon-debug/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/airoha-pon-debug.config $(1)/etc/config/airoha-pon-debug
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/airoha-pon-debug.init $(1)/etc/init.d/airoha-pon-debug
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/airoha-pon-debug $(1)/usr/libexec/airoha-pon-debug
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/pon-debug $(1)/usr/sbin/pon-debug
endef

$(eval $(call BuildPackage,airoha-pon-debug))
