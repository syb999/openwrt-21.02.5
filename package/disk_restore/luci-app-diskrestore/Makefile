#
# Copyright (C) 2026 syb999
#
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-diskrestore
PKG_VERSION:=1.6.7
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-diskrestore
	SECTION:=luci
	CATEGORY:=LuCI
	SUBMENU:=3. Applications
	TITLE:=USB disk file recovery (ddrescue + testdisk + photorec)
	PKGARCH:=all
	# runtime commands -> the package that ships them (missing one = broken page):
	#   ddrescue/testdisk/photorec -> package/disk_restore/*
	#   blkid (-p PTTYPE, -s TYPE) -> blkid          (split out of util-linux)
	#   stat -c '%Y %n' (按日期整理时批量取时间戳) -> coreutils-stat
	#      🔴 stat 不是基本系统自带的(busybox 没有 stat applet), 但代码里有 ls 退路,
	#         所以这个包只是"让整理更快", 缺了也能用。
	#   smartctl (-H -A)           -> smartmontools
	#   mount/umount/losetup/dd/base64/awk/sed/find/du/df -> busybox (base system)
	#   mount.ntfs-3g              -> ntfs-3g (already pulled in by testdisk)
	DEPENDS:=+luci-base +ddrescue +testdisk +photorec +blkid +smartmontools +coreutils-stat
endef

define Package/luci-app-diskrestore/description
	Web UI to recover files from a damaged USB disk / external drive.
	The disk is copied with ddrescue (source is opened read only, bad areas
	are retried and an interrupted job resumes from its mapfile), the image
	is analysed with testdisk and files are carved with photorec.
	Jobs run in the background in their own worker, so the browser can be
	closed; the page shows live progress, the job log and the recovered
	file list.
endef

define Build/Prepare
endef

define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-diskrestore/install
	# Lua controller + view (uhttpd runs the CGI as root, files must be 644)
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller
	$(INSTALL_DATA) ./luasrc/controller/diskrestore.lua $(1)/usr/lib/lua/luci/controller/
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/view/diskrestore
	$(INSTALL_DATA) ./luasrc/view/diskrestore/main.htm $(1)/usr/lib/lua/luci/view/diskrestore/
	# NOTE: no .lmo is shipped. All UI strings use Chinese msgids
	# (<%:中文%>), so LuCI renders them verbatim when no dictionary is
	# present -- the same approach as luci-app-iptv-epg. po2lmo in this
	# tree exits 0 without producing a file, so building an identity
	# dictionary would only add a broken install step. po/ is kept as
	# the string reference.
	# backend: device scan + background worker
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./root/usr/bin/diskrestore $(1)/usr/bin/diskrestore
endef

$(eval $(call BuildPackage,luci-app-diskrestore))
