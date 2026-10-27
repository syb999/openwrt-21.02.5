include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-excel-match
PKG_VERSION:=1.0.0
PKG_RELEASE:=7

PKG_LICENSE:=MIT
PKG_MAINTAINER:=local

LUCI_TITLE:=Excel MATCH
LUCI_DESCRIPTION:=Match form data by columns
LUCI_DEPENDS:=+python3-light +python3-pandas +python3-openpyxl
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
