include $(TOPDIR)/rules.mk

PKG_NAME:=cups-printer-choose
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/cups-printer-choose/Default
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Printing
  URL:=https://www.cups.org/
endef

define Package/cups-printer-choose
$(call Package/cups-printer-choose/Default)
  TITLE:=CUPS printer queue chooser
  DEPENDS:=+cups +cups-client +kmod-usb-printer \
	+CUPS_PRINTER_CHOOSE_HP_ZJS:foo2zjs \
	+CUPS_PRINTER_CHOOSE_BROTHER:brlaser \
	+CUPS_PRINTER_CHOOSE_CANON_BJNP:cups-bjnp
endef

define Package/cups-printer-choose/description
  One command on the router 
endef

define Package/cups-printer-choose/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/cups-printer-choose $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/50-cups-enable $(1)/etc/uci-defaults/
	$(INSTALL_DIR) $(1)/etc/hotplug.d/usb
	$(INSTALL_BIN) ./files/30-cups-printer-firmware $(1)/etc/hotplug.d/usb/
	$(INSTALL_DIR) $(1)/usr/share/ppd/epson
	$(INSTALL_DATA) ./files/ppd/Epson-LQ-850-24pin.ppd $(1)/usr/share/ppd/epson/
endef

define Build/Compile
endef

define Package/cups-printer-choose/config
	source "$(SOURCE)/Config.in"
endef

$(eval $(call BuildPackage,cups-printer-choose))
