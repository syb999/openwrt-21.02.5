# SPDX-License-Identifier: GPL-2.0-only
#
# Airoha AN7581/AN7583 (ARM64) target - backported to Linux 5.4

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=airoha
BOARDNAME:=Airoha ARM
SUBTARGETS:=an7581 an7583
FEATURES:=dt squashfs nand ramdisk gpio

KERNEL_PATCHVER:=5.4
KERNELNAME:=Image dtbs

define Target/Description
	Build firmware images for Airoha AN7581 ARM based boards.
endef

include $(INCLUDE_DIR)/target.mk
DEFAULT_PACKAGES += kmod-leds-gpio kmod-gpio-button-hotplug

$(eval $(call BuildTarget))
