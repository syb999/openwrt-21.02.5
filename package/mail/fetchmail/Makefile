#
# Copyright (C) 2006 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
# $Id$

include $(TOPDIR)/rules.mk

PKG_NAME:=fetchmail
PKG_VERSION:=6.4.37
PKG_RELEASE:=1

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.xz
PKG_SOURCE_URL:=@SF/$(PKG_NAME)
PKG_MIRROR_HASH:=4a182e5d893e9abe6ac37ae71e542651fce6d606234fc735c2aaae18657e69ea
PKG_HASH:=4a182e5d893e9abe6ac37ae71e542651fce6d606234fc735c2aaae18657e69ea
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/fetchmail
  SECTION:=mail
  CATEGORY:=Mail
  TITLE:=Remote mail retriever
  DEPENDS:=+libopenssl +libpcre +libbz2 +procmail
endef

define Package/fetchmail/description
  Retrieves remote mail via POP/IMAP.
  Very useful in conjunction with mutt.
endef

define Build/Configure
	$(call Build/Configure/Default, \
		--enable-fallback=procmail \
		--without-hesiod \
		, \
		ac_cv_path_procmail=/usr/bin/procmail \
	)
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		DESTDIR="$(PKG_INSTALL_DIR)" \
		all install
endef

define Package/fetchmail/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/fetchmail $(1)/usr/bin/
	$(INSTALL_BIN) ./files/process_mail $(1)/usr/bin/
	$(INSTALL_BIN) ./files/fetchmail_set $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(CP) ./files/fetchmailrc $(1)/etc
	$(INSTALL_BIN) ./files/fetchmail.init $(1)/etc/init.d/fetchmail
endef

$(eval $(call BuildPackage,fetchmail))
