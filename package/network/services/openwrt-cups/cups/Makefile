#
# Copyright (C) 2006-2016 OpenWrt.org
# Copyright (C) 2016 lede-project.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=cups
PKG_VERSION:=2.3.3
PKG_RELEASE:=2

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-source.tar.gz
PKG_SOURCE_URL:=https://github.com/apple/cups/releases/download/v$(PKG_VERSION)/
PKG_HASH:=261fd948bce8647b6d5cb2a1784f0c24cc52b5c4e827b71d726020bcc502f3ee

PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

# libcups/libcupsimage are built and packaged by
# feeds/packages/libs/libcups (same upstream version, same tarball), so this
# package only ships the CUPS daemon and its tools. Use a dedicated build
# directory: PKG_NAME/PKG_VERSION are identical in both Makefiles.
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-daemon-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

PKG_UNPACK:=$(HOST_TAR) -C $(PKG_BUILD_DIR) --strip-components=1 -xzf $(DL_DIR)/$(PKG_SOURCE)

STRIP:=true

define Package/cups/Default
  URL:=http://www.cups.org/
  SUBMENU:=Printing
  DEPENDS:=+libcups
endef

define Package/cups
  $(call Package/cups/Default)
  SECTION:=net
  CATEGORY:=Network
  DEPENDS+=+libusb-1.0 +libstdcpp
  TITLE:=Common UNIX Printing System (daemon)
endef

define Package/cups/description
  Common UNIX Printing System (daemon)
endef

define Package/cups/conffiles
/etc/cups/classes.conf
/etc/cups/cupsd.conf
/etc/cups/printers.conf
endef

define Package/cups-bsd
  $(call Package/cups/Default)
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Common UNIX Printing System - BSD commands (old)
endef

define Package/cups-client
$(call Package/cups/Default)
  SECTION:=net
  CATEGORY:=Network
  DEPENDS+=+libcupsimage
  TITLE:=Common UNIX Printing System - Client commands
endef

define Package/cups-client/conffiles
/etc/cups/client.conf
endef

define Package/cups-raster-filters
  $(call Package/cups/Default)
  SECTION:=net
  CATEGORY:=Network
  DEPENDS+=+libcupsimage
  TITLE:=Common UNIX Printing System - raster filters (cups 自带)
endef

define Package/cups-ppdc
  $(call Package/cups/Default)
  SECTION:=net
  CATEGORY:=Network
  DEPENDS+=+libstdcpp
  TITLE:=Common UNIX Printing System - PPDC utils
endef

define Package/cups-locale-zh
$(call Package/cups/Default)
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Common UNIX Printing System - Locale zh
endef


CONFIGURE_ARGS+= \
	--with-cups-user="nobody" \
	--with-cups-group="nogroup" \
	--with-system-groups="root" \
	--enable-default-shared \
	--enable-shared \
	--enable-libusb \
	--disable-acl \
	--disable-dnssd \
	--disable-dbus \
	--disable-avahi \
	--disable-launchd \
	--disable-pam \
	--disable-slp \
	--disable-gnutls \
	--disable-cdsassl \
	--disable-ssl \
	--disable-gssapi

