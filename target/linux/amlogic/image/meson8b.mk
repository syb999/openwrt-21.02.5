define Device/Default
  FILESYSTEMS := ext4
  IMAGES := emmc.img
  KERNEL_DEPENDS = $$(wildcard $(DTS_DIR)/$$(DEVICE_DTS).dts)
  KERNEL_LOADADDR := 0x01080000
  KERNEL_NAME := Image
  KERNEL := kernel-bin | uImage none
  PROFILES = Default $$(DEVICE_NAME)
endef

define Device/thunder-onecloud
  DEVICE_DTS := meson8b-onecloud
  DEVICE_TITLE := Thunder OneCloud
  KERNEL_LOADADDR := 0x00208000
  IMAGES += emmc.burn.img.gz sysupgrade.tar.gz
  DEVICE_PACKAGES := kmod-fs-vfat kmod-nls-cp437 kmod-nls-utf8
  IMAGE/emmc.img := boot-script onecloud | emmc-common $$(DEVICE_NAME)
  IMAGE/emmc.burn.img.gz := boot-script onecloud | onecloud-burn $$(DEVICE_NAME) | gzip
  IMAGE/sysupgrade.tar.gz := boot-script onecloud | onecloud-sysupgrade $$(DEVICE_NAME) | gzip
endef
TARGET_DEVICES += thunder-onecloud
