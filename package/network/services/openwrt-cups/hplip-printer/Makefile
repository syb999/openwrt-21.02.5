#
# 🔴 硬阻塞（2026-10 实测，别重走）：hpcups 链接需要 libImageProcessor，
#    而 hplip 源码里**只有预编译的 x86 版本**（prnt/hpcups/libImageProcessor-x86_64.so /
#    -x86_32.so，无 arm 版本、也没有源码），交叉编译 aarch64 时 ld 报
#    "skipping incompatible .../libImageProcessor.so" + "cannot find -lImageProcessor"。
#    ⇒ 在 aarch64 OpenWrt 上 hpcups 编不出来。HP LaserJet Pro P1106/P1102 请优先试
#    foo2zjs 自带的 HP-LaserJet_Pro_P1102.ppd（同引擎家族），或换 IPP/其它路径。
#    当前包默认不选（.config 里 is not set），保留代码供以后找到 arm 版 ImageProcessor 时用。
#
# hplip-printer 3.21.2 — 只取 hplip 的**打印**部分 (hpcups filter + PPD)
#
# 为什么要单独一个包: feeds/packages/utils/hplip 编的是 lite + SANE only
# (--disable-hpcups-install --disable-cups-drv-install --enable-lite-build),
# 里面**没有**打印驱动。共享包不动, 在本地树另开一个只编打印机部分的包。
#
# 用途: HP host-based 激光机且 foo2zjs 不支持的型号, 例如
#       HP LaserJet Pro P1102 / P1106 / P1107 / P1108 (models.dat 里 plugin=0, 不需要 HP 闭源插件)
# 链路: 客户端 PDF -> [cups-filters pdftopdf/gstoraster] -> CUPS Raster -> hpcups -> 打印机
#       ⇒ 依赖 gs 的 cups 设备 (已在 ghostscript 包里打开)
#
include $(TOPDIR)/rules.mk

PKG_NAME:=hplip-printer
PKG_VERSION:=3.21.2
PKG_RELEASE:=1

PKG_SOURCE:=hplip-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=@SF/hplip
PKG_HASH:=410421a13e62205d41bacd3215993c89e513fb4d7fab07e23e2720465aea7c41

PKG_LICENSE:=GPL-2.0-or-later
PKG_LICENSE_FILES:=COPYING
PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1
PKG_FIXUP:=autoreconf
PKG_BUILD_DEPENDS:=libcups

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

# tarball 顶层目录是 hplip-<ver>, 与我们自己的 BUILD_DIR 名字不同
PKG_UNPACK:=$(HOST_TAR) -C $(PKG_BUILD_DIR) --strip-components=1 -xzf $(DL_DIR)/$(PKG_SOURCE)

CONFIGURE_ARGS += \
	--disable-gui-build \
	--disable-network-build \
	--disable-fax-build \
	--disable-pp-build \
	--disable-doc-build \
	--disable-dbus-build \
	--enable-hpcups-only-build \
	--disable-hpijs-only-build \
	--enable-hpcups-install \
	--disable-hpps-install \
	--disable-cups-drv-install

define Package/hplip-printer
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=HPLIP printing part (hpcups filter + PPDs)
  URL:=https://developers.hp.com/hp-linux-imaging-and-printing
  DEPENDS:=+libcups +libjpeg-turbo +libstdcpp
endef

define Package/hplip-printer/description
  HPLIP 的打印部分：hpcups 过滤器 + HP 官方 PPD 集（817 个，约 2.9MB）。
  用于 foo2zjs 不覆盖的 HP host-based 机型（如 LaserJet Pro P1106/P1102）。
  需要 ghostscript 的 cups 设备 + cups-filters 提供 CUPS Raster 输入。
endef

define Package/hplip-printer/install
	$(INSTALL_DIR) $(1)/usr/lib/cups/filter
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/cups/filter/hpcups $(1)/usr/lib/cups/filter/

	if [ -d $(PKG_INSTALL_DIR)/usr/share/ppd/HP ]; then \
		$(INSTALL_DIR) $(1)/usr/share/ppd/hp; \
		$(CP) $(PKG_INSTALL_DIR)/usr/share/ppd/HP/*.ppd.gz $(1)/usr/share/ppd/hp/; \
	elif [ -d $(PKG_INSTALL_DIR)/usr/share/cups/model/HP ]; then \
		$(INSTALL_DIR) $(1)/usr/share/ppd/hp; \
		$(CP) $(PKG_INSTALL_DIR)/usr/share/cups/model/HP/*.ppd.gz $(1)/usr/share/ppd/hp/; \
	fi
endef

$(eval $(call BuildPackage,hplip-printer))
