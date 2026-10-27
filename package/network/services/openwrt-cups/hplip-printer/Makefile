#
# 🔴 ARM/MIPS 上编不出来（2026-10 实测 + 符号表核实）：hpcups 的 hpcups_LDADD 里
#    硬编码 -lImageProcessor，而 hplip 只带预编译的 x86 版本
#    （prnt/hpcups/libImageProcessor-x86_64.so / -x86_32.so，libImageProcessor.so 是
#    指向 x86_64 的符号链接），既无 ARM 版也无源码 ⇒ 交叉链接报
#      ./prnt/hpcups//libImageProcessor.so: file format not recognized
#      collect2: error: ld returned 1 exit status
#    （ip/*.c 编出来的是 libhpip，导出符号名不同，替不了。）
#    它提供的 5 个符号 imageProcessorCreate/StartPage/ProcessLine/EndPage/Destroy
#    只有 HPCupsFilter.cpp 调用，且都在
#      if (strncmp(m_JA.printer_platform, "ljzjstream", 10) == 0) 里 ⇒ 只影响 ZjStream
#    机型（P1005/P1006/P1102/P1106 这一系），这些机型请用本树的 foo2zjs 包。
#    ⇒ 本包已用 DEPENDS 限制为 @(TARGET_x86||TARGET_x86_64)，非 x86 平台不可选。
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
  # 只允许在 x86 上选：hpcups 链接时需要 -lImageProcessor，而 hplip 源码里只有
  # 预编译的 x86 版本（prnt/hpcups/libImageProcessor-x86_64.so / -x86_32.so），
  # 既没有 ARM 版也没有源码。ARM 上交叉链接会拿到 x86 ELF ⇒
  #   ./prnt/hpcups//libImageProcessor.so: file format not recognized
  #   collect2: error: ld returned 1 exit status
  # （HPLIP 里 ip/*.c 编出来的是另一个库 libhpip，导出的符号名不同，顶不上。）
  # 而且这 5 个符号 imageProcessorCreate/StartPage/ProcessLine/EndPage/Destroy
  # 在全部源码里只有 prnt/hpcups/HPCupsFilter.cpp 调用，并且都被
  #   if (strncmp(m_JA.printer_platform, "ljzjstream", 10) == 0)
  # 包着 —— 只有 ZjStream 机型（P1005/P1006/P1102/P1106 这一系）才走。
  # 这些机型在 OpenWrt 上要用 foo2zjs（本树 package/network/services/openwrt-cups/foo2zjs），
  # 不需要 hplip。所以这里直接把包限制在 x86/x86_64，ARM 上不可选 ⇒ 不会被误开。
  DEPENDS:=@(TARGET_x86||TARGET_x86_64) +libcups +libjpeg-turbo +libstdcpp
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
