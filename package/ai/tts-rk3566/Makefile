include $(TOPDIR)/rules.mk

PKG_NAME:=tts-rk3566
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_LICENSE:=Apache-2.0

include $(INCLUDE_DIR)/package.mk

define Package/tts-rk3566
  SECTION:=sound
  CATEGORY:=Sound
  TITLE:=Offline Chinese TTS for RK3566 (matcha-zh-baker, CPU)
  DEPENDS:=@TARGET_rockchip +rk356x-npu-ocr +ffmpeg-rkrga
endef

define Package/tts-rk3566/description
 Offline Chinese text-to-speech: text -> pinyin phones -> acoustic model
 (int8 QDQ, 22 MB) -> hifi-gan vocoder (fp32, 3.6 MB) -> 22.05 kHz wav.
 Runs on CPU through sherpa-onnx; no NPU, no network.
 The binaries are prebuilt aarch64/glibc because libonnxruntime and
 libsherpa-onnx are not buildable in this tree; they are started through
 /opt/glibc/ld-linux-aarch64.so.1 (musl rootfs).
 Measured on 10.80.1.125: RTF 0.84, 26 MB of model weights.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
endef

define Package/tts-rk3566/install
	$(INSTALL_DIR) $(1)/opt/tts-rk3566/lib
	$(INSTALL_DIR) $(1)/opt/tts-rk3566/matcha
	$(INSTALL_BIN) ./files/tts_sherpa $(1)/opt/tts-rk3566/
	$(INSTALL_BIN) ./files/tts.sh $(1)/usr/bin/tts
	$(INSTALL_DATA) ./files/lib/libonnxruntime.so $(1)/opt/tts-rk3566/lib/
	$(INSTALL_DATA) ./files/lib/libsherpa-onnx-c-api.so $(1)/opt/tts-rk3566/lib/
	$(INSTALL_DATA) ./files/lib/libsherpa-onnx-cxx-api.so $(1)/opt/tts-rk3566/lib/
	$(INSTALL_DATA) ./files/matcha/model_qdq.onnx $(1)/opt/tts-rk3566/matcha/
	$(INSTALL_DATA) ./files/matcha/hifigan_v2.onnx $(1)/opt/tts-rk3566/matcha/
	$(INSTALL_DATA) ./files/matcha/tokens.txt $(1)/opt/tts-rk3566/matcha/
	$(INSTALL_DATA) ./files/matcha/lexicon.txt $(1)/opt/tts-rk3566/matcha/
	$(INSTALL_DATA) ./files/matcha/date.fst $(1)/opt/tts-rk3566/matcha/
	$(INSTALL_DATA) ./files/matcha/number.fst $(1)/opt/tts-rk3566/matcha/
	$(INSTALL_DIR) $(1)/opt/tts-rk3566/matcha/dict
	$(INSTALL_DATA) ./files/matcha/dict/*.utf8 $(1)/opt/tts-rk3566/matcha/dict/
	$(INSTALL_DATA) ./files/README.md $(1)/opt/tts-rk3566/
endef

$(eval $(call BuildPackage,tts-rk3566))
