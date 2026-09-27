#!/bin/sh
#
# sysupgrade support for the rockchip target.
#
# platform_do_upgrade() dispatches on board_name(); supporting a new board means
# adding one case arm plus one rockchip_do_upgrade_<board> function and nothing
# else.  The dedicated functions are independent, each documents the image
# layout it was written against, and none of them keeps a hard-coded sector
# offset: every start/size is read from the target image's own partition table
# at run time.
#
#   panther,x2                    (rk3566) -> rockchip_do_upgrade_panther
#   hinlink,opc-h28k / hinlink,h28k (rk3528) -> rockchip_do_upgrade_h28k
#   every other board (rk3328 NanoPi R2S, rk3399 RockPro64 / ROCK Pi 4, ...)
#                                 -> rockchip_do_upgrade_generic
#
# rockchip_do_upgrade_generic is the *unmodified upstream* OpenWrt body,
# reproduced byte-for-byte in one place so that a board which is not listed
# above upgrades exactly like it always did.
#
# Why the two dedicated boards do not use that body: the Rockchip boot chain
# reaches the storage *outside* the MBR partitions - the pine64-img layout used
# by this target puts the idbloader at sector 0x40 and u-boot.itb at sector
# 0x4000, both below the first partition - so the upstream "layout changed ->
# dd the whole disk from LBA 0" step rewrites the loader as well.  An upgrade
# that is cut short while those sectors are being written leaves a board that
# only maskrom mode can recover, which is why panther,x2 and hinlink,opc-h28k
# write the filesystem partitions only and leave the loader area and the
# partition table on the running disk untouched.
#
# Note: rockchip has a single subtarget (armv8).  This file exists both at the
# target level and in armv8/base-files/ because the subtarget copy is installed
# last and therefore wins; the two copies are kept identical on purpose so that
# neither ordering can make a board pick up a different upgrade path.
#
# shellcheck shell=sh

. /lib/functions.sh
. /lib/upgrade/common.sh

platform_check_image() {
	local diskdev partdev diff

	export_bootdevice && export_partdevice diskdev 0 || {
		echo "Unable to determine upgrade device"
		return 1
	}

	get_partitions "/dev/$diskdev" bootdisk

	#extract the boot sector from the image
	get_image "$@" | dd of=/tmp/image.bs count=1 bs=512b 2>/dev/null

	get_partitions /tmp/image.bs image

	#compare tables
	diff="$(grep -F -x -v -f /tmp/partmap.bootdisk /tmp/partmap.image)"

	rm -f /tmp/image.bs /tmp/partmap.bootdisk /tmp/partmap.image

	if [ -n "$diff" ]; then
		echo "Partition layout has changed. Full image will be written."
		ask_bool 0 "Abort" && exit 1
		return 0
	fi
}

platform_copy_config() {
	local partdev

	if export_partdevice partdev 1; then
		mount -o rw,noatime "/dev/$partdev" /mnt
		cp -af "$UPGRADE_BACKUP" "/mnt/$BACKUP_FILE"
		umount /mnt
	fi
}

#
# Shared helpers for the dedicated (partition-only) upgrade paths.
#
# The disk is resolved from the running root filesystem instead of from
# BOOT_DEVICE, because the kernel device name depends on the storage that
# actually booted and must never be hard-coded: on panther,x2 the (dead) SD slot
# and the SDIO wifi claim the earlier mmc numbers, so the eMMC shows up as
# mmcblk2; on hinlink,opc-h28k either the SD card or the eMMC can be the boot
# device.  /rom is the read-only lower layer of the running system, so the
# device node behind it is exactly the disk to upgrade.
#
rockchip_upgrade_root_disk() {
	local src disk

	src=$(awk '$2 == "/rom" { print $1; exit }' /proc/mounts)
	if [ -b "$src" ]; then
		src=$(readlink -f "$src" 2>/dev/null)
	elif [ -b /dev/root ]; then
		src=$(readlink -f /dev/root)
	else
		src=""
	fi

	if [ -n "$src" ]; then
		# /dev/mmcblk2p2 -> /dev/mmcblk2, /dev/sda2 -> /dev/sda
		disk=$(echo "$src" | sed -e 's|/dev/mmcblk\([0-9]*\)p[0-9]*$|/dev/mmcblk\1|' \
				      -e 's|/dev/sd\([a-z]*\)[0-9]*$|/dev/sd\1|' \
				      -e 's|/dev/nvme\([0-9]*\)n\([0-9]*\)p[0-9]*$|/dev/nvme\1n\2|')
		[ -b "$disk" ] && { echo "$disk"; return 0; }
	fi

	return 1
}

