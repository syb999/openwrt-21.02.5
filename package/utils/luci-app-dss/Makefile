include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-dss
PKG_VERSION:=1.0.26
PKG_RELEASE:=1

LUCI_TITLE:=LuCI app for DeepSeek shell client (dss): settings, command examples, live log, code download
LUCI_DEPENDS:=+luci-base +luci-compat +dss
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# 装/卸之后清掉 LuCI 的菜单缓存，否则新菜单要重启才出现
define Package/luci-app-dss/postinst
#!/bin/sh
rm -rf /tmp/luci-indexcache /tmp/luci-indexcache.* /tmp/luci-modulecache
exit 0
endef

define Package/luci-app-dss/prerm
#!/bin/sh
rm -rf /tmp/luci-indexcache /tmp/luci-indexcache.* /tmp/luci-modulecache
exit 0
endef

# call BuildPackage - OpenWrt buildroot signature
