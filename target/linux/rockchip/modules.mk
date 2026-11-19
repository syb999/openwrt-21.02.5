# SPDX-License-Identifier: GPL-2.0-only
#
# Rockchip target kernel modules.
#
# The vendor video codec (MPP) stack is built as the single loadable module
# rk_vcodec.ko (see patches-5.4/317-rockchip-mpp-drivers.patch and the
# CONFIG_ROCKCHIP_MPP_* settings in armv8/config-5.4).  It is packaged here so
# that it is only *installed* on the devices that ask for it through
# DEVICE_PACKAGES.  The other rockchip boards (rk3328/rk3399/rk3528) share the
# same armv8 subtarget kernel config and therefore compile the module too, but
# they never install or load it, so the codec driver stays completely off their
# systems.  There is no per-device kernel config in OpenWrt 21.02, so packaging
# the =m symbol as a kmod is the only device-level isolation that exists.

define KernelPackage/rk-vcodec
  SUBMENU:=Other modules
  TITLE:=Rockchip MPP video codec service
  DEPENDS:=@TARGET_rockchip
  KCONFIG:=CONFIG_ROCKCHIP_MPP_SERVICE
  FILES:=$(LINUX_DIR)/drivers/video/rockchip/mpp/rk_vcodec.ko
  AUTOLOAD:=$(call AutoLoad,90,rk_vcodec)
endef

define KernelPackage/rk-vcodec/description
 Rockchip vendor Multi Media Platform (MPP) service: the VPU/JPEG codec
 blocks (rkvdec2, rkvenc, vdpu2, vepu2, jpgdec) exposed as /dev/mpp_service.
 Built as a module and installed only by the boards that select it
 (RK3566 Panther X2); it provides the kernel side used by the rockchip-mpp
 userspace library.
endef

$(eval $(call KernelPackage,rk-vcodec))

#
# SPDIF (optical) output - JP TV Box 3566.
#
# The SoC SPDIF transmitter (spdif@fe460000) plus the generic
# simple-audio-card machine driver and the "linux,spdif-dit" dummy codec give
# one ALSA card named "SPDIF" whose only PCM is the optical output.  All three
# are modules so that the other rockchip boards neither build nor load them;
# the driver patch that teaches rockchip_spdif.ko about "rockchip,rk3568-spdif"
# is patches-5.4/336.
#
define KernelPackage/rk-spdif
  SUBMENU:=$(SOUND_MENU)
  TITLE:=Rockchip SPDIF transmitter (rk3568)
  DEPENDS:=@TARGET_rockchip +kmod-sound-soc-core +kmod-sound-soc-spdif
  KCONFIG:= \
	CONFIG_SND_SOC_ROCKCHIP \
	CONFIG_SND_SOC_ROCKCHIP_SPDIF
  FILES:=$(LINUX_DIR)/sound/soc/rockchip/snd-soc-rockchip-spdif.ko
  AUTOLOAD:=$(call AutoLoad,60,snd-soc-rockchip-spdif)
endef

define KernelPackage/rk-spdif/description
 Rockchip SPDIF transmitter as an ASoC CPU DAI (used by the JP TV Box 3566
 optical output together with kmod-sound-simple-card).
endef

$(eval $(call KernelPackage,rk-spdif))

define KernelPackage/rk-simple-card
  SUBMENU:=$(SOUND_MENU)
  TITLE:=Simple Audio Card
  DEPENDS:=@TARGET_rockchip +kmod-sound-soc-core
  KCONFIG:= \
	CONFIG_SND_SIMPLE_CARD \
	CONFIG_SND_SIMPLE_CARD_UTILS
  FILES:= \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card-utils.ko \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card.ko
  AUTOLOAD:=$(call AutoLoad,62,snd-soc-simple-card-utils snd-soc-simple-card)
endef

define KernelPackage/rk-simple-card/description
 Generic simple-audio-card ASoC machine driver, driven by the "spdif-sound"
 node of the board device tree.
