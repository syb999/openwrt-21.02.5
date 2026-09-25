#
# sht2x - read the SHT2x/SHT21 temperature+humidity sensor that sits on the
# Panther X2's mini-PCIe LoRa concentrator card (i2c2, address 0x40).
#
# The sensor is measured from userspace because this kernel has no driver for
# it (mainline only ships sht3x/sht4x, a different protocol).  The package is
# selected by the panther_x2 device only - see target/linux/rockchip/image/armv8.mk.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=sht2x
PKG_RELEASE:=1

PKG_MAINTAINER:=
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/sht2x
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=read the SHT2x temperature/humidity sensor (Panther X2 LoRa card)
  # the i2c core is built into this kernel, no kmod dependency needed
endef

define Package/sht2x/description
  Reads temperature and relative humidity from the Sensirion SHT2x/SHT21 on
  the Panther X2's mini-PCIe LoRa concentrator card (i2c2, address 0x40) and
  prints them, with CRC checking.  Useful to confirm that the add-on card is
  present and powered, since the sensor lives on it.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/sht2x.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -o $(PKG_BUILD_DIR)/sht2x $(PKG_BUILD_DIR)/sht2x.c
endef

define Package/sht2x/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sht2x $(1)/usr/bin/sht2x
endef

$(eval $(call BuildPackage,sht2x))
