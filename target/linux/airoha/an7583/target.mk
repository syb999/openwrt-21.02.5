ARCH:=aarch64
SUBTARGET:=an7583
BOARDNAME:=AN7583
CPU_TYPE:=cortex-a53
KERNELNAME:=Image dtbs
FEATURES+=pwm

define Target/Description
	Build firmware images for Airoha an7583 ARM based boards.
endef

# Ported from pbs05/ponwrt.  Upstream also lists airoha-an7583-npu-firmware,
# which this tree does not provide yet.
DEFAULT_PACKAGES += kmod-leds-gpio kmod-gpio-button-hotplug uboot-envtools
