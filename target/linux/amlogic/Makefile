# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2006-2013 OpenWrt.org

include $(TOPDIR)/rules.mk

BOARD:=amlogic
BOARDNAME:=Amlogic Meson family
FEATURES:=boot-part ext4 audio usb fpu legacy-sdcard squashfs usbgadget display gpio pci pcie
SUBTARGETS:=meson8b

KERNEL_PATCHVER:=5.4

define Target/Description
	Build firmware image for Amlogic Meson SoC devices.
	Currently produces SD Card/USB disk image.
endef

include $(INCLUDE_DIR)/target.mk

# Ported from lede, which also lists automount and htop (not present in this
# tree) and autocore-arm (lede only).  bash + parted are needed by the upstream
# /root/resize.sh first boot script (the OneCloud grows its rootfs after the
# first boot).
DEFAULT_PACKAGES += \
	e2fsprogs \
	mkf2fs \
	resize2fs \
	partx-utils \
	losetup \
	bash \
	parted \
	kmod-usb2 \
	kmod-usb-storage \
	kmod-usb-storage-uas \
	kmod-scsi-core \
	kmod-fs-ext4 \
	kmod-fs-vfat \
	kmod-fs-exfat \
	kmod-nls-cp437 \
	kmod-nls-iso8859-1 \
	kmod-nls-utf8 \
	block-mount

KERNELNAME:=Image dtbs

$(eval $(call BuildTarget))
