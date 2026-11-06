# SPDX-License-Identifier: GPL-2.0-only
#
# Amlogic S905/S905X (GXBB/GXL) boards, 64 bit kernel.

ARCH:=aarch64
SUBTARGET:=mesongx
BOARDNAME:=Amlogic S905/S905X boards (64 bit)
CPU_TYPE:=cortex-a53

# panel-ap-setup opens the wan zone (and turns the single port into a bridge)
# on first boot, which is what makes a one-port box reachable at all.
DEFAULT_PACKAGES += ethtool panel-ap-setup luci alsa-utils

define Target/Description
	Build firmware image for Amlogic S905/S905X devices.
	This firmware features a 64 bit kernel.
endef
