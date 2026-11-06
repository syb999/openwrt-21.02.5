# SPDX-License-Identifier: GPL-2.0-only
#
# Amlogic S905/S905X (GXBB/GXL) devices.
#
# Boot chain on a stock Amlogic TV box u-boot (2015.01):
#
#   u-boot  bootcmd = "run start_autoscript; run storeboot"
#     -> start_mmc_autoscript / start_usb_autoscript   (vendor env)
#        fatload <media>:1 s905_autoscript ; autoscr
#          -> mesongx-s905_autoscript.cmd: fatload boot.scr ; autoscr
#             -> mesongx-boot.txt: fatload uImage + dtb ; bootm
#
# A mainline u-boot is neither needed nor built by this target: the vendor
# u-boot boots an ARM64 uImage plus a separate FDT through bootm, which is all
# this image asks for.  The medium only has to carry a FAT boot partition and
# an ext4 rootfs.

define Device/Default
  FILESYSTEMS := ext4
  KERNEL_DEPENDS = $$(wildcard $(DTS_DIR)/$$(DEVICE_DTS).dts)
  KERNEL_NAME := Image
  KERNEL := kernel-bin | uImage none
  KERNEL_LOADADDR := 0x01080000
  DEVICE_DTS_DIR := $(DTS_DIR)/amlogic
  PROFILES = Default $$(DEVICE_NAME)
endef

FAT_BLOCK_SIZE := 1024
FAT_BLOCKS := $(shell echo $$(( $(CONFIG_TARGET_KERNEL_PARTSIZE) * 1024 * 1024 / $(FAT_BLOCK_SIZE) )))

# FAT boot partition: s905_autoscript is the entry point the stock Amlogic
# u-boot looks for (it is also shipped as boot.scr for bootloaders that load
# that name instead), aml_autoscript is what its recovery path executes.
define Build/mesongx-boot-fs
	$(RM) -rf $@.bootdir
	$(INSTALL_DIR) $@.bootdir
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n "s905_autoscript" \
		-d mesongx-autoscript.cmd $@.bootdir/s905_autoscript
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n "boot.scr" \
		-d mesongx-autoscript.cmd $@.bootdir/boot.scr
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n "aml_autoscript" \
		-d aml_autoscript.cmd $@.bootdir/aml_autoscript
	$(CP) $(IMAGE_KERNEL) $@.bootdir/uImage
	$(CP) $(KDIR)/image-$(DEVICE_DTS).dtb $@.bootdir/dtb
	$(RM) -f $@.boot
	mkfs.fat -n boot -C $@.boot $(FAT_BLOCKS)
	mcopy -i $@.boot $@.bootdir/s905_autoscript $@.bootdir/boot.scr \
		$@.bootdir/aml_autoscript $@.bootdir/uImage $@.bootdir/dtb ::
	$(RM) -rf $@.bootdir
endef

define Build/mesongx-sd-img
	$(OC_PYTHON) ./gen_mesongx_sd_img.py $@ $@.boot \
		$(CONFIG_TARGET_KERNEL_PARTSIZE) $(IMAGE_ROOTFS) \
		$(CONFIG_TARGET_ROOTFS_PARTSIZE)
	$(RM) -f $@.boot
endef

# sysupgrade tarball: boot (FAT image) + CONTROL + root (ext4 image).  Member
# order matters - platform.sh reads the first member to find the directory, and
# it must not have to stream the whole rootfs for that.
define Build/mesongx-sysupgrade
	$(RM) -rf $(KDIR)/mesongx-sysupgrade
	$(INSTALL_DIR) $(KDIR)/mesongx-sysupgrade/sysupgrade-$(1)
	echo "BOARD=$(1)" > $(KDIR)/mesongx-sysupgrade/sysupgrade-$(1)/CONTROL
	$(CP) $@.boot $(KDIR)/mesongx-sysupgrade/sysupgrade-$(1)/boot
	$(CP) $(IMAGE_ROOTFS) $(KDIR)/mesongx-sysupgrade/sysupgrade-$(1)/root
	( cd $(KDIR)/mesongx-sysupgrade && \
		tar -cf - sysupgrade-$(1)/boot sysupgrade-$(1)/CONTROL sysupgrade-$(1)/root ) > $@
	$(RM) -rf $(KDIR)/mesongx-sysupgrade
	$(RM) -f $@.boot
endef

define Device/sumavision_q5
  DEVICE_VENDOR := SumaVision
  DEVICE_MODEL := Q5
  DEVICE_DTS := meson-gxbb-q5
  SOC := meson-gxbb
  DEVICE_PACKAGES := kmod-usb2 kmod-usb-storage kmod-fs-vfat kmod-nls-cp437 kmod-nls-utf8
  IMAGES := sdcard.img.gz sysupgrade.tar.gz
  IMAGE/sdcard.img.gz := mesongx-boot-fs | mesongx-sd-img | gzip
  IMAGE/sysupgrade.tar.gz := mesongx-boot-fs | mesongx-sysupgrade $$(DEVICE_NAME) | gzip
endef
TARGET_DEVICES += sumavision_q5
