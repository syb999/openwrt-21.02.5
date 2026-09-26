include $(TOPDIR)/rules.mk

PKG_NAME:=rockchip-mpp
PKG_VERSION:=1.1.0
PKG_RELEASE:=1

PKG_SOURCE:=mpp-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/rockchip-linux/mpp/archive/refs/tags/
PKG_SOURCE_URL_FILE:=$(PKG_VERSION).tar.gz
PKG_HASH:=7bb0040f364468ad9abbb71a04553ebc6a5814584e083c2fc9a553783e9656df
PKG_BUILD_DIR:=$(BUILD_DIR)/mpp-$(PKG_VERSION)

PKG_LICENSE:=Apache-2.0 MIT
PKG_LICENSE_FILES:=LICENSES/Apache-2.0 LICENSES/MIT

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/rockchip-mpp
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Rockchip Media Process Platform (MPP)
  URL:=https://github.com/rockchip-linux/mpp
  DEPENDS:=@aarch64 +libstdcpp
endef

define Package/rockchip-mpp/description
 Rockchip MPP userspace library (librockchip_mpp), used by the ffmpeg rkmpp
 hardware decoders.  Pairs with the kernel rk_vcodec.ko (mpp_service).
endef

# The CMA/heap backed buffers need the contiguous allocator, hence CMA >= 64M.
#
# Binutils passed explicitly: the OpenWrt cmake wrapper only sets CMAKE_AR /
# CMAKE_RANLIB / CMAKE_NM, and it sets CMAKE_STRIP to the literal ":" (a shell
# no-op).  CMAKE_STRIP is a FILEPATH-typed cache entry, so cmake resolves that
# relative ":" against the binary dir and stores "<build_dir>/:" - any recipe
# that shells out to ${CMAKE_STRIP} then dies with
#     /bin/sh: 1: <build_dir>/:: not found        (Error 127)
# mpp's static-library POST_BUILD step (mpp/CMakeLists.txt, "strip --strip-debug
# librockchip_mpp.a") does exactly that.  Passing the real toolchain strip below
# wins because CMAKE_OPTIONS is emitted after -DCMAKE_STRIP=: in cmake.mk.
TOOLCHAIN_BIN:=/home/oem/myopenwrt/openwrt-21.02.5/staging_dir/toolchain-aarch64_cortex-a53_gcc-8.4.0_musl/bin
CMAKE_OPTIONS += -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib \
	-DBUILD_SHARED_LIBS=ON -DBUILD_TEST=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DCMAKE_AR=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ar -DCMAKE_RANLIB=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ranlib \
	-DCMAKE_NM=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-nm -DCMAKE_STRIP=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-strip \
	-DCMAKE_C_COMPILER_AR=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ar -DCMAKE_C_COMPILER_RANLIB=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ranlib \
	-DCMAKE_CXX_COMPILER_AR=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ar -DCMAKE_CXX_COMPILER_RANLIB=$(TOOLCHAIN_BIN)/aarch64-openwrt-linux-musl-ranlib
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_INSTALL_DIR)/usr/include/rockchip $(1)/usr/include/
	$(INSTALL_DIR) $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librockchip_mpp.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/rockchip_mpp.pc $(1)/usr/lib/pkgconfig/
endef

define Package/rockchip-mpp/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librockchip_mpp.so.* $(1)/usr/lib/
endef

$(eval $(call BuildPackage,rockchip-mpp))
