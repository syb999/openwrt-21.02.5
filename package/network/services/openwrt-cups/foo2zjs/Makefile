include $(TOPDIR)/rules.mk

PKG_NAME:=foo2zjs
PKG_VERSION:=2021-04-26
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/koenkooi/foo2zjs
PKG_SOURCE_PROTO:=git
PKG_SOURCE_VERSION:=e04290de6b7a30d588f3411fd9834618e09f7b9b
PKG_HASH:=1b670fa2b20bed119d93e79011c041411870924c6e185c2c0d9afc3b829a7bd2
PKG_MIRROR_HASH:=1b670fa2b20bed119d93e79011c041411870924c6e185c2c0d9afc3b829a7bd2

PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

PKG_BUILD_DEPENDS:=libcups

include $(INCLUDE_DIR)/package.mk

define Package/foo2zjs
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=A linux printer driver for ZjStream protocol.
  URL:=http://foo2zjs.rkkda.com/
  DEPENDS:= +libcups
endef

define Package/foo2zjs/description
	A linux printer driver for ZjStream protocol.
endef

define Package/foo2zjs/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(CP) $(PKG_BUILD_DIR)/getweb $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/bin/* $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/* $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/usr/share
	$(CP) $(PKG_INSTALL_DIR)/usr/share/* $(1)/usr/share/

	$(INSTALL_DIR) $(1)/usr/share/ppd/foo2zjs
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/PPD/*.ppd $(1)/usr/share/ppd/foo2zjs/

	$(HOSTCC) $(HOST_CFLAGS) -o $(PKG_BUILD_DIR)/arm2hpdl-host $(PKG_BUILD_DIR)/arm2hpdl.c
	sh ./files/mkfirmware.sh $(PKG_BUILD_DIR) $(1)/usr/share/foo2zjs/firmware $(1)/usr/share/foo2xqx/firmware $(PKG_BUILD_DIR)/arm2hpdl-host
endef

$(eval $(call BuildPackage,foo2zjs))
