define Build/an7581-emmc-bl2-bl31-uboot
  head -c $$((0x800)) /dev/zero > $@
  cat $(STAGING_DIR_IMAGE)/an7581_$1-bl2.fip >> $@
  dd if=$(STAGING_DIR_IMAGE)/an7581_$1-bl31-u-boot.fip of=$@ bs=1 seek=$$((0x20000)) conv=notrunc
endef
define Build/an7581-preloader
  $(STAGING_DIR_HOST)/bin/fiptool create \
		--tb-fw $(STAGING_DIR_IMAGE)/an7581-bl2.bin \
		$(STAGING_DIR_IMAGE)/an7581_$1-bl2.fip
  cat $(STAGING_DIR_IMAGE)/an7581_$1-bl2.fip >> $@
endef
define Build/an7581-bl31-uboot
  $(STAGING_DIR_HOST)/bin/fiptool create \
		--soc-fw $(STAGING_DIR_IMAGE)/an7581-bl31.lzma \
		--nt-fw $(STAGING_DIR_IMAGE)/an7581_$1-u-boot.lzma \
		$(STAGING_DIR_IMAGE)/an7581_$1-bl31-u-boot.fip
  cat $(STAGING_DIR_IMAGE)/an7581_$1-bl31-u-boot.fip >> $@
endef
define Build/an7581-chainloader
  $(INSTALL_DIR) $(KDIR)/chainload-fit-$(notdir $@)
  @if [ -f "$(STAGING_DIR_IMAGE)/an7581_$1-u-boot.lzma" ]; then \
    KERNEL="$(STAGING_DIR_IMAGE)/an7581_$1-u-boot.lzma"; \
    COMP="lzma"; \
  else \
    KERNEL="$(STAGING_DIR_IMAGE)/an7581_$1-u-boot.bin"; \
    COMP="none"; \
  fi; \
  $(TOPDIR)/scripts/mkits.sh \
    -D $(DEVICE_NAME) \
    -o $(KDIR)/chainload-fit-$(notdir $@)/u-boot.its \
    -k $$KERNEL \
    -C $$COMP \
    -a 0x80200000 -e 0x80200000 \
    -c conf-uboot \
    -A arm64 -v u-boot \
    -d $(STAGING_DIR_IMAGE)/an7581_$1-u-boot.dtb \
    -s 0x82000000
  PATH=$(LINUX_DIR)/scripts/dtc:$(PATH) \
    $(STAGING_DIR_HOST)/bin/mkimage \
    -D "-i $(KDIR)/chainload-fit-$(notdir $@)" \
    -f $(KDIR)/chainload-fit-$(notdir $@)/u-boot.its \
    $(STAGING_DIR_IMAGE)/an7581_$1-chainload-u-boot.itb
  cat $(STAGING_DIR_IMAGE)/an7581_$1-chainload-u-boot.itb >> $@
endef

define Device/FitImageLzma
	KERNEL_SUFFIX := -uImage.itb
	KERNEL = kernel-bin | lzma | fit lzma $$(KDIR)/image-$$(DEVICE_DTS).dtb
	KERNEL_NAME := Image
endef

define Device/airoha_an7581-evb
  $(call Device/FitImageLzma)
  DEVICE_VENDOR := Airoha
  DEVICE_MODEL := AN7581 Evaluation Board (SNAND)
  DEVICE_DTS := an7581-evb
  DEVICE_DTS_CONFIG := config@1
  DEVICE_PACKAGES := kmod-usb-ledtrig-usbport
  IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += airoha_an7581-evb

define Device/airoha_an7581-evb-emmc
  $(call Device/FitImageLzma)
  DEVICE_VENDOR := Airoha
  DEVICE_MODEL := AN7581 Evaluation Board (EMMC)
  DEVICE_DTS := an7581-evb-emmc
  DEVICE_DTS_CONFIG := config@1
  DEVICE_PACKAGES := kmod-usb-ledtrig-usbport
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | pad-rootfs | append-metadata
endef
TARGET_DEVICES += airoha_an7581-evb-emmc

  # NOTE: ARTIFACT/preloader.bin + ARTIFACT/bl31-uboot.fip are intentionally NOT
  # enabled: this tree's arm-trusted-firmware-airoha has BUILD_DEVICES empty
  # (deliberate), so fiptool and the an7583 bl2/bl31 staging images do not exist.
  # The Build/an7583-* recipes above are kept ready for when that is wired up.
