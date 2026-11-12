include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-xlsx2sqlite
PKG_VERSION:=1.2.9
PKG_RELEASE:=1


PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

LUCI_COMPAT:=$(if $(wildcard $(TOPDIR)/feeds/luci/modules/luci-compat),+luci-compat,)

define Package/luci-app-xlsx2sqlite
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=Excel/CSV/SQL <-> SQLite merge tool (luasrc, LuCI 1.x)
  DEPENDS:=+luci-base +libsqlite3 +zlib $(LUCI_COMPAT)
endef

define Package/luci-app-xlsx2sqlite/description
 Upload Excel/xls/csv/sql files with different column layouts, merge the rows by
 header names into one table (like 1.xlsx), write a sqlite3 database, and export
 the database back to .xlsx / .csv / .sql text.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/src
	$(CP) ./src/* $(PKG_BUILD_DIR)/src/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/src \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		AR="$(TARGET_AR)"
endef

define Package/luci-app-xlsx2sqlite/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/xlsx2sqlite $(1)/usr/bin/xlsx2sqlite
	$(INSTALL_DIR) $(1)/www/cgi-bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/src/xlsx2sqlite $(1)/www/cgi-bin/xlsx2sqlite
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller
	$(INSTALL_DATA) ./luasrc/controller/xlsx2sqlite.lua $(1)/usr/lib/lua/luci/controller/xlsx2sqlite.lua
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/view/xlsx2sqlite
	$(INSTALL_DATA) ./luasrc/view/xlsx2sqlite/main.htm $(1)/usr/lib/lua/luci/view/xlsx2sqlite/main.htm
endef

define Package/luci-app-xlsx2sqlite/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	rm -f /tmp/luci-indexcache*
	rm -rf /tmp/luci-modulecache
	killall -HUP rpcd 2>/dev/null
	exit 0
}
endef

$(eval $(call BuildPackage,luci-app-xlsx2sqlite))
