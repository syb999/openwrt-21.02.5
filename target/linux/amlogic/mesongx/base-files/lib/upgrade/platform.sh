# SPDX-License-Identifier: GPL-2.0-or-later
#
# Thunder OneCloud: the kernel (uImage + boot.scr + dtb) lives on a FAT
# partition (p1) and the rootfs on an ext4 partition (p2), the same split that
# the USB burning image uses.  A sysupgrade image carries both partition images
# and they are written with dd; the partition table is never touched, so any
# layout that keeps "p1 = FAT boot, p2 = rootfs" works.
#

RAMFS_COPY_BIN='fw_printenv fw_setenv'
RAMFS_COPY_DATA='/etc/fw_env.config /var/lock/fw_printenv.lock'
REQUIRE_IMAGE_METADATA=0

ONECLOUD_ROOT_PART="$(sed -n 's#^.*root=\(/dev/mmcblk[0-9]*p[0-9]*\).*#\1#p' /proc/cmdline)"
[ -b "$ONECLOUD_ROOT_PART" ] || ONECLOUD_ROOT_PART=/dev/mmcblk0p2
ONECLOUD_BOOT_PART="${ONECLOUD_ROOT_PART%p*}p1"

onecloud_first_member() {
	local n

	n="$(zcat "$1" 2>/dev/null | dd bs=1 count=100 2>/dev/null | sed 's/[^[:print:]]//g')"
	[ -n "$n" ] || return 1
	printf '%s\n' "$n"
}

onecloud_is_tar() {
	[ "$(zcat "$1" 2>/dev/null | dd bs=1 skip=257 count=5 2>/dev/null)" = "ustar" ]
}

onecloud_image_dir() {
	local first

	first="$(onecloud_first_member "$1")" || return 1
	printf '%s\n' "${first%%/*}"
}

platform_check_image() {
	local magic first

	magic="$(dd if="$1" bs=2 count=1 2>/dev/null | hexdump -n 2 -e '1/1 "%02x"')"
	[ "$magic" = "1f8b" ] || {
		v "Invalid image (not gzip)"
		return 1
	}

	onecloud_is_tar "$1" || {
		v "Invalid image (not a tar archive)"
		return 1
	}

	first="$(onecloud_first_member "$1")" || {
		v "Invalid image (empty tar)"
		return 1
	}
	case "$first" in
		*/boot|*/boot/|*/root|*/root/) ;;
		*) v "Invalid image (first member is not boot/root)"; return 1 ;;
	esac

	return 0
}

platform_do_upgrade() {
	local conf_tar="${UPGRADE_BACKUP:-${CONF_TAR:-/tmp/sysupgrade.tgz}}"
	local dir BB tarbin dd img

	dir="$(onecloud_image_dir "$1")" || {
		echo "Image does not contain the boot and root partitions"
		return 1
	}

	[ "$(dd if="$ONECLOUD_BOOT_PART" bs=1 skip=54 count=3 2>/dev/null)" = "FAT" ] || {
		echo "Partition layout on this device does not match the image."
		echo "$ONECLOUD_BOOT_PART is not a FAT partition. Nothing was written."
		return 1
	}

	mkdir -p /tmp/upgbin
	cp -f /bin/busybox /tmp/upgbin/busybox 2>/dev/null || \
		cp -f /usr/bin/busybox /tmp/upgbin/busybox 2>/dev/null
	if [ -x /tmp/upgbin/busybox ]; then
		BB="/tmp/upgbin/busybox"
		tarbin="$BB tar"
		dd="$BB dd"
	else
		BB=""
		tarbin="tar"
		dd="dd"
	fi

	img="$1"

	echo "Writing boot partition to $ONECLOUD_BOOT_PART ..."
	${BB:-} zcat "$img" | $tarbin -xOf - "$dir/boot" > /tmp/oc-boot.img || {
		echo "Failed to unpack the boot partition"
		return 1
	}
	$dd if=/tmp/oc-boot.img of="$ONECLOUD_BOOT_PART" bs=64k conv=fsync || {
		echo "Failed to write the boot partition"
		return 1
	}
	${BB:-} rm -f /tmp/oc-boot.img

	echo "Writing rootfs to $ONECLOUD_ROOT_PART ..."
	${BB:-} zcat "$img" | $tarbin -xOf - "$dir/root" | $dd of="$ONECLOUD_ROOT_PART" bs=1M conv=fsync || {
		echo "Failed to write the rootfs"
		return 1
	}

	[ -n "$BB" ] && $BB sync

	if [ -f "$conf_tar" ]; then
		echo "注意：sysupgrade 后配置会重建（MAC/主机名/固定IP 自动恢复，手改项另备）"
	fi

	return 0
}
