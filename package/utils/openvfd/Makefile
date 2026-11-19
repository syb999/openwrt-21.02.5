# SPDX-License-Identifier: GPL-2.0-only
#
# openvfd - userspace daemon for the front panel display of Android TV boxes.
#
# The kernel half is drivers/misc/openvfd (target files overlay, kmod-openvfd);
# this package installs the upstream OpenVFDService (same GPL-2.0 project,
# github.com/arthur-liberman/linux_openvfd) as /usr/sbin/openvfd.  Without
# arguments it shows the clock, which is what the stock Android firmware does
# on these boxes.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=openvfd
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0
PKG_MAINTAINER:=local

include $(INCLUDE_DIR)/package.mk

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

define Package/openvfd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=OpenVFD front panel display daemon
  DEPENDS:=+kmod-openvfd
endef

define Package/openvfd/description
 Userspace daemon that drives the OpenVFD character device (/dev/openvfd) and
 keeps the box's front panel display showing the time.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)/src/driver
	$(CP) ./src/OpenVFDService.c $(PKG_BUILD_DIR)/src/
	$(CP) ./src/driver/openvfd_drv.h $(PKG_BUILD_DIR)/src/driver/
	$(CP) ./src/driver/glyphs.h $(PKG_BUILD_DIR)/src/driver/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -I$(PKG_BUILD_DIR)/src \
		-o $(PKG_BUILD_DIR)/openvfd $(PKG_BUILD_DIR)/src/OpenVFDService.c \
		$(TARGET_LDFLAGS) -lpthread
endef

define Package/openvfd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/openvfd $(1)/usr/sbin/openvfd
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/openvfd.init $(1)/etc/init.d/openvfd
	$(INSTALL_DIR) $(1)/etc/rc.d
	$(LN) ../init.d/openvfd $(1)/etc/rc.d/S95openvfd
endef

$(eval $(call BuildPackage,openvfd))
