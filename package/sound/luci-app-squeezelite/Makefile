#
# This is free software, licensed under the Apache License, Version 2.0 .
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-squeezelite
PKG_VERSION:=1.0
PKG_RELEASE:=2

LUCI_TITLE:=LuCI Support for SqueezeLite
LUCI_DEPENDS:=+squeezelite-full
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

define Package/$(PKG_NAME)/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	chmod +x /etc/init.d/squeezelite /etc/init.d/squeezelite-volume
	(. /etc/uci-defaults/luci-squeezelite) && rm -f /etc/uci-defaults/luci-squeezelite
	rm -f /tmp/luci-indexcache
	rm -rf /tmp/luci-modulecache/
	killall -HUP rpcd 2>/dev/null
	exit 0
}
endef

# call BuildPackage - OpenWrt buildroot signature
