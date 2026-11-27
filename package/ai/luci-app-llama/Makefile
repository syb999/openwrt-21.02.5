include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-llama
PKG_VERSION:=3.1.139
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_MAINTAINER:=local

LUCI_TITLE:=LuCI app for local llama.cpp inference
LUCI_DESCRIPTION:=llama-server (GGUF, pure CPU) plus a login-free page at /llama.htm \
 with a C agent: 14 plugins (math / curl / jq / doc / sys / net / owrt / music / batch / table / sysmon / browser / netcap / video)
LUCI_DEPENDS:=+luci-base +zlib +jq-full +curl +python3-light +desub-rk3566 +llama.cpp +sr-rk3566 +realesrgan-rk3566

LUCI_PKGARCH:=aarch64_cortex-a53

MAKE_FLAGS += \
	CC="$(TARGET_CC)" \
	CFLAGS="$(TARGET_CFLAGS) -I. -D_FORTIFY_SOURCE=0" \
	LDFLAGS=""

include $(TOPDIR)/feeds/luci/luci.mk

define Package/luci-app-llama/postinst
rm -f /tmp/luci-indexcache* 2>/dev/null
rm -rf /tmp/luci-modulecache 2>/dev/null
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/llama enable >/dev/null 2>&1
exit 0
endef

define Package/luci-app-llama/conffiles
/etc/config/llama
endef

# call BuildPackage - OpenWrt buildroot signature

