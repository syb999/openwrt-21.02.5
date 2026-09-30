#
# Copyright (C) 2026 RK3566 Panther X2 project
#
# This is free software, licensed under the Apache License, Version 2.0 .
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-vputrans
PKG_VERSION:=2026-09-30
PKG_RELEASE:=4
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Panther X2 Project

# luci.mk supplies the build/install rules and the i18n plumbing: every
# po/<lang>/*.po is compiled with po2lmo into
#   /usr/lib/lua/luci/i18n/<basename>.<lang>.lmo
# and shipped in the sub-package luci-i18n-vputrans-<lang>, so
#   po/zh_Hans/vputrans.po -> luci-i18n-vputrans-zh-cn (vputrans.zh-cn.lmo)
# Select CONFIG_PACKAGE_luci-i18n-vputrans-zh-cn=y to install the Chinese UI;
# without it the English source strings are used (that is the default).
#
# This app ships a JS view under htdocs/, a POSIX sh backend under root/ and
# the translation catalogs under po/.
LUCI_TITLE:=LuCI App for RK3566 hardware video transcode (vputrans)
#
# coreutils-stat: `stat -c %s` is used to size files in O(1).  BusyBox does NOT
# provide stat in this tree (/bin/stat -> /usr/libexec/stat-coreutils comes from
# coreutils-stat), and `wc -c < file` would read the whole file (measured ~2s of
# CPU for a 60MB clip, which made the web probe crawl).
LUCI_DEPENDS:=+luci-base +ffmpeg-rkrga +ffprobe +coreutils-stat
LUCI_PKGARCH:=all

# luci.mk only defines a default postinst `ifndef`, so this one replaces it:
# same cache clearing as the default, plus a fresh mtime on the shipped view JS.
# The packaging preserves the source file mtime (the installed file ends up
# dated like the checkout), and a months-old Last-Modified makes browsers treat
# their cached copy as heuristically fresh for a long time ==> "前端改了没生效".
define Package/luci-app-vputrans/postinst
[ -n "$${IPKG_INSTROOT}" ] || {
	rm -f /tmp/luci-indexcache
	rm -rf /tmp/luci-modulecache/
	killall -HUP rpcd 2>/dev/null
	touch /www/luci-static/resources/view/vputrans/main.js 2>/dev/null
	exit 0
}
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
