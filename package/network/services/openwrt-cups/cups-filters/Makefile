#
# cups-filters 1.28.15 — OpenPrinting 的 CUPS 过滤器集
#
# 只编打印链真正需要的部分 (与 OpenWrt 体积/依赖权衡):
#   pdftops      PDF -> PostScript   (用 ghostscript 的 ps2write, 不引 poppler)
#   foomatic-rip 读 PPD 里的 FoomaticRIPCommandLine 去驱动 gs 设备
#   imagetopdf   JPEG/PNG 图片 -> PDF (客户端直接发图片时用)
#   texttopdf    纯文本 -> PDF
# 关掉: poppler / mutool / dbus / avahi / ldap / braille / pclm / driverless
#
include $(TOPDIR)/rules.mk

PKG_NAME:=cups-filters
PKG_VERSION:=1.28.15
PKG_RELEASE:=2

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/OpenPrinting/cups-filters/releases/download/$(PKG_VERSION)/
PKG_HASH:=614e053404bb833f78e1edf077f7759d27e475a212eb9a3bd3eac2cfec8bb6c1

PKG_LICENSE:=GPL-2.0-or-later LGPL-2.0-or-later
PKG_LICENSE_FILES:=COPYING
PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1
PKG_BUILD_DEPENDS:=libcups fontconfig qpdf

include $(INCLUDE_DIR)/package.mk

CONFIGURE_ARGS += \
	--disable-dbus \
	--disable-avahi \
	--disable-ldap \
	--disable-poppler \
	--disable-mutool \
	--disable-pclm \
	--disable-braille \
	--disable-driverless \
	--disable-auto-setup-local-only \
	--disable-auto-setup-driverless-only \
	--enable-ghostscript \
	--enable-gs-ps2write \
	--with-pdftops=gs \
	--with-gs-path=/usr/bin/gs \
	--enable-foomatic \
	--enable-imagefilters \
	--with-jpeg \
	--with-png \
	--without-tiff \
	--with-fontdir=/usr/share/fonts \
	--without-php

define Package/cups-filters
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Printing
  TITLE:=OpenPrinting CUPS filters (pdftops / foomatic-rip / imagetopdf)
  URL:=https://github.com/OpenPrinting/cups-filters
  DEPENDS:=+libcups +libcupsimage +libjpeg-turbo +libpng +libstdcpp +libfreetype \
	+liblcms2 +libqpdf +fontconfig +ghostscript
endef

define Package/cups-filters/description
  OpenPrinting 的 CUPS 过滤器集。让 CUPS 能把客户端发来的 PDF
  转成 PostScript 并交给 foomatic-rip/ghostscript 驱动打印机:
    PDF --pdftops--> PS --foomatic-rip--> gs -sDEVICE=<ppd 指定> --> 打印机
  含 imagetopdf (JPEG/PNG 直接打印) 与 texttopdf (纯文本)。
endef

define Package/cups-filters/install
	# 过滤器必须 0755 (cupsd 以 lp 用户跑过滤链)。用 find -exec install -m0755:
	# ① 权限由本 Makefile 显式写入, 不依赖源码里文件的原始 754, 也不用事后 chmod
	# ② 必须是 shell 在运行时求值, 不能用 $(wildcard)(它在 make 解析阶段求值, 那时
	#    PKG_INSTALL_DIR 还是空的 ⇒ 过滤器会一个都不打包, 已踩过)
	$(INSTALL_DIR) $(1)/usr/lib/cups/filter
	find $(PKG_INSTALL_DIR)/usr/lib/cups/filter -type f -exec $(INSTALL_BIN) {} $(1)/usr/lib/cups/filter/ \;

	# cups-filters 自己的两个运行库, 过滤器都靠它们 (漏了会报 missing dependencies)
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libcupsfilters.so.* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libfontembed.so.* $(1)/usr/lib/

	$(INSTALL_DIR) $(1)/usr/share/cups
	# charsets: 文本/texttopdf 需要; data: CUPS 网页的 "Print Test Page" 要 /usr/share/cups/data/testprint
	for d in mime drv charsets data banners; do \
		[ -d $(PKG_INSTALL_DIR)/usr/share/cups/$$d ] || continue; \
		$(INSTALL_DIR) $(1)/usr/share/cups/$$d; \
		$(CP) $(PKG_INSTALL_DIR)/usr/share/cups/$$d/* $(1)/usr/share/cups/$$d/; \
	done

	[ -d $(PKG_INSTALL_DIR)/usr/share/ppd ] && { \
		$(INSTALL_DIR) $(1)/usr/share/ppd; \
		$(CP) $(PKG_INSTALL_DIR)/usr/share/ppd/* $(1)/usr/share/ppd/; } || true

endef

$(eval $(call BuildPackage,cups-filters))
