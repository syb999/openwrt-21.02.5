include $(TOPDIR)/rules.mk

PKG_NAME:=realesrgan-rk3566
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_ARCH:=aarch64_cortex-a53
PKG_LICENSE:=BSD-3-Clause

include $(INCLUDE_DIR)/package.mk

define Package/realesrgan-rk3566
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Real-ESRGAN image upscale/restore (RK3566 NPU)
  DEPENDS:=@TARGET_rockchip
  PKGARCH:=$(PKG_ARCH)
endef

define Package/realesrgan-rk3566/description
  Real-ESRGAN general-x4v3（int8，RKNPU）
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
endef

define Package/realesrgan-rk3566/install
	$(INSTALL_DIR) $(1)/opt/realesrgan/models
	$(INSTALL_DATA) ./files/sr_img.tgz $(1)/opt/realesrgan/
	$(INSTALL_DATA) ./files/models/*.rknn $(1)/opt/realesrgan/models/
	$(INSTALL_BIN) ./files/demoire.py $(1)/opt/realesrgan/demoire.py
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/imgup $(1)/usr/bin/imgup
endef

define Package/realesrgan-rk3566/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
cd /opt/realesrgan && tar xzf sr_img.tgz && rm -f sr_img.tgz && chmod +x sr_img imgup 2>/dev/null
exit 0
endef

$(eval $(call BuildPackage,realesrgan-rk3566))
