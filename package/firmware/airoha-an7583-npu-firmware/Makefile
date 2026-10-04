include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-an7583-npu-firmware
PKG_VERSION:=2026.10.03
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

# Firmware for the Airoha AN7583 NPU (WiFi offload engine).  The driver is
# already part of the airoha ethernet package (drivers/net/ethernet/airoha/
# airoha_npu.c) and is built into the kernel; it loads these two images by the
# default names below (an7583_npu_soc_data), so the DTS needs no firmware-name.
define Download/AN7583_NPU_DATA
  FILE:=an7583_npu_data.bin
  URL:=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/airoha
  HASH:=21f06212c46803bfee0907ad755d55ea0f3672d4e81b59c63b3e0437239dfca4
endef
$(eval $(call Download,AN7583_NPU_DATA))

define Download/AN7583_NPU_RV32
  FILE:=an7583_npu_rv32.bin
  URL:=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/airoha
  HASH:=0d8bc1d22eee5bb4f36a70d1d450132f55493b88348d46e532894b564a36cca4
endef
$(eval $(call Download,AN7583_NPU_RV32))

define Build/Compile
endef

define Package/airoha-an7583-npu-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Airoha AN7583 NPU firmware
endef

define Package/airoha-an7583-npu-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/airoha
	$(INSTALL_DATA) $(DL_DIR)/an7583_npu_data.bin $(1)/lib/firmware/airoha/
	$(INSTALL_DATA) $(DL_DIR)/an7583_npu_rv32.bin $(1)/lib/firmware/airoha/
endef

$(eval $(call BuildPackage,airoha-an7583-npu-firmware))
