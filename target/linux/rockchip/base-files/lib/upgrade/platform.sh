#!/bin/sh
#
# sysupgrade support for the rockchip target.
#
# The sysupgrade image is a full disk image (MBR + loader at sector 0x40 +
# u-boot.itb at 0x4000 + boot partition + rootfs partition).  Writing that
# image over the *running* disk from stage2 would also rewrite the bootloader
# areas, so instead this only replaces the two filesystem partitions:
#
#   p1 (sectors 65536..98303)  -> holding kernel.img, rockchip.dtb, boot.scr
#   p2 (sectors 131072..921599)-> squashfs rootfs + overlay
#
# The loader areas and the partition table are left untouched, which keeps the
# board bootable even if the transfer is cut short, and makes an update a
# matter of ~400 MB instead of a maskrom reflash.
#
# Layout constants mirror target/linux/rockchip/image/armv8.mk and the
# bootscript; if the image layout ever changes, change them here too.
#
# shellcheck shell=sh

LOADER_SECTOR=64		# idbloader (RKNS container) lives here
BOOT_START=65536
BOOT_SECTORS=32768
ROOT_START=131072
ROOT_SECTORS=790528

. /lib/functions.sh
. /lib/upgrade/common.sh

# Which disk did we boot from?  /rom is the squashfs of the rootfs partition;
# /proc/mounts reports it as /dev/root, so the symlink has to be resolved (the
# overlay sits on a loop device inside the same partition, which is no use
# here).  The eMMC does not have a fixed name - on this board it enumerates as
# mmcblk2 because the (dead) SD slot and the SDIO wifi also claim numbers - so
# the name must never be hard-coded.
rockchip_root_disk() {
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

	# Fallback: find the disk carrying this firmware's 16 MiB boot partition.
	for disk in /dev/mmcblk* /dev/sd* /dev/nvme*n*; do
		case "$disk" in
		*p[0-9]*|[0-9]) continue ;;
		esac
		[ -b "$disk" ] || continue
		part=$(rockchip_part_dev "$disk" 1)
		[ -b "$part" ] || continue
		[ "$(cat /sys/class/block/$(basename "$part")/size 2>/dev/null)" = "$BOOT_SECTORS" ] || continue
		echo "$disk"
		return 0
	done

	return 1
}

# /dev/mmcblk2 -> /dev/mmcblk2p1, /dev/sda -> /dev/sda1
rockchip_part_dev() {
	local disk="$1" nr="$2"

	case "$disk" in
	/dev/mmcblk*|/dev/nvme*|/dev/loop*) echo "${disk}p${nr}" ;;
	*)                                   echo "${disk}${nr}" ;;
	esac
}

platform_check_image() {
	local disk

	disk=$(rockchip_root_disk)
	if [ -z "$disk" ]; then
		echo "cannot determine the boot disk, refusing to upgrade"
		return 1
	fi

	# Cheap sanity check on the stream, so that a truncated or foreign file
	# fails here rather than halfway through the write.  The loader container
	# magic "RKNS" sits at sector 64 - note this is *not* the boot partition
	# offset, that one starts with an ext4 superblock.
	if ! get_image "$1" | dd bs=512 skip=$LOADER_SECTOR count=1 2>/dev/null | \
	     grep -q 'RKNS'; then
		echo "image does not look like a rockchip image (no loader magic)"
		return 1
	fi

	return 0
}

platform_do_upgrade() {
	local disk part

	disk=$(rockchip_root_disk)
	[ -n "$disk" ] || {
		echo "cannot determine the boot disk"
		return 1
	}

	echo "upgrading $disk from $1"

	part=$(rockchip_part_dev "$disk" 1)
	if [ -b "$part" ]; then
		echo "writing boot partition ($part)"
		get_image "$1" | dd bs=512 skip=$BOOT_START count=$BOOT_SECTORS of="$part" 2>/dev/null
	else
		echo "no boot partition ($part), skipping kernel/dtb/boot.scr update"
	fi

	part=$(rockchip_part_dev "$disk" 2)
	if [ -b "$part" ]; then
		echo "writing rootfs partition ($part)"
		get_image "$1" | dd bs=512 skip=$ROOT_START count=$ROOT_SECTORS of="$part" 2>/dev/null
	else
		echo "rootfs partition $part not found"
		return 1
	fi

	sync
}

platform_copy_config() {
	# The overlay lives in the rootfs partition and is handled by the generic
	# code (it is saved to /tmp before the write and restored afterwards).
	return 0
}

platform_pre_upgrade() {
	return 0
}
