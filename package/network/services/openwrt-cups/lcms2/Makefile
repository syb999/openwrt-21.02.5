#
# Little CMS 2 — cups-filters 的硬依赖 (configure 会 error 退出, 没有关掉的开关)
#
include $(TOPDIR)/rules.mk

PKG_NAME:=lcms2
PKG_VERSION:=2.12
PKG_RELEASE:=1

PKG_SOURCE:=lcms2-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/mm2/Little-CMS/releases/download/lcms2.$(PKG_VERSION)/
PKG_HASH:=18663985e864100455ac3e507625c438c3710354d85e5cbb7cd4043e11fe10f5

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=COPYING
PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk

define Package/liblcms2
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Little CMS color management library
  URL:=https://www.littlecms.com/
endef

define Package/liblcms2/description
  Little CMS 2 (lcms2) 色彩管理库。cups-filters 的 configure 强制要求它。
endef

define Package/liblcms2/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/liblcms2.so.* $(1)/usr/lib/
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_INSTALL_DIR)/usr/include/lcms2*.h $(1)/usr/include/

	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/liblcms2.so* $(1)/usr/lib/

	$(INSTALL_DIR) $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/lcms2.pc $(1)/usr/lib/pkgconfig/
endef

$(eval $(call BuildPackage,liblcms2))
