include $(TOPDIR)/rules.mk

PKG_VERSION:=2.10
PKG_RELEASE:=2
PKG_HASH:=skip

PKG_MAINTAINER:=Christian Marangi <ansuelsmth@gmail.com>

PKG_BUILD_DEPENDS:=arm-trusted-firmware-tools/host arm-trusted-firmware-airoha/host

HOST_BUILD_DIR:=$(BUILD_DIR_HOST)/trusted-firmware-a-airoha-$(PKG_VERSION)

include $(INCLUDE_DIR)/trusted-firmware-a.mk
include $(INCLUDE_DIR)/host-build.mk
include $(INCLUDE_DIR)/package.mk

define Trusted-Firmware-A/Default
  BUILD_TARGET:=airoha
  PLAT:=en7523
  HIDDEN:=y
endef

define Trusted-Firmware-A/bl2/Default
  NAME:=Airoha an7581 bl2
  ARCH:=aarch32
  ATF_STAGE:=bl2
endef

define Trusted-Firmware-A/an7581-bl2
  $(call Trusted-Firmware-A/bl2/Default)
  BUILD_SUBTARGET:=an7581
  SOC:=EN7581
  BUILD_DEVICES:=airoha_an7581-evb airoha_an7581-evb-emmc-eagle airoha_an7581-evb-emmc-kite nokia_valyrian nokia_xg-040g-md-ubi
endef

define Trusted-Firmware-A/an7583-bl2
  $(call Trusted-Firmware-A/bl2/Default)
  BUILD_SUBTARGET:=an7583
  SOC:=AN7583
  BUILD_DEVICES:=airoha_an7583-evb airoha_an7583-evb-emmc \
	nokia_xg-040g-mf-ubi
endef

define Trusted-Firmware-A/bl31/Default
  NAME:=Airoha an7581 bl31
  ARCH:=aarch64
  ATF_STAGE:=bl31
endef

define Trusted-Firmware-A/an7581-bl31
  $(call Trusted-Firmware-A/bl31/Default)
  BUILD_SUBTARGET:=an7581
  SOC:=EN7581
  BUILD_DEVICES:=airoha_an7581-evb airoha_an7581-evb-emmc-eagle airoha_an7581-evb-emmc-kite nokia_valyrian nokia_xg-040g-md-ubi
endef

define Trusted-Firmware-A/an7583-bl31
  $(call Trusted-Firmware-A/bl31/Default)
  BUILD_SUBTARGET:=an7583
  SOC:=AN7583
  BUILD_DEVICES:=airoha_an7583-evb airoha_an7583-evb-emmc \
	nokia_xg-040g-mf-ubi
endef

TFA_TARGETS:= \
	an7581-bl2 an7581-bl31 \
	an7583-bl2 an7583-bl31

TFA_MAKE_FLAGS= \
	ARM32TOOLCHAIN_BASE=$(PKG_BUILD_DIR)/$(BL2_GCC_SOURCE)/arm-none-eabi- \
	MBEDTLS_DIR=$(PKG_BUILD_DIR)/$(MBEDTLS_SOURCE) \
	ARCH=$(ARCH) CONFIG_ECNT=1  \
	TCSUPPORT_OPENWRT=1 TCSUPPORT_ATF_UNOPEN=0 \
	TCSUPPORT_CPU_$(SOC)=1 TCSUPPORT_CPU_EN7523=1 \
	TCSUPPORT_CPU_ARMV8=1 TCSUPPORT_UBOOT_64BIT=1 \
	TCSUPPORT_EMMC=1 TCSUPPORT_UBOOT=1 TCSUPPORT_BL2_OPTIMIZATION=1 \
	TOOLS_DIR=$(STAGING_DIR_HOST)/bin

# ARM's official bare-metal toolchain (gitlab.arm.com) is unreachable from this
# build host, and BL2 only needs a *soft-float* aarch32 compiler.  A wrapper
# prefix generated from the tree's arm toolchain is used instead (verified:
# readelf shows no Tag_ABI_VFP_args).  NOTE: ld must not get -mfloat-abi.
ARM_TC_DIR:=$(wildcard $(TOPDIR)/staging_dir/toolchain-arm_cortex-a7*/bin)
BL2_GCC_SOURCE:=arm-soft

define Download/atf-airoha
  URL:=https://github.com/pbs05/uboot-an758x.git
  SOURCE_VERSION:=4b91e112c345b8a4ad4f954775f3f4955f2cafb2
  MIRROR_HASH:a4be2789ce66e110dc76696c3ff20fc9a6c52f04681163ed27bca06a138ae0c7
  PROTO:=git
  FILE:=atf-airoha-4b91e112.tar.gz
  SUBDIR:=atf-airoha
endef

define Download/atf-airoha-host
  $(call Download/atf-airoha)
endef

MBEDTLS_NAME:=mbedtls
MBEDTLS_RELEASE:=3.4.1
MBEDTLS_SOURCE:=$(MBEDTLS_NAME)-$(MBEDTLS_RELEASE)

# Upstream ponwrt downloads mbedtls as an upstream release tarball (.tar.zst);
# 21.02 has no zstd support in the download machinery, so the .tar.xz release
# archive is used instead.  A git-generated archive is NOT equivalent: the one
# produced by the git fallback lacked library/*.c here.
define Download/mbedtls
  FILE:=$(MBEDTLS_SOURCE).tar.xz
  URL:=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.4.1
  MIRROR_HASH:=c95de8fa32ced29da1f7fc6715331eb94784404d3e02b208d3bbe6c2adf468f9
  SUBDIR:=$(MBEDTLS_SOURCE)
