include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-en8811h-firmware
PKG_VERSION:=2026.10.03
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

# Firmware for the Airoha EN8811H 2.5G PHY (used on the Nokia XG-040G-MF main
# 2.5G port).  The driver itself is built into the kernel via the target config
# (CONFIG_AIR_EN8811H_PHY=y in target/linux/airoha/an7583/config-5.4), so no
# kmod package is needed here - only these two blobs.
define Download/EthMD32_DM
  FILE:=EthMD32.dm.bin
  URL:=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/airoha
  HASH:=874982b88330112c376e484cdce114cf2e1476ccbb901c87f80882f127ffb90f
endef
$(eval $(call Download,EthMD32_DM))

define Download/EthMD32_DSP
  FILE:=EthMD32.DSP.bin
  URL:=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/airoha
  HASH:=3e4699ec709c836d5fce7c91bc5d205beb54aea326c4b70c7050b355784cbebd
endef
$(eval $(call Download,EthMD32_DSP))

define Build/Compile
endef

define Package/airoha-en8811h-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Airoha EN8811H 2.5G Ethernet PHY firmware
endef

define Package/airoha-en8811h-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/airoha
	$(INSTALL_DATA) $(DL_DIR)/EthMD32.dm.bin $(1)/lib/firmware/airoha/
	$(INSTALL_DATA) $(DL_DIR)/EthMD32.DSP.bin $(1)/lib/firmware/airoha/
endef

$(eval $(call BuildPackage,airoha-en8811h-firmware))
