include $(TOPDIR)/rules.mk

PKG_NAME:=rk356x-yolov5n
PKG_VERSION:=2026-09-27
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=RK3566 Panther X2 project

include $(INCLUDE_DIR)/package.mk

# Package-only build: every payload is a prebuilt aarch64 blob shipped under
# ./files.  Disable rstrip so the vendored ELF demo is installed byte-for-byte
# identical to the verified working copy (stripping is pointless here and would
# rewrite the blob).
RSTRIP:=:

define Package/rk356x-yolov5n
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Rockchip RK356x (RK3566/RK3568) NPU YOLOv5n object-detection demo
  URL:=https://github.com/airockchip/rknn_model_zoo
  DEPENDS:=@TARGET_rockchip +rk356x-npu-ocr
endef

define Package/rk356x-yolov5n/description
  YOLOv5n (INT8, RKNPU1) object-detection demo for RK3566/RK3568 boards
  running a musl OpenWrt userland.  Ships the prepared demo binary, the
  yolov5n_rk3566_i8.rknn model, the COCO label list and a sample image.
  It reuses the glibc runtime (/opt/glibc) and the RKNPU runtime
  (/opt/rknpu) provided by rk356x-npu-ocr, so no duplicate 20 MB+ payloads
  are shipped.  RK356x family only: RK3588/RK3576 use a different NPU
  generation and are NOT supported.
  Installs:
    /opt/yolov5/    demo, model, labels, runner script
    /usr/bin/yolov5n  wrapper (launches the demo through the glibc loader)
endef

# No source tarball: every payload is shipped prebuilt under ./files.
define Build/Prepare
endef

define Build/Compile
endef

define Package/rk356x-yolov5n/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/usr/bin/yolov5n $(1)/usr/bin/yolov5n

	# Engine tree: copy the whole directory as-is so anything dropped into
	# ./files/opt/yolov5 gets installed verbatim.
	$(INSTALL_DIR) $(1)/opt/yolov5
	$(CP) ./files/opt/yolov5/. $(1)/opt/yolov5/

	$(INSTALL_DIR) $(1)/usr/share/doc/rk356x-yolov5n
	$(INSTALL_DATA) ./files/usr/share/doc/rk356x-yolov5n/README $(1)/usr/share/doc/rk356x-yolov5n/README
endef

define Package/rk356x-yolov5n/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	# The prebuilt ELF objects must keep their execute bit after unpacking.
	chmod 0755 /opt/yolov5/rknn_yolov5_demo 2>/dev/null
	chmod 0755 /opt/yolov5/run_yolov5n.sh 2>/dev/null
	chmod 0755 /usr/bin/yolov5n 2>/dev/null
	echo "rk356x-yolov5n: installed. Run 'yolov5n /opt/yolov5/model/bus.jpg' to test."
}
exit 0
endef

$(eval $(call BuildPackage,rk356x-yolov5n))
