# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2020 Tobias Maedel

define Device/friendlyarm_nanopi-r2s
  DEVICE_VENDOR := FriendlyARM
  DEVICE_MODEL := NanoPi R2S
  SOC := rk3328
  UBOOT_DEVICE_NAME := nanopi-r2s-rk3328
  IMAGE/sysupgrade.img.gz := boot-common | boot-script nanopi-r2s | pine64-img | gzip | append-metadata
  DEVICE_PACKAGES := kmod-usb-net-rtl8152
endef
TARGET_DEVICES += friendlyarm_nanopi-r2s

define Device/pine64_rockpro64
  DEVICE_VENDOR := Pine64
  DEVICE_MODEL := RockPro64
  SOC := rk3399
  UBOOT_DEVICE_NAME := rockpro64-rk3399
  IMAGE/sysupgrade.img.gz := boot-common | boot-script | pine64-img | gzip | append-metadata
endef
TARGET_DEVICES += pine64_rockpro64

define Device/radxa_rock-pi-4
  DEVICE_VENDOR := Radxa
  DEVICE_MODEL := ROCK Pi 4
  SOC := rk3399
  SUPPORTED_DEVICES := radxa,rockpi4
  UBOOT_DEVICE_NAME := rock-pi-4-rk3399
  IMAGE/sysupgrade.img.gz := boot-common | boot-script | pine64-img | gzip | append-metadata
endef
TARGET_DEVICES += radxa_rock-pi-4

# RK3566 (Panther X2): boot chain is TPL/ATF from rkbin + U-Boot 2026.07, see
# package/boot/uboot-rockchip-rk356x and package/boot/rockchip-rkbin.
# U-Boot comes from the upstream "generic-rk3568" target: its DT explicitly
# covers RK3566/RK3568 with eMMC, SD-card and serial2 at 1500000n8, which is
# what this board uses for its console.
define Device/panther_x2
  DEVICE_VENDOR := Panther
  DEVICE_MODEL := X2
  DEVICE_DTS := rockchip/rk3566-panther-x2
  SUPPORTED_DEVICES := panther,x2
  SOC := rk3566
  UBOOT_DEVICE_NAME := generic-rk3568
  IMAGE/sysupgrade.img.gz := boot-common | boot-script | pine64-img | gzip | append-metadata
  DEVICE_PACKAGES := e2fsprogs mkf2fs fdisk cfdisk partx-utils block-mount \
    dosfstools kmod-fs-vfat kmod-fs-msdos kmod-fs-ext4 \
    kmod-nls-cp437 kmod-nls-utf8 \
    ntfs-3g kmod-fuse \
    kmod-usb-storage kmod-usb-storage-uas kmod-usb-storage-extras \
    kmod-usb-net-rtl8152 \
    kmod-brcmfmac wpad-basic-wolfssl iw kmod-bluetooth \
    kmod-rk-vcodec \
    luci-app-vputrans luci-app-rk356x-ocr luci-app-rk356x-yolov5n panel-ap-setup sht2x -urngd
endef
TARGET_DEVICES += panther_x2

# RK3528 (HINLINK OPC-H28K): boot chain is TPL/ATF from rkbin + U-Boot 2026.07,
# see package/boot/uboot-rockchip-rk3528 and package/boot/rockchip-rkbin.
define Device/hinlink_opc-h28k
  DEVICE_VENDOR := HINLINK
  DEVICE_MODEL := OPC-H28K
  SOC := rk3528
  DEVICE_DTS := rockchip/rk3528-opc-h28k
  UBOOT_DEVICE_NAME := generic-rk3528
  IMAGE/sysupgrade.img.gz := boot-common | boot-script | pine64-img | gzip | append-metadata
  DEVICE_PACKAGES := kmod-r8168 e2fsprogs mkf2fs \
    fdisk cfdisk partx-utils block-mount dosfstools \
    kmod-fs-vfat kmod-fs-msdos kmod-nls-cp437 kmod-nls-utf8 \
    ntfs-3g kmod-fuse luci-app-rk356x-ocr \
    kmod-usb-storage kmod-usb-storage-uas kmod-usb-storage-extras -urngd
endef
TARGET_DEVICES += hinlink_opc-h28k
