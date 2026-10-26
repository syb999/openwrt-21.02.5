#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

DEVICE_VARS += UBOOT_DEVICE_NAME

FAT32_BLOCK_SIZE=1024
FAT32_BLOCKS=$(shell echo $$(($(CONFIG_TARGET_KERNEL_PARTSIZE)*1024*1024/$(FAT32_BLOCK_SIZE))))

define Build/boot-script
	# Make an U-boot image and copy it to the boot partition.
	# 注意：必须是 $@ 私有的文件名。三个镜像目标（emmc.img / emmc.burn.img.gz /
	# sysupgrade.tar.gz）在 make -j 下是并行执行的，之前共用 $(KDIR)/boot.scr，
	# 会读到对方刚 truncate 的空文件 → FAT 里塞进 941 字节全零，u-boot 报
	# "Wrong image format for autoscript"。
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -n "boot.scr" -d boot.txt $@.scr
endef

define Build/emmc-common
	$(RM) -f $@.boot
	mkfs.fat -C $@.boot $(FAT32_BLOCKS)

	mkdir -p $(KDIR)/boot.fat

	$(CP) $@.scr $(KDIR)/boot.fat/boot.scr
	mcopy -i $@.boot $@.scr ::boot.scr
	$(CP) $(IMAGE_KERNEL) $(KDIR)/boot.fat/uImage
	mcopy -i $@.boot $(KDIR)/boot.fat/uImage ::

	$(foreach dts,$(shell echo $(DEVICE_DTS)),$(CP) $(DTS_DIR)/$(dts).dtb $(KDIR)/boot.fat/dtb;)
	mcopy -i $@.boot $(KDIR)/boot.fat/dtb ::

	$(RM) -rf $(KDIR)/boot.fat

	./gen_aml_emmc_img.sh $@ $@.boot $(IMAGE_ROOTFS) \
		$(CONFIG_TARGET_KERNEL_PARTSIZE) $(CONFIG_TARGET_ROOTFS_PARTSIZE)
endef

### Image scripts ###

# sysupgrade image: one tar with the boot (FAT) and rootfs (ext4) partitions,
# written by the board's platform_do_upgrade with dd (partition table untouched).
define Build/onecloud-sysupgrade
	$(RM) -rf $(KDIR)/onecloud-sysupgrade
	$(INSTALL_DIR) $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)
	echo "BOARD=$(1)" > $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)/CONTROL
	mkfs.fat -C $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)/boot $(FAT32_BLOCKS)
	mcopy -i $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)/boot $@.scr ::boot.scr
	mcopy -i $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)/boot $(IMAGE_KERNEL) ::uImage
	mcopy -i $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)/boot $(DTS_DIR)/$(firstword $(DEVICE_DTS)).dtb ::dtb
	$(CP) $(IMAGE_ROOTFS) $(KDIR)/onecloud-sysupgrade/sysupgrade-$(1)/root
	# 成员顺序必须是 boot → CONTROL → root：设备端 platform.sh 靠"第一个成员是
	# boot"来避免解压整个 512MB root（LuCI 的验证步骤有 60 秒 CGI 超时）。
	( cd $(KDIR)/onecloud-sysupgrade && \
		tar -cf - sysupgrade-$(1)/boot sysupgrade-$(1)/CONTROL sysupgrade-$(1)/root ) > $@
	$(RM) -rf $(KDIR)/onecloud-sysupgrade
endef

# Amlogic USB Burning Tool image ("*.burn.img"): the same boot (FAT) and rootfs
# (ext4) partitions as the sdcard/eMMC image, packed together with the u-boot
# components in ./onecloud-burn so that the result can be flashed directly with
# the USB Burning Tool (no external AmlImg / img2simg step needed).
OC_PYTHON:=$(or $(wildcard $(STAGING_DIR_HOST)/bin/python3),python3)

define Build/onecloud-burn
	$(RM) -rf $(KDIR)/onecloud-burn
	$(INSTALL_DIR) $(KDIR)/onecloud-burn
	mkfs.fat -C $(KDIR)/onecloud-burn/boot.fat $(FAT32_BLOCKS)
	mcopy -i $(KDIR)/onecloud-burn/boot.fat $@.scr ::boot.scr
	mcopy -i $(KDIR)/onecloud-burn/boot.fat $(IMAGE_KERNEL) ::uImage
	mcopy -i $(KDIR)/onecloud-burn/boot.fat $(DTS_DIR)/$(firstword $(DEVICE_DTS)).dtb ::dtb
	$(OC_PYTHON) ./gen_onecloud_burn_img.py $@ $(KDIR)/onecloud-burn/boot.fat \
		$(IMAGE_ROOTFS) ./onecloud-burn
	$(RM) -rf $(KDIR)/onecloud-burn
endef

define Build/boot-common
	# This creates a new folder copies the dtb (as amlogic.dtb) 
	# and the kernel image (as kernel.img)
	rm -fR $@.boot
	mkdir -p $@.boot

	$(CP) $(KDIR)/image-$(firstword $(DEVICE_DTS)).dtb $@.boot/amlogic.dtb
	$(CP) $(IMAGE_KERNEL) $@.boot/kernel.img
	$(CP) "$(STAGING_DIR_IMAGE)"/$(UBOOT_DEVICE_NAME)-u-boot-overlay.bin $@.boot/u-boot.emmc
endef

define Build/boot-combined
	# This creates a new folder copies the dtbs (as amlogic*.dtb) 
	# and the kernel image (as kernel.img)
	rm -fR $@.boot
	mkdir -p $@.boot

	i=0; \
	for dts in $(DEVICE_DTS); do \
		$(CP) $(KDIR)/image-$${dts}.dtb $@.boot/amlogic$$(perl -e 'printf "%b\n",'$$i).dtb; \
		let i+=1; \
	done
	$(CP) $(IMAGE_KERNEL) $@.boot/kernel.img
	$(CP) "$(STAGING_DIR_IMAGE)"/$(UBOOT_DEVICE_NAME)-u-boot-overlay.bin $@.boot/u-boot.emmc
endef

define Build/boot-combined-script
	# Make an U-boot image and copy it to the boot partition
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -d aml_autoscript.cmd $@.boot/aml_autoscript
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -d emmc_autoscript.cmd $@.boot/emmc_autoscript
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -d s905_autoscript.cmd $@.boot/s905_autoscript
	mkimage -A arm -O linux -T script -C none -a 0 -e 0 -d $(if $(1),$(1),amlogic).bootscript $@.boot/boot.scr
endef

define Build/aml-img
	# Creates the final SD/USB images, 
	# combining boot partition, root partition

	SIGNATURE="$(IMG_PART_SIGNATURE)" \
		./gen_amlogic_image.sh \
		$@ \
		64 $@.boot \
		256 $(IMAGE_ROOTFS) \
		2048

endef

### Devices ###

include $(SUBTARGET).mk

$(eval $(call BuildImage))
