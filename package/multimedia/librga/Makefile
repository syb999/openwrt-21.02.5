#
# librga - Rockchip RGA (2D Raster Graphic Acceleration) user space library
#
# Source: unofficial but faithful mirror of the (now removed) upstream
#         rockchip-linux/rga repository.  Provides the im2d API that the
#         ffmpeg-rkrga "scale_rkrga" / "vpp_rkrga" filters use to drive the
#         RGA2 block found on RK3566/RK3568.
#
# Built for musl/aarch64 with the OpenWrt toolchain so that it can be linked
# directly by other musl binaries (no glibc loader tricks needed).
#

include $(TOPDIR)/rules.mk

PKG_NAME:=librga
PKG_VERSION:=1.10.5
PKG_RELEASE:=1

PKG_REV:=0e67a8fc9b4ce493f77e6371de314bcbd9befac8
PKG_SOURCE:=$(PKG_REV).tar.gz
PKG_SOURCE_URL:=https://github.com/jjm2473/librga/archive
PKG_HASH:=47d78f5d23d623a767bd6b102de1a39dcfe4467c6e03ba7c25f11be3a569585f
PKG_BUILD_DIR:=$(BUILD_DIR)/librga-$(PKG_REV)

PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=COPYING

PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/librga
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Rockchip RGA 2D acceleration library (im2d)
  URL:=https://github.com/rockchip-linux/rga
  DEPENDS:=@aarch64 +libstdcpp
endef

define Package/librga/description
 User space library for the Rockchip RGA 2D raster graphic accelerator.
 Provides the legacy RgaApi (c_RkRgaBlit) and the modern im2d API
 (imcopy / imresize / imcvtcolor / improcess).  Requires the kernel
 "rga" driver (/dev/rga).
endef

# Same binutils/CMAKE_STRIP workaround as package/multimedia/rockchip-mpp:
# the OpenWrt cmake wrapper sets CMAKE_STRIP to the literal ":" which cmake
# resolves against the build dir.  Pass the real toolchain binaries so any
# POST_BUILD strip step keeps working.
TOOLCHAIN_BIN:=/home/oem/myopenwrt/openwrt-21.02.5/staging_dir/toolchain-aarch64_cortex-a53_gcc-8.4.0_musl/bin
CMAKE_OPTIONS += \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_BUILD_TARGET=cmake_linux \
	-DBUILD_WITH_VERSION=true \
	-DRGA_SAMPLES_ENABLE=false \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_INSTALL_INCLUDEDIR=include \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DCMAKE_AR=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ar \
	-DCMAKE_RANLIB=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ranlib \
	-DCMAKE_NM=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-nm \
	-DCMAKE_STRIP=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-strip \
	-DCMAKE_C_COMPILER_AR=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ar \
	-DCMAKE_C_COMPILER_RANLIB=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ranlib \
	-DCMAKE_CXX_COMPILER_AR=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ar \
	-DCMAKE_CXX_COMPILER_RANLIB=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ranlib

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/rga
	$(CP) $(PKG_INSTALL_DIR)/usr/include/*.h $(1)/usr/include/rga/
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librga.so* $(1)/usr/lib/
	$(INSTALL_DIR) $(1)/usr/lib/pkgconfig
	$(INSTALL_DATA) ./files/librga.pc $(1)/usr/lib/pkgconfig/librga.pc
endef

define Package/librga/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librga.so* $(1)/usr/lib/
endef

$(eval $(call BuildPackage,librga))
