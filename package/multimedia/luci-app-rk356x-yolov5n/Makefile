#
# Copyright (C) 2026 RK3566 Panther X2 project
#
# This is free software, licensed under the Apache License, Version 2.0 .
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-rk356x-yolov5n
PKG_VERSION:=2026-09-28
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=RK3566 Panther X2 project

# luci.mk supplies the build/install rules.  The page carries English msgids
# and the Chinese strings inline through the template T() helper, so no po/
# directory (and therefore no luci-i18n subpackage) is shipped.
LUCI_TITLE:=LuCI App for RK356x NPU YOLOv5n object detection (nested under NPU OCR)
LUCI_DEPENDS:=+luci-base +rk356x-yolov5n @TARGET_rockchip @TARGET_rockchip_armv8
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