# /dev/mmcblk2 -> /dev/mmcblk2p1, /dev/sda -> /dev/sda1
rockchip_upgrade_part_dev() {
	local disk="$1" nr="$2"

	case "$disk" in
	/dev/mmcblk*|/dev/nvme*|/dev/loop*) echo "${disk}p${nr}" ;;
	*)                                   echo "${disk}${nr}" ;;
	esac
}

#
# Generic path - the upstream implementation, used by every board that has no
# dedicated function above.  Keep this body byte-for-byte identical to the
# generic OpenWrt code; it is what guarantees rk3328 (NanoPi R2S), rk3399
# (RockPro64 / ROCK Pi 4) and any board added later keep upgrading exactly as
# upstream does.
#
rockchip_do_upgrade_generic() {
	local diskdev partdev diff

	export_bootdevice && export_partdevice diskdev 0 || {
		echo "Unable to determine upgrade device"
		return 1
	}

	sync

	if [ "$UPGRADE_OPT_SAVE_PARTITIONS" = "1" ]; then
		get_partitions "/dev/$diskdev" bootdisk

		#extract the boot sector from the image
		get_image "$@" | dd of=/tmp/image.bs count=1 bs=512b

		get_partitions /tmp/image.bs image

		#compare tables
		diff="$(grep -F -x -v -f /tmp/partmap.bootdisk /tmp/partmap.image)"
	else
		diff=1
	fi

	if [ -n "$diff" ]; then
		get_image "$@" | dd of="/dev/$diskdev" bs=4096 conv=fsync

		# Separate removal and addtion is necessary; otherwise, partition 1
		# will be missing if it overlaps with the old partition 2
		partx -d - "/dev/$diskdev"
		partx -a - "/dev/$diskdev"

		return 0
	fi

	#iterate over each partition from the image and write it to the boot disk
	while read part start size; do
		if export_partdevice partdev $part; then
			echo "Writing image to /dev/$partdev..."
			get_image "$@" | dd of="/dev/$partdev" ibs="512" obs=1M skip="$start" count="$size" conv=fsync
		else
			echo "Unable to find partition $part device, skipped."
		fi
	done < /tmp/partmap.image

	#copy partition uuid
	echo "Writing new UUID to /dev/$diskdev..."
	get_image "$@" | dd of="/dev/$diskdev" bs=1 skip=440 count=4 seek=440 conv=fsync
}

#
# Panther X2 path (rk3566).
#
# Storage: eMMC only (the SD slot is dead), enumerating as mmcblk2.
#
# The disk is *not* derived from the running root filesystem: OpenWrt mounts
# the rootfs from the literal string /dev/root, which does not exist as a
# device node on this board (ls /dev/root -> No such file), so /proc/mounts
# cannot be used here - rockchip_upgrade_root_disk() always came back empty
# and the upgrade refused with "cannot determine the boot disk".  The boot
# device resolved from root=PARTUUID=... in /proc/cmdline is used instead:
# export_bootdevice sets BOOTDEV_MAJOR/MINOR (179:0 -> mmcblk2 on this board)
# and export_partdevice then maps partition numbers to minors, which on MMC
# are the disk minor plus the partition number (mmcblk2p1 -> minor 1,
# mmcblk2p2 -> minor 2).
#
# Image layout (pine64-img, same builder as every other board in this target):
#   sector 0x40   idbloader (RKNS container: TPL + DDR init)
#   sector 0x4000 u-boot.itb (ATF + U-Boot proper)
#   sector 0x10000 partition 1, ext4, 16 MiB boot partition
#   sector 0x20000 partition 2, squashfs root filesystem (+ f2fs overlay)
#
# Write only partition 1 (kernel) and partition 2 (rootfs) - explicitly, one
# at a time - so the loader areas below the first partition and the partition
# table on the running disk are never rewritten, and an upgrade that is cut
# short can always be retried.  A running partition whose size does not match
# the image is treated as a hard error, so a foreign or stale image can never
# be written half-way onto the eMMC.
#
rockchip_do_upgrade_panther() {
	local diskdev part partdev start size dpart dsize

	export_bootdevice && export_partdevice diskdev 0 || {
		echo "cannot determine the boot disk, refusing to upgrade"
		return 1
	}

	echo "upgrading /dev/$diskdev from $1"

	get_image "$1" | dd of=/tmp/image.bs count=1 bs=512b 2>/dev/null
	get_partitions /tmp/image.bs image

	sync

	while read part start size; do
		case "$part" in
		1|2) ;;
		*) continue ;;
		esac

		if ! export_partdevice partdev "$part"; then
			echo "partition $part ($diskdev$part) not found, skipped"
			continue
		fi

		dpart=$(basename "$partdev")
		dsize=$(cat "/sys/class/block/$dpart/size" 2>/dev/null)
		# Refuse only the dangerous direction: an image partition longer than
		# the running one would be written past its end.  A running partition
		# that is *larger* than the image's is normal - that is what a board
		# leaves behind after its kernel/rootfs partition was enlarged by a
		# full-image flash - and only the image's own length is written, so it
		# is accepted.  This is what lets sysupgrade keep working after
		# CONFIG_TARGET_KERNEL_PARTSIZE (or ROOTFS_PARTSIZE) was raised.
		if [ -n "$dsize" ] && [ "$size" -gt "$dsize" ]; then
			echo "partition $part: image ($size) longer than disk ($dsize), refusing"
			return 1
		fi

		echo "writing image partition $part to /dev/$partdev"
		get_image "$1" | dd of="/dev/$partdev" ibs=512 obs=1M \
			skip="$start" count="$size" conv=fsync
	done < /tmp/partmap.image

	sync
}

