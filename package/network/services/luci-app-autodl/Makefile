include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-autodl
PKG_VERSION:=20230219

include $(INCLUDE_DIR)/package.mk

define Package/$(PKG_NAME)/config
menu "Configuration"
	
config PACKAGE_$(PKG_NAME)_INCLUDE_Online_Server
	bool "Include Online Server"
	default n
	help
	requires python3.6+ with pip(for fastapi and uvicorn),node,ffmpeg,pillow,qrcode

endmenu
endef

define Package/luci-app-autodl
 	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=3. Applications
	TITLE:=LuCI Support for auto download video & audio
	PKGARCH:=all
	DEPENDS:=+curl +wget-ssl +urlencode \
	+PACKAGE_$(PKG_NAME)_INCLUDE_Online_Server:online_server
endef

define Build/Prepare
	$(foreach po,$(wildcard ${CURDIR}/luasrc/i18n/*.po), \
		po2lmo $(po) $(PKG_BUILD_DIR)/$(patsubst %.po,%.lmo,$(notdir $(po)));)
endef

define Build/Compile
endef

define Package/luci-app-autodl/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci
	cp -pR ./luasrc/* $(1)/usr/lib/lua/luci
	$(INSTALL_DIR) $(1)/usr/autodl
	$(INSTALL_BIN) ./files/autodl/* $(1)/usr/autodl
	$(INSTALL_DIR) $(1)/usr/autodl/ols
	$(INSTALL_BIN) ./files/ols/* $(1)/usr/autodl/ols
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/autodl.conf $(1)/etc/config/autodl
endef

$(eval $(call BuildPackage,luci-app-autodl))
