include $(TOPDIR)/rules.mk

PKG_NAME:=rk356x-npu-ocr
PKG_VERSION:=2026-09-27
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0 LGPL-2.1-or-later GPL-2.0-or-later GPL-3.0-with-GCC-exception
PKG_MAINTAINER:=RK3566 Panther X2 project

include $(INCLUDE_DIR)/package.mk

# Keep the prebuilt payloads byte-for-byte identical to the verified working
# set: the default rstrip also strips shared objects and would rewrite the
# bundled glibc (libc.so.6, ld.so) and librknnrt.so, which is unsafe for glibc
# and needlessly changes vendored blobs.
RSTRIP:=:

define Package/rk356x-npu-ocr
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Rockchip RK356x (RK3566/RK3568) NPU PP-OCR engine (with bundled glibc runtime)
  URL:=https://github.com/airockchip/rknn_model_zoo
  DEPENDS:=@TARGET_rockchip
endef

define Package/rk356x-npu-ocr/description
  NPU (RKNPU1/RKNPU2 ABI) text-recognition engine for RK3566/RK3568 boards
  running a musl OpenWrt userland.  Ships the PP-OCRv4 detection/recognition
  models, the RKNN runtime (librknnrt.so) and the glibc runtime it needs, so a
  glibc engine can run on musl.  RK356x family only: RK3588/RK3576 use a
  different NPU generation and different model format and are NOT supported.
  Installs:
    /opt/ppocr/   engine, models, dictionary, test image
    /opt/rknpu/   librknnrt.so
    /opt/glibc/   glibc loader + shared libraries
    /usr/bin/ppocr  wrapper (launches the engine through the glibc loader)
    /usr/bin/rk356x-npu-ocr  alias for the wrapper
endef

# No source tarball: every payload is shipped prebuilt under ./files.
define Build/Prepare
endef

define Build/Compile
endef

define Package/rk356x-npu-ocr/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/usr/bin/ppocr $(1)/usr/bin/ppocr
	$(LN) ppocr $(1)/usr/bin/rk356x-npu-ocr

	# Engine tree: copy the whole directory instead of a per-file manifest, so
	# the evolving multi-mode CLI / tiling engine that also lives under
	# /opt/ppocr can simply be dropped into ./files/opt/ppocr and it gets
	# installed as-is.
	$(INSTALL_DIR) $(1)/opt/ppocr
	$(CP) ./files/opt/ppocr/. $(1)/opt/ppocr/

	$(INSTALL_DIR) $(1)/opt/rknpu
	$(CP) ./files/opt/rknpu/. $(1)/opt/rknpu/

	$(INSTALL_DIR) $(1)/opt/glibc
	$(CP) ./files/opt/glibc/. $(1)/opt/glibc/

	$(INSTALL_DIR) $(1)/usr/share/doc/rk356x-npu-ocr
	$(INSTALL_DATA) ./files/usr/share/doc/rk356x-npu-ocr/README $(1)/usr/share/doc/rk356x-npu-ocr/README
endef

define Package/rk356x-npu-ocr/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	# The prebuilt ELF objects must keep their execute bit after unpacking.
	chmod 0755 /opt/glibc/ld-linux-aarch64.so.1 2>/dev/null
	chmod 0755 /opt/ppocr/ppocr_demo 2>/dev/null
	chmod 0755 /usr/bin/ppocr 2>/dev/null
	echo "rk356x-npu-ocr: installed. Run 'ppocr /opt/ppocr/test.jpg' to test."
}
exit 0
endef

$(eval $(call BuildPackage,rk356x-npu-ocr))
