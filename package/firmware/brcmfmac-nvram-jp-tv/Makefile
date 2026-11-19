# SPDX-License-Identifier: GPL-2.0-only
#
# NVRAM for the BCM4339 (AP6335 combo) SDIO wifi of the JP TV Box 3566.
#
# The .bin itself comes from cypress-firmware-4339-sdio; this package only
# carries the board parameters, which brcmfmac will not guess.  Two copies are
# installed on purpose:
#
#   brcmfmac4339-sdio.jp-tvbox,rk3566.txt   <- what brcmfmac actually asks for,
#                                              because it derives the board type
#                                              from the machine compatible
#   brcmfmac4339-sdio.txt                   <- generic fallback name
#
# Content is the stock AP6335 NVRAM (V1.5, "#AP6335_NVRAM_V1.5_03112014"),
# taken from LibreELEC's brcmfmac_sdio-firmware.  Verified on the board: with
# this file present the driver stops asking for a missing nvram, and the chip
# (reported as BCM4339/2) proceeds past the nvram stage.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=brcmfmac-nvram-jp-tv
PKG_RELEASE:=1

PKG_LICENSE:=Proprietary
PKG_MAINTAINER:=local

include $(INCLUDE_DIR)/package.mk

define Package/brcmfmac-nvram-jp-tv
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=BCM4339 (AP6335) firmware + NVRAM for JP TV Box 3566
  DEPENDS:=+kmod-brcmfmac
endef

define Package/brcmfmac-nvram-jp-tv/description
 Board NVRAM for the AP6335 (BCM4339 wifi + BCM4335 BT) combo module of the
 JP TV Box 3566, installed under both the board-specific and the generic
 brcmfmac firmware name.
endef

define Build/Compile
endef

define Package/brcmfmac-nvram-jp-tv/install
	$(INSTALL_DIR) $(1)/lib/firmware/brcm
	$(INSTALL_DATA) ./files/brcmfmac4339-sdio.bin $(1)/lib/firmware/brcm/
	$(INSTALL_DATA) ./files/brcmfmac4339-sdio.txt $(1)/lib/firmware/brcm/
	$(INSTALL_DATA) "./files/brcmfmac4339-sdio.jp-tvbox,rk3566.txt" $(1)/lib/firmware/brcm/
endef

$(eval $(call BuildPackage,brcmfmac-nvram-jp-tv))
