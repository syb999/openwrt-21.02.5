#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
# Rockchip rkbin bootloader fragments for RK3528.
#
# The upstream rkbin repository is a multi-hundred-MB blob repository, so the
# files that are actually needed are vendored under files/ instead:
#
#   Source: https://github.com/rockchip-linux/rkbin
#   Commit: 12660714c81be85350a4092542e2ff599aa5adcb
#
#   bin/rk35/rk3528_bl31_v1.17.elf         (ATF for u-boot.itb)
#   bin/rk35/rk3528_ddr_1056MHz_v1.10.bin  (TPL)
#   bin/rk35/rk3528_spl_v1.05.bin          (loader FlashBoot)
#   bin/rk35/rk3528_usbplug_v1.03.bin      (loader usbplug)
#   RKBOOT/RK3528MINIALL.ini               (recipe for tools/boot_merger)
#   tools/boot_merger                      (Rockchip packer, x86_64)
#
# boot_merger turns DDR + usbplug + spl into a maskrom loader, which has to be
# pushed into RAM over USB in maskrom mode before any storage can be written
# (rkdeveloptool db / upgrade_tool DB / RKDevTool "download loader").
#
# Two containers are produced on purpose:
#   rk3528_spl_loader_*.bin   new "LDR " container with an embedded RKNS idblock
#                             (what Rockchip's Windows RKDevTool expects)
#   rk3528_loader_miniall.bin classic "BOOT" container
#                             (what the Linux rkdeveloptool/upgrade_tool accept:
#                              they verify the "BOOT" tag and their own CRC_32 -
#                              they reject the "LDR " tag outright)

include $(TOPDIR)/rules.mk

PKG_NAME:=rkbin
PKG_RELEASE:=3

PKG_LICENSE:=Proprietary
PKG_MAINTAINER:=Tianling Shen <cnsztl@immortalwrt.org>

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/files/. $(PKG_BUILD_DIR)/
	chmod +x $(PKG_BUILD_DIR)/tools/boot_merger
endef

define Build/Compile
	@echo "Packing maskrom loaders for $(BUILD_VARIANT)"
	cd $(PKG_BUILD_DIR) && ./tools/boot_merger $(if $(LOADER_INI),$(LOADER_INI),RKBOOT/$(shell echo $(BUILD_VARIANT) | tr a-z A-Z)MINIALL.ini) >/dev/null
ifneq ($(LOADER_LEGACY_INI),)
	cd $(PKG_BUILD_DIR) && ./tools/boot_merger $(LOADER_LEGACY_INI) >/dev/null
endif
endef

define rkbin/rk3528
  DEPENDS:=@TARGET_rockchip_armv8
  ATF:=rk35/rk3528_bl31_v1.17.elf
  TPL:=rk35/rk3528_ddr_1056MHz_v1.10.bin
  LOADER_INI:=RKBOOT/RK3528MINIALL.ini
  LOADER:=rk3528_spl_loader_v1.10.105.bin
  LOADER_LEGACY_INI:=RKBOOT/RK3528MINIALL-legacy.ini
  LOADER_LEGACY:=rk3528_loader_miniall.bin
endef

define Build/rkbin/Target
  $(eval $(call rkbin/$(1)))

  define Package/rkbin-$(1)
    SECTION:=boot
    CATEGORY:=Boot Loaders
    TITLE:=Rockchip rkbin for the $(1) SoC
    DEPENDS:=$(DEPENDS)
    VARIANT:=$(1)
  endef

  define Package/rkbin-$(1)/install
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(CP) $(CURDIR)/files/bin/$(ATF) $(STAGING_DIR_IMAGE)/
	$(CP) $(CURDIR)/files/bin/$(TPL) $(STAGING_DIR_IMAGE)/
ifneq ($(LOADER),)
	$(CP) $(PKG_BUILD_DIR)/$(LOADER) $(STAGING_DIR_IMAGE)/
endif
ifneq ($(LOADER_LEGACY),)
	$(CP) $(PKG_BUILD_DIR)/$(LOADER_LEGACY) $(STAGING_DIR_IMAGE)/
endif
  endef
endef

VARIANTS:=rk3528

define BuildPackage/rkbin
  $(foreach type,$(if $(DUMP),$(VARIANTS),$(BUILD_VARIANT)), \
    $(eval $(call Build/rkbin/Target,$(type))) \
  )
  $(eval $(call Build/DefaultTargets))
  $(foreach type,$(if $(DUMP),$(VARIANTS),$(BUILD_VARIANT)), \
    $(call BuildPackage,rkbin-$(type)) \
  )
endef

$(eval $(call BuildPackage/rkbin))
