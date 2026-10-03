# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-pond
PKG_VERSION:=0.3.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/airoha-pond
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha PON OMCI/OAM agent (C)
  DEPENDS:=+kmod-airoha-xpon +libubox +ip-full
endef

define Package/airoha-pond/description
 Userspace OMCI (GPON family) and OAM (EPON family) agent for Airoha PON.
 C implementation of airoha-pond; feeds ONU provisioning to the MAC driver.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -o $(PKG_BUILD_DIR)/airoha-pond \
		$(PKG_BUILD_DIR)/*.c $(TARGET_LDFLAGS) -pthread
endef

define Package/airoha-pond/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/airoha-pond $(1)/usr/sbin/airoha-pond
	$(INSTALL_DIR) $(1)/usr/bin
	$(LN) /usr/sbin/airoha-pond $(1)/usr/bin/pondctl
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/pond.init $(1)/etc/init.d/airoha-pond
endef

$(eval $(call BuildPackage,airoha-pond))
