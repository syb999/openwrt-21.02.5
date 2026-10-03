# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-ponctl
PKG_VERSION:=0.2.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/airoha-ponctl
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha PON control utility (C)
  DEPENDS:=+kmod-airoha-xpon
endef

define Package/airoha-ponctl/description
 Configures PON mode, identity, status and data paths.
 C implementation of the airoha-ponctl utility; talks to the "xpon"
 generic netlink family and to the driver's sysfs attributes.
endef

define Package/airoha-ponctl/conffiles
/etc/config/pon
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=gnu11 \
		-o $(PKG_BUILD_DIR)/ponctl $(PKG_BUILD_DIR)/ponctl.c -lm
endef

define Package/airoha-ponctl/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ponctl $(1)/usr/sbin/ponctl
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/pon.init $(1)/etc/init.d/pon
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/pon.config $(1)/etc/config/pon
endef

$(eval $(call BuildPackage,airoha-ponctl))