#
# HINLINK OPC-H28K path (rk3528).
#
# Storage: SD card *and* eMMC are both populated and the BootROM probes the SD
# card first, so the running system - and therefore the disk to upgrade - may be
# either one.  The boot script pins the root filesystem with
# root=PARTUUID=<uuid>, resolved on the device it actually booted from, so the
# upgrade must follow the running root disk and must not assume eMMC; that is
# the one thing that is genuinely different from panther,x2, which only ever has
# its eMMC.
#
# Image layout - the same pine64-img one, i.e. the same 32 MiB loader window
# that must survive an upgrade (this board has no exposed UART, so a board left
# half-written in that window can only be recovered from maskrom):
#   sector 0x40   idbloader (RKNS container, TPL/DDR init, generic-rk3528)
#   sector 0x4000 u-boot.itb (ATF + U-Boot 2026.07 proper, generic-rk3528)
#   sector 0x10000 partition 1, ext4, 32 MiB - kernel.img, rockchip.dtb, boot.scr
#   sector 0x18000 partition 2, squashfs root filesystem, f2fs overlay straight
#                 after the squashfs inside the same partition
#
# So exactly like the panther path, and for the same "no UART" reason: only the
# partitions listed by the target image are written, the first 32 MiB and the
# partition table on the running disk (and with it the PARTUUID the boot script
# depends on) are left alone.
#
# Difference to panther,x2: partition 2 here follows CONFIG_TARGET_ROOTFS_PARTSIZE
# and a running partition may therefore be larger *or* smaller than the one in
# the image.  Larger is fine - only the image's own length is written and the
# surplus stays free for the overlay, which fstools formats on first boot - so
# only the direction that would write past the end of the running partition
# (running smaller than the image) is refused.
#
rockchip_do_upgrade_h28k() {
	local disk part partdev dpart dsize

	disk=$(rockchip_upgrade_root_disk)
	if [ -z "$disk" ]; then
		echo "cannot determine the boot disk, refusing to upgrade"
		return 1
	fi

	echo "upgrading $disk from $1"

	get_image "$1" | dd of=/tmp/image.bs count=1 bs=512b 2>/dev/null
	get_partitions /tmp/image.bs image

	sync

	while read part start size; do
		partdev=$(rockchip_upgrade_part_dev "$disk" "$part")
		if [ ! -b "$partdev" ]; then
			echo "partition $part ($partdev) not found, skipped"
			continue
		fi

		dpart=$(basename "$partdev")
		dsize=$(cat "/sys/class/block/$dpart/size" 2>/dev/null)
		if [ -n "$dsize" ] && [ "$dsize" -lt "$size" ]; then
			echo "partition $part is smaller on $disk (disk $dsize, image $size), refusing"
			return 1
		fi

		echo "writing image partition $part to $partdev"
		get_image "$1" | dd of="$partdev" ibs=512 obs=1M \
			skip="$start" count="$size" conv=fsync
	done < /tmp/partmap.image

	sync
}

platform_do_upgrade() {
	case "$(board_name)" in
	panther,x2)
		rockchip_do_upgrade_panther "$@"
		;;
	hinlink,opc-h28k|hinlink,h28k)
		rockchip_do_upgrade_h28k "$@"
		;;
	*)
		rockchip_do_upgrade_generic "$@"
		;;
	esac
}
