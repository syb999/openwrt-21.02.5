#
# Ghostscript 9.55.0 — CUPS 打印链的 PDF/PS -> 位图/打印语言 引擎
#
# 这是在 OpenWrt 上支持「路由器侧驱动打印」的关键组件:
#   - cups-filters 的 gstoraster/pdftops 都靠它把 PDF/PS 转成 CUPS raster 或位图
#   - foomatic-rip 里所有 gs 类驱动 (pxlmono/pxlcolor/epson/ljet4...) 也靠它
#   - foo2zjs 的 wrapper 脚本内部直接调 gs
# 版本刻意与 Linux Mint / Ubuntu jammy 的 9.55.0 对齐, 便于对照行为。
#
# 第三方库用源码包内自带的 (freetype/jpeg/libpng/tiff/openjpeg/lcms2mt/expat),
# 不额外增加固件里的共享库依赖。
#
include $(TOPDIR)/rules.mk

PKG_NAME:=ghostscript
PKG_VERSION:=9.55.0
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/ArtifexSoftware/ghostpdl-downloads/releases/download/gs9550/
PKG_HASH:=31e2064be67e15b478a8da007d96d6cd4d2bee253e5be220703a225f7f79a70b

PKG_LICENSE:=AGPL-3.0-or-later
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

# 只编打印/栅格化真正用得到的设备, 全量设备会让体积翻好几倍
# 名称全部来自 gs 9.55 源码里实际存在的设备 (devices/*.c grep gs_<name>_device 核对过):
#   epson/escp/eps9high = 9 针 ESC/P, lq850 = 24 针 ESC/P2 (老 Epson 针打)
#   pxlmono/pxlcolor = PCL6, pcl3 = PCL3, ljet4 = HP PCL4
#   cups = CUPS raster 设备 (cups-filters 的 gstoraster 要用)
# 设备选择: 用 ALL。
# 试过只列需要的设备, 但 gs 9.55 的 PDF 解释器 (BUILD_PDF=1) 无条件编
# devices/gdevpdfocr.c (ocr/hocr/pdfocr* 设备), 它依赖 pdfimage 设备的
# gdevpdfimg.o; 而 pdfimage* 不在 --with-drivers 的别名表里(列了也不认),
# 结果链接期 pdf_image_* 全是 undefined。用 ALL 最省事, 体积代价可接受。
GS_DRIVERS:=ALL

# 打开 cups 设备 (hpcups/brlaser/escpr 这类吃 CUPS raster 的驱动, 以及 cups-filters 的
# gstoraster, 都要 -sDEVICE=cups)。用 staging 里的 cups-config 取 cflags/libs,
# 否则 gs 会去用宿主机的 cups-config (交叉编译时被识别并丢弃)。
CONFIGURE_VARS += CUPSCONFIG=$(STAGING_DIR)/usr/bin/cups-config

CONFIGURE_ARGS += \
	--disable-gtk \
	--without-x \
	--disable-dbus \
	--disable-fontconfig \
	--without-ijs \
	--with-drivers=$(GS_DRIVERS)

define Package/ghostscript
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Ghostscript (PostScript/PDF interpreter, printing devices)
  URL:=https://www.ghostscript.com/
  DEPENDS:=+zlib +libpng +libjpeg-turbo +libstdcpp +libcups
endef

define Package/ghostscript/description
  PostScript/PDF 解释器。CUPS 在路由器侧出驱动时靠它把 PDF/PS
  转成打印语言或 CUPS raster (cups-filters 的 gstoraster/pdftops,
  foomatic-rip 的 gs 类驱动, foo2zjs 的 wrapper 都依赖它)。
endef

define Package/ghostscript/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/gs $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/usr/share/ghostscript/$(PKG_VERSION)
	$(CP) $(PKG_INSTALL_DIR)/usr/share/ghostscript/$(PKG_VERSION)/lib $(1)/usr/share/ghostscript/$(PKG_VERSION)/

	# 🔴 install-data 的目标名是 `install-resdata$(COMPILE_INITS)` — 编译进 init 后
	# 这个变量为空 ⇒ 上游 make install **完全不装 Resource/**, 结果板子上没有字体、
	# 没有 CMap ⇒ 中文 PDF (CID 字体) 直接渲染失败。这里手工补上。
	$(INSTALL_DIR) $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource
	$(CP) $(PKG_BUILD_DIR)/Resource/Font $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/Encoding $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/Decoding $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/ColorSpace $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/CMap $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/CIDFont $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/CIDFSubst $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/SubstCID $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
	$(CP) $(PKG_BUILD_DIR)/Resource/IdiomSet $(1)/usr/share/ghostscript/$(PKG_VERSION)/Resource/
endef

$(eval $(call BuildPackage,ghostscript))