endef

# The download source is pbs05/uboot-an758x: a *complete U-Boot tree* that also
# carries a full tf-a/ sub-tree (upstream ponwrt downloads a TF-A-only fork, so
# there a plain --strip-components=1 is enough; here the tf-a/ sub-tree has to be
# selected explicitly).  Works equally well for a tarball we pre-seed in dl/ and
# for one OpenWrt regenerates from the git source.
define Host/Prepare
	$(eval $(call Download,atf-airoha-host))
	$(TAR) -C $(HOST_BUILD_DIR) --strip-components=2 --wildcards -xf $(DL_DIR)/$(FILE) '*/tf-a/*'
endef

HOST_CFLAGS += \
	-DFLASH_TABLE_OPEN \
	-DTCSUPPORT_BL2_OPTIMIZATION \
	-I$(HOST_BUILD_DIR)/plat/ecnt/en7523/include
define Host/Compile
	$(HOSTCC) $(HOST_CFLAGS) -o $(HOST_BUILD_DIR)/spi_nand_flash_table \
		$(HOST_BUILD_DIR)/plat/ecnt/common/drivers/flash/spi_nand_flash_table.c
endef

define Host/Install
	$(INSTALL_BIN) $(HOST_BUILD_DIR)/spi_nand_flash_table $(STAGING_DIR_HOST)/bin/airoha-atf-gen-flash-table
endef

define Build/Prepare
	# Soft-float wrapper prefix (see note above).  Uses a plain shell if-block
	# rather than the make if-function, because the body contains a `)` (a case
	# pattern) that the latter would swallow as its argument terminator.
	if [ "$(ATF_STAGE)" = "bl2" ]; then \
		$(INSTALL_DIR) $(PKG_BUILD_DIR)/$(BL2_GCC_SOURCE); \
		for t in $(ARM_TC_DIR)/arm-openwrt-linux-*; do \
			suf="`basename $$$$t | sed -e 's/^arm-openwrt-linux-//'`"; \
			case "$$$$suf" in \
				gcc|g++|cpp|cc) flag=" -mfloat-abi=soft" ;; \
				*)              flag="" ;; \
			esac; \
			printf '#!/bin/sh\nexec %s%s "$$$$@"\n' "$$$$t" "$$$$flag" \
				> $(PKG_BUILD_DIR)/$(BL2_GCC_SOURCE)/arm-none-eabi-$$$$suf; \
			chmod 755 $(PKG_BUILD_DIR)/$(BL2_GCC_SOURCE)/arm-none-eabi-$$$$suf; \
		done; \
	fi

	$(call Build/Prepare/Default,)

	$(eval $(call Download,mbedtls))
	$(TAR) -C $(PKG_BUILD_DIR) -xf $(DL_DIR)/$(FILE)

	$(eval $(call Download,atf-airoha))
	$(TAR) -C $(PKG_BUILD_DIR) --strip-components=2 --wildcards -xf $(DL_DIR)/$(FILE) '*/tf-a/*'
endef

define Build/Compile/bl2
	$(Build/Compile/Trusted-Firmware-A) IMAGE_BL21=1 bl2
	$(Build/Compile/Trusted-Firmware-A) clean
	$(Build/Compile/Trusted-Firmware-A) IMAGE_BL22=1 bl2
	$(Build/Compile/Trusted-Firmware-A) clean
	$(Build/Compile/Trusted-Firmware-A) IMAGE_BL23=1 bl2
endef

define Build/Compile/bl31
	$(Build/Compile/Trusted-Firmware-A) bl31
endef

define Build/Compile
	$(call Build/Compile/$(ATF_STAGE))
endef

define Package/trusted-firmware-a/bl2/install
	dd if=/dev/null of=$(PKG_BUILD_DIR)/bl21.bin bs=1 count=0 seek=14336

	(cd $(PKG_BUILD_DIR); $(STAGING_DIR_HOST)/bin/airoha-atf-gen-flash-table)
	$(STAGING_DIR_HOST)/bin/lzma e \
		$(PKG_BUILD_DIR)/flash_table.bin \
		$(PKG_BUILD_DIR)/flash_table.lzma

	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	./scripts/airoha_pack_bl2.sh \
		$(PKG_BUILD_DIR)/bl21.bin \
		$(PKG_BUILD_DIR)/bl22.lzma \
		$(PKG_BUILD_DIR)/bl23.lzma \
		$(PKG_BUILD_DIR)/flash_table.lzma \
		$(STAGING_DIR_IMAGE)/$(BUILD_SUBTARGET)-bl2.bin
endef

define Package/trusted-firmware-a/bl31/install
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(STAGING_DIR_HOST)/bin/lzma e \
		$(PKG_BUILD_DIR)/build/$(PLAT)/release/bl31.bin \
		$(STAGING_DIR_IMAGE)/$(BUILD_SUBTARGET)-bl31.lzma
endef

define Package/trusted-firmware-a/install
	$(call Package/trusted-firmware-a/$(ATF_STAGE)/install)
endef

$(eval $(call HostBuild))
$(eval $(call BuildPackage/Trusted-Firmware-A))
