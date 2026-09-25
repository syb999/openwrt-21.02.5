#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
# U-Boot 2026.07 for RK3566/RK3568 boards.
#
# Kept separate from package/boot/uboot-rockchip (2021.01) on purpose: that one
# is used by the existing RK3328/RK3399 devices and cannot build RK356x, while
# 2026.07 needs the newer include/u-boot.mk hooks (UBOOT_CUSTOMIZE_CONFIG,
# ROCKCHIP_TPL).
#
# The RK3566 and RK3568 differ only in the DDR init blob and in the peripheral
# set, so the single upstream "generic-rk3568" target is used: U-Boot's
# generic DT (rk3568-generic.dts) is explicitly documented as
# "Minimal generic DT for RK3566/RK3568 with eMMC, SD-card, SPI flash and USB
# OTG enabled" and already uses serial2 at 1500000n8, which is exactly what the
# Panther X2 uses for its console.  No board DT patches are needed.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_VERSION:=2026.07
PKG_RELEASE:=1

PKG_HASH:=78e8bfc382fe388f9b55aa1daf8c563522a037779b5d4c349d1415e381f1243e

PKG_MAINTAINER:=Tobias Maedel <openwrt@tbspace.de>

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk

define U-Boot/Default
  BUILD_TARGET:=rockchip
  UENV:=default
  HIDDEN:=1
endef


# RK3566 boards

define U-Boot/rk3566/Default
  BUILD_SUBTARGET:=armv8
  DEPENDS:=+PACKAGE_u-boot-$(1):rkbin-rk3566
  ATF:=rk3568_bl31_v1.46.elf
  TPL:=rk3566_ddr_1056MHz_v1.26.bin
endef

define U-Boot/generic-rk3568
  $(U-Boot/rk3566/Default)
  NAME:=GENERIC RK3566
  BUILD_DEVICES:= \
    panther_x2
endef

UBOOT_TARGETS := \
  generic-rk3568

UBOOT_CONFIGURE_VARS += USE_PRIVATE_LIBGCC=yes

UBOOT_CUSTOMIZE_CONFIG := \
	--disable TOOLS_MKEFICAPSULE \
	--set-str MKIMAGE_DTC_PATH $(PKG_BUILD_DIR)/scripts/dtc/dtc \
	--enable CMD_EXT4_WRITE \
	--set-val EXT4_MAX_JOURNAL_ENTRIES 100

# CMD_EXT4_WRITE lets the boot script drop breadcrumb files into the ext4 boot
# partition; the generic RK356x board has its console on UART2, which most
# Panther X2 units do not expose, so the same headless diagnostics that the
# H28K port uses stay available here.

UBOOT_MAKE_FLAGS += \
  BL31=$(STAGING_DIR_IMAGE)/$(ATF) \
  PATH=$(STAGING_DIR_HOST)/bin:$(PATH) \
  DTC="$(PKG_BUILD_DIR)/scripts/dtc/dtc" \
  $(if $(TPL),ROCKCHIP_TPL=$(STAGING_DIR_IMAGE)/$(TPL))

# DTC is deliberately overridden above: include/u-boot.mk points it at the
# kernel's dtc (1.5.0 on this tree), whose check list and output differ from
# the dtc shipped inside U-Boot itself.  The later assignment on the
# make command line wins, so this one takes effect.

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(CP) $(PKG_BUILD_DIR)/idbloader.img $(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-idbloader.img
	$(CP) $(PKG_BUILD_DIR)/u-boot.itb $(STAGING_DIR_IMAGE)/$(BUILD_VARIANT)-u-boot.itb
endef

define Package/u-boot/install/default
endef

$(eval $(call BuildPackage/U-Boot))
