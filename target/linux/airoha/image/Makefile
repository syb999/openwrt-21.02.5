include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

AIROHA_USB_STORAGE_PACKAGES := \
  block-mount kmod-usb-storage kmod-usb-storage-uas \
  kmod-fs-ext4 kmod-fs-vfat kmod-fs-exfat

loadaddr-$(CONFIG_TARGET_airoha_an7581) := 0x80200000
loadaddr-$(CONFIG_TARGET_airoha_an7583) := 0x80200000

# default all platform image(fit) build
define Device/Default
  PROFILES = Default $$(DEVICE_NAME)
  KERNEL_NAME := Image
  KERNEL = kernel-bin | lzma | \
	fit lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb
  KERNEL_INITRAMFS = kernel-bin | lzma | \
	fit lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb
  KERNEL_LOADADDR = $(loadaddr-y)
  FILESYSTEMS := squashfs
  DEVICE_DTS = $$(SOC)-$(lastword $(subst _, ,$(1)))
  DEVICE_DTS_DIR := ../dts
  DEVICE_DTS_CONFIG := config@1
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | pad-to 128k | append-rootfs | \
	pad-rootfs | append-metadata
endef

include $(SUBTARGET).mk

define Image/Build
	$(call Image/Build/$(1),$(1))
endef

$(eval $(call BuildImage))


# ---- airoha: modern fit support (ported from pbs05/ponwrt) ------------------
# The 21.02 Build/fit only understands "-C <compress> -d <dtb>"; the airoha
# recipes pass "external-static-with-rootfs" / "with-initrd" as a 3rd word,
# which the old version silently ignored - so the rootfs never made it into
# the sysupgrade .itb.  ponwrt's variant parses those flags with findstring.
# mkits.sh below is the matching (target-local) script with -r/-i/-s/-l support.
# Kept target-local on purpose so no shared file changes.
define Build/fit-its
	$(if $(findstring with-rootfs,$(word 3,$(1))), \
		$(call locked,dd if=$(IMAGE_ROOTFS) of=$(IMAGE_ROOTFS).pagesync bs=4096 conv=sync, \
		  gen-cpio$(if $(TARGET_PER_DEVICE_ROOTFS),.$(ROOTFS_ID/$(DEVICE_NAME)))))
	$(TOPDIR)/target/linux/airoha/image/mkits.sh \
		-D $(DEVICE_NAME) -o $@.its -k $@ \
		-C $(word 1,$(1)) \
		$(if $(word 2,$(1)),\
			$(if $(findstring 11,$(if $(DEVICE_DTS_OVERLAY),1)$(if $(findstring $(KERNEL_BUILD_DIR)/image-,$(word 2,$(1))),,1)), \
				-d $(KERNEL_BUILD_DIR)/image-$$(basename $(word 2,$(1))), \
				-d $(word 2,$(1)))) \
		$(if $(findstring with-rootfs,$(word 3,$(1))),-r $(IMAGE_ROOTFS)) \
		$(if $(findstring with-initrd,$(word 3,$(1))), \
			$(if $(CONFIG_TARGET_ROOTFS_INITRAMFS_SEPARATE), \
				-i $(KERNEL_BUILD_DIR)/initrd$(if $(TARGET_PER_DEVICE_ROOTFS),.$(ROOTFS_ID/$(DEVICE_NAME))).cpio$(strip $(call Build/initrd_compression)))) \
		-a $(KERNEL_LOADADDR) -e $(if $(KERNEL_ENTRY),$(KERNEL_ENTRY),$(KERNEL_LOADADDR)) \
		$(if $(DEVICE_FDT_NUM),-n $(DEVICE_FDT_NUM)) \
		$(if $(DEVICE_DTS_DELIMITER),-l $(DEVICE_DTS_DELIMITER)) \
		$(if $(DEVICE_DTS_LOADADDR),-s $(DEVICE_DTS_LOADADDR)) \
		$(if $(DEVICE_DTS_OVERLAY),$(foreach dtso,$(DEVICE_DTS_OVERLAY), -O $(dtso):$(KERNEL_BUILD_DIR)/image-$(dtso).dtbo)) \
		-c $(if $(DEVICE_DTS_CONFIG),$(DEVICE_DTS_CONFIG),"config-1") \
		-A $(LINUX_KARCH) -v $(LINUX_VERSION)
endef
define Build/fit-image
	$(call locked,PATH=$(LINUX_DIR)/scripts/dtc:$(PATH) mkimage $(if $(findstring external,$(word 3,$(1))),\
		-E -B 0x1000 $(if $(findstring static,$(word 3,$(1))),-p 0x1000)) -f $@.its $@.new, \
	  gen-cpio$(if $(TARGET_PER_DEVICE_ROOTFS),.$(ROOTFS_ID/$(DEVICE_NAME))))
	@mv $@.new $@
endef
define Build/fit
	$(call Build/fit-its,$(1))
	$(call Build/fit-image,$(1))
endef
