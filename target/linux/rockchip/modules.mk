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