endef

$(eval $(call KernelPackage,rk-simple-card))

#
# SATA (AHCI) - JP TV Box 3566.
#
# The box has one SATA port on the SoC's second DWC AHCI controller
# (sata2 @ 0xfc800000 through combphy2).  There is no rockchip-specific AHCI
# driver to port: the stock 5.4 ahci_platform already matches
# "snps,dwc-ahci", so this is only a module package so that the driver is
# built and loaded for the board that has the port, and for no other.
# CONFIG_ATA=m follows from the package's KCONFIG and is what makes libata.ko
# exist; the sibling boards never select this package, so they get neither
# the module nor a kernel symbol.
#
define KernelPackage/rk-ahci
  SUBMENU:=Other modules
  TITLE:=Rockchip DWC AHCI SATA (rk3568 sata2)
  DEPENDS:=@TARGET_rockchip +kmod-ata-core
  KCONFIG:= \
	CONFIG_ATA \
	CONFIG_SATA_AHCI_PLATFORM
  FILES:= \
	$(LINUX_DIR)/drivers/ata/ahci_platform.ko \
	$(LINUX_DIR)/drivers/ata/libahci_platform.ko
  AUTOLOAD:=$(call AutoLoad,41,libahci libahci_platform ahci_platform)
endef

define KernelPackage/rk-ahci/description
 Platform AHCI (snps,dwc-ahci) support for the rk3568 SATA controller, used
 by the JP TV Box 3566 external SATA port.
endef

$(eval $(call KernelPackage,rk-ahci))

#
# I2S/TDM controller - JP TV Box 3566 (reserved I2S header, i2s1@fe410000).
#
# The controller driver (rockchip_i2s_tdm) is vendored by patch 339 because
# this 5.4 tree predates it.  Only the controller is packaged: there is no
# codec/machine driver until one is defined for the header, so no ALSA card
# appears yet - the DAI and the pin muxing are what this provides.
#
define KernelPackage/rk-i2s
  SUBMENU:=$(SOUND_MENU)
  TITLE:=Rockchip I2S/TDM controller (rk3568)
  DEPENDS:=@TARGET_rockchip +kmod-sound-soc-core
  KCONFIG:=CONFIG_SND_SOC_ROCKCHIP_I2S_TDM
  FILES:=$(LINUX_DIR)/sound/soc/rockchip/snd-soc-rockchip-i2s-tdm.ko
  AUTOLOAD:=$(call AutoLoad,58,snd-soc-rockchip-i2s-tdm)
endef

define KernelPackage/rk-i2s/description
 Rockchip I2S/TDM CPU DAI driver (rk3568 i2s1), used by the JP TV Box 3566
 I2S header.
endef

$(eval $(call KernelPackage,rk-i2s))

#
# Front panel display (7-segment clock) - JP TV Box 3566.
#
# The stock firmware drives this small display with its own "digital_dis"
# driver over two GPIOs (gpio0 RK_PB3 clk / RK_PB4 dat).  The OpenVFD driver
# from github.com/arthur-liberman/linux_openvfd is vendored into the target's
# files/drivers/misc/openvfd and hooked in by patch 337; the userspace half is
# the "openvfd" package (upstream OpenVFDService, shows the clock).
#
define KernelPackage/openvfd
  SUBMENU:=Other modules
  TITLE:=OpenVFD front panel LED/segment display
  DEPENDS:=@TARGET_rockchip
  KCONFIG:=CONFIG_OPENVFD
  FILES:=$(LINUX_DIR)/drivers/misc/openvfd/openvfd.ko
  AUTOLOAD:=$(call AutoLoad,70,openvfd)
endef

define KernelPackage/openvfd/description
 Driver for the FD628/FD650-class 7-segment front panel display found on
 Android TV boxes; exposes /dev/openvfd to the openvfd daemon.
endef

$(eval $(call KernelPackage,openvfd))
