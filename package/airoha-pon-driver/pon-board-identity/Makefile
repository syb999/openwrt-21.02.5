# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=pon-board-identity
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/pon-board-identity
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=PON
  TITLE:=PON board identity editor
  DEPENDS:=+jshn +jsonfilter +mtd +nand-utils +ubi-utils
endef

define Package/pon-board-identity/description
 Reads and writes the factory PON identity fields (PON serial number, board MAC)
 declared by /etc/board.json. Requires the board.d layout provided by
 target/linux/airoha/base-files/etc/board.d/03_pon_data.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -Werror \
		-o $(PKG_BUILD_DIR)/pon-identity-image $(PKG_BUILD_DIR)/identity-image.c
endef

define Package/pon-board-identity/install
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/pon-identity-image $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/pon-board-identity $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-pon-data $(1)/usr/libexec/
endef

$(eval $(call BuildPackage,pon-board-identity))
