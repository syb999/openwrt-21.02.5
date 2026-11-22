include $(TOPDIR)/rules.mk

PKG_NAME:=sr-rk3566
PKG_VERSION:=1.0.0
PKG_RELEASE:=2

PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/sr-rk3566
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=NPU video upscaling for RK3566 (Real-ESRGAN animevideov3)
  DEPENDS:=@TARGET_rockchip +python3-light +python3-numpy +ffmpeg-rkrga +rk356x-npu-ocr
endef

define Package/sr-rk3566/description
 2x video upscaling with Real-ESRGAN animevideov3 running on the RK3566 NPU.
 Picks the tile size from the source aspect ratio (4:3 -> 288x216,
 16:9 -> 320x180), tiles the frame at native resolution, runs 4x per tile
 and downscales to a 2x output with the original audio copied over.
 The 18 KB C runner and both RKNN models ship prebuilt because librknnrt
 is a glibc-only vendor blob.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
endef

define Package/sr-rk3566/install
	$(INSTALL_DIR) $(1)/opt/sr-rk3566
	$(INSTALL_BIN) ./files/sr-rk3566.py $(1)/opt/sr-rk3566/
	$(INSTALL_BIN) ./files/anime4k_rknn $(1)/opt/sr-rk3566/
	$(INSTALL_DATA) ./files/av3_res.rknn $(1)/opt/sr-rk3566/
	$(INSTALL_DATA) ./files/av3_320x180.rknn $(1)/opt/sr-rk3566/
	$(INSTALL_DATA) ./files/av3_576x432.rknn $(1)/opt/sr-rk3566/
	$(INSTALL_DATA) ./files/av3_640x480.rknn $(1)/opt/sr-rk3566/
	$(INSTALL_DATA) ./files/av3_640x360.rknn $(1)/opt/sr-rk3566/
	$(INSTALL_DATA) ./files/README.md $(1)/opt/sr-rk3566/
endef

$(eval $(call BuildPackage,sr-rk3566))