define Package/cups/install
	$(INSTALL_DIR) $(1)/etc/cups
	$(CP) $(PKG_INSTALL_DIR)/etc/cups/* $(1)/etc/cups/
	sed -i -e 's/^User root/User lp/' -e 's/^Group root/Group lp/' \
		-e 's/^# User lp/User lp/' -e 's/^# Group lp/Group lp/' \
		$(1)/etc/cups/cups-files.conf
	$(LN) /tmp $(1)/etc/cups/certs

	$(INSTALL_DIR) $(1)/usr/lib/cups
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/cups/backend $(1)/usr/lib/cups
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/cups/cgi-bin $(1)/usr/lib/cups
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/cups/daemon $(1)/usr/lib/cups
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/cups/driver $(1)/usr/lib/cups
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/cups/monitor $(1)/usr/lib/cups

	# 过滤器必须 0755: cupsd 以 lp 用户跑过滤链, 源码树里装出来是 754 → execv Permission denied(113)
	# 用 INSTALL_BIN(install -m0755) 在安装时把权限写进去, 不依赖源码模式、也不用事后 chmod
	$(INSTALL_DIR) $(1)/usr/lib/cups/filter
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/commandtops $(1)/usr/lib/cups/filter/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/pstops      $(1)/usr/lib/cups/filter/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/gziptoany  $(1)/usr/lib/cups/filter/

	$(INSTALL_DIR) $(1)/usr/share/cups/templates
	$(CP) $(PKG_INSTALL_DIR)/usr/share/cups/templates/*.tmpl \
		$(1)/usr/share/cups/templates/

	$(INSTALL_DIR) $(1)/usr/share/cups/mime
	$(CP) $(PKG_INSTALL_DIR)/usr/share/cups/mime/* $(1)/usr/share/cups/mime/

	### Do install docs: cups web interface relies on them
	$(INSTALL_DIR) $(1)/usr/share/doc/cups
	$(CP) $(PKG_INSTALL_DIR)/usr/share/doc/cups/*.*html \
		$(1)/usr/share/doc/cups/
	$(CP) $(PKG_INSTALL_DIR)/usr/share/doc/cups/*.css \
		$(1)/usr/share/doc/cups/
	$(CP) $(PKG_INSTALL_DIR)/usr/share/doc/cups/*.txt \
		$(1)/usr/share/doc/cups/
	$(CP) $(PKG_INSTALL_DIR)/usr/share/doc/cups/images \
		$(1)/usr/share/doc/cups/

	$(INSTALL_DIR) $(1)/usr/sbin
	# overwrite default config with our own
	$(CP) ./files/etc/cups/* $(1)/etc/cups/

	# install initscript with priority 60
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/cupsd.init $(1)/etc/init.d/cupsd

	# CUPS 2.x 需要非特权账号 lp（OpenWrt 默认没有），首次安装在 uci-defaults 里补
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/92-cups-lp-user $(1)/etc/uci-defaults/
	$(INSTALL_BIN) \
		$(PKG_INSTALL_DIR)/usr/sbin/{cupsctl,cupsd} \
		$(1)/usr/sbin/

	# needed for cups to find usb printers per http://wiki.openwrt.org/doc/howto/cups.server
	chmod 700 $(1)/usr/lib/cups/backend/usb
endef

define Package/cups-bsd/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/{lprm,lpq,lpr} $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/lpc $(1)/usr/sbin/
endef

define Package/cups-client/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) \
		$(PKG_INSTALL_DIR)/usr/bin/{lp,cancel,cupstestppd} \
		$(PKG_INSTALL_DIR)/usr/bin/{ipptool,lpoptions,lpstat} \
		$(1)/usr/bin/

	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) \
		$(PKG_INSTALL_DIR)/usr/sbin/{cupsaccept,cupsfilter} \
		$(PKG_INSTALL_DIR)/usr/sbin/{lpadmin,lpinfo,lpmove} \
		$(1)/usr/sbin/
	$(LN) cupsaccept $(1)/usr/sbin/cupsenable
	$(LN) cupsaccept $(1)/usr/sbin/cupsdisable
	$(LN) cupsaccept $(1)/usr/sbin/cupsreject
endef

# 注意: 这里只是 cups 2.3 自带的 4 个 raster 过滤器 (rastertoepson/hp/label/pwg)。
# 真正的 OpenPrinting cups-filters (pdftops/foomatic-rip/imagetopdf) 是独立包
# package/21.02-other-package/cups-filters —— 两者包名必须不同, 否则和当初
# libcups 一样会撞名。此处改名 cups-raster-filters。
define Package/cups-raster-filters/install
	$(INSTALL_DIR) $(1)/usr/lib/cups/filter
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/rastertoepson $(1)/usr/lib/cups/filter/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/rastertohp    $(1)/usr/lib/cups/filter/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/rastertolabel $(1)/usr/lib/cups/filter/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/rastertopwg   $(1)/usr/lib/cups/filter/
	$(LN) rastertolabel $(1)/usr/lib/cups/filter/rastertodymo
endef

define Package/cups-ppdc/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) \
		$(PKG_INSTALL_DIR)/usr/bin/{ppdc,ppdhtml,ppdi,ppdmerge,ppdpo} \
		$(1)/usr/bin/
endef

define Package/cups-locale-zh/install
	$(INSTALL_DIR) $(1)/usr/share/locale/zh_CN
	$(CP) $(PKG_INSTALL_DIR)/usr/share/locale/zh_CN/* $(1)/usr/share/locale/zh_CN
endef

# 这里故意不装头文件/库: CUPS 的 cups/*.h 与 libcups*.so 由 feeds/packages/libs/libcups
# 单一提供。两个包都往 staging 写同一批文件时, 谁的 CleanStaging 一跑就会把对方正在用的
# 头文件删掉 (实测: foo2zjs 编译时报 cups/sidechannel.h not found)。
# 保留这个宏只是为了 package.mk 把 compile 依赖到 STAMP_INSTALLED。
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/lib/cups
endef

$(eval $(call BuildPackage,cups))
$(eval $(call BuildPackage,cups-bsd))
$(eval $(call BuildPackage,cups-client))
$(eval $(call BuildPackage,cups-raster-filters))
$(eval $(call BuildPackage,cups-ppdc))
$(eval $(call BuildPackage,cups-locale-zh))
