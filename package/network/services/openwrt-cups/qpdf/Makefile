#
# qpdf 10.6.3 — cups-filters 的硬依赖 (configure.ac 里 PKG_CHECK_MODULES([LIBQPDF],
# [libqpdf >= 8.3.0]) 是无条件的, 没有关掉的开关)。pdftoraster 用它解析 PDF。
# 只打包库和开发文件; 命令行工具 qpdf 不装 (省体积)。
#
include $(TOPDIR)/rules.mk

PKG_NAME:=qpdf
PKG_VERSION:=10.6.3
PKG_RELEASE:=1

PKG_SOURCE:=qpdf-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=@SF/qpdf/qpdf/$(PKG_VERSION)/
PKG_HASH:=e8fc23b2a584ea68c963a897515d3eb3129186741dd19d13c86d31fa33493811

PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=LICENSE.txt
PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk

CONFIGURE_ARGS += \
	--disable-static \
	--enable-shared \
	--disable-crypto-openssl \
	--disable-crypto-gnutls \
	--disable-werror \
	--disable-doc-maintenance

TARGET_CXXFLAGS += -std=c++14

# qpdf 的 configure 用 AC_CHECK_FILE(/dev/urandom), 交叉编译时会直接 error
CONFIGURE_VARS += ac_cv_file__dev_urandom=yes

define Package/libqpdf
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=QPDF library (PDF transformation/parsing)
  URL:=https://qpdf.sourceforge.io/
  DEPENDS:=+zlib +libjpeg-turbo +libstdcpp +libatomic
endef

define Package/libqpdf/description
  libqpdf — PDF 结构解析/改写库。cups-filters 的 pdftoraster 依赖它。
endef

define Package/libqpdf/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libqpdf.so.* $(1)/usr/lib/
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_INSTALL_DIR)/usr/include/qpdf $(1)/usr/include/

	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/libqpdf.so* $(1)/usr/lib/

	$(INSTALL_DIR) $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/libqpdf.pc $(1)/usr/lib/pkgconfig/
endef

$(eval $(call BuildPackage,libqpdf))
