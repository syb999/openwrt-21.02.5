include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-finddev
PKG_VERSION:=1

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-finddev
 	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=3. Applications
	TITLE:=LuCI Support for find device
	PKGARCH:=all
endef

define Package/luci-app-finddev/description
	finddev luci page
endef

define Build/Compile
endef

define Package/luci-app-finddev/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci
	cp -pR ./luasrc/* $(1)/usr/lib/lua/luci
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/finddev.conf $(1)/etc/config/finddev
	$(INSTALL_DIR) $(1)//usr/bin
	$(INSTALL_BIN) ./files/finddev.sh $(1)//usr/bin/finddev.sh
endef

$(eval $(call BuildPackage,luci-app-finddev))
