#
# Copyright (C) 2026 RK3566 Panther X2 project
#
# This is free software, licensed under the Apache License, Version 2.0 .
#

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-rk356x-ocr
PKG_VERSION:=2026-09-27
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0

# luci.mk supplies the build/install rules and turns every po/<lang>/ directory
# into a luci-i18n-rk356x-ocr-<lang> subpackage.  The pages/controller carry
# English msgids only; the Chinese text lives in po/zh_Hans/.
LUCI_TITLE:=LuCI App for RK356x NPU OCR (top level menu)
LUCI_DEPENDS:=+luci-base +rk356x-npu-ocr @TARGET_rockchip @TARGET_rockchip_armv8
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
