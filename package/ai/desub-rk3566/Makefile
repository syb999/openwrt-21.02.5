include $(TOPDIR)/rules.mk

PKG_NAME:=desub-rk3566
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/desub-rk3566
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=NPU subtitle removal for RK3566 (MI-GAN on RKNPU)
  DEPENDS:=@TARGET_rockchip +python3-light +python3-numpy +ffmpeg-rkrga +rk356x-npu-ocr
endef

define Package/desub-rk3566/description
 Burned-in subtitle removal running MI-GAN on the RK3566 NPU.
 Runtime is pure Python plus numpy; the 18 KB C runner and the 19 MB
 RKNN model ship prebuilt because librknnrt is a glibc-only vendor blob.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
endef

define Package/desub-rk3566/install
	$(INSTALL_DIR) $(1)/opt/desub-rk3566
	$(INSTALL_BIN) ./files/desub-rk3566.py $(1)/opt/desub-rk3566/
	$(INSTALL_BIN) ./files/one.py $(1)/opt/desub-rk3566/
	$(INSTALL_BIN) ./files/flicker.py $(1)/opt/desub-rk3566/
	$(INSTALL_BIN) ./files/scan.py $(1)/opt/desub-rk3566/
	$(INSTALL_BIN) ./files/bench.py $(1)/opt/desub-rk3566/
	$(INSTALL_BIN) ./files/cmp_models.py $(1)/opt/desub-rk3566/
	$(INSTALL_BIN) ./files/migan_rknn $(1)/opt/desub-rk3566/
	$(INSTALL_DATA) ./files/migan_noclip.rknn $(1)/opt/desub-rk3566/
	$(INSTALL_DATA) ./files/README.md $(1)/opt/desub-rk3566/
endef

$(eval $(call BuildPackage,desub-rk3566))
