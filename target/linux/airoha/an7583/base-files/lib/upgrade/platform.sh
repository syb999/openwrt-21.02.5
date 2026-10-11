RAMFS_COPY_BIN='fitblk fit_check_sign ubiattach ubiupdatevol ubirsvol'

REQUIRE_IMAGE_METADATA=1

# This kernel's UBI sysfs layout puts the device at /sys/class/ubi/ubiN
# (bearing mtd_num and the ubiN_M volume dirs).  nand_find_ubi() and
# nand_find_volume() from /lib/upgrade/nand.sh look under
# /sys/devices/virtual/ubi/, which does not exist here, so they always return
# nothing.  airoha_require_ubi_layout() then failed its first check and
# platform_do_upgrade()'s "&& fit_do_upgrade" short-circuited: sysupgrade
# reported success but wrote no kernel at all.  Verified on the device:
#   /sys/class/ubi/ubi0/mtd_num = 2, ubi0_5 name = "fit".
airoha_find_ubi() {
	local mtdnum="$(find_mtd_index "$1")" d b

	[ -n "$mtdnum" ] || return 1
	for d in /sys/class/ubi/ubi*; do
		[ -d "$d" ] || continue
		b="$(basename "$d")"
		case "$b" in *_) continue ;; esac
		if [ "$(cat "$d/mtd_num" 2>/dev/null)" = "$mtdnum" ]; then
			echo "$b"
			return 0
		fi
	done
	return 1
}

airoha_find_volume() {
	local d

	for d in /sys/class/ubi/${1}_*; do
		[ -d "$d" ] || continue
		if [ "$(cat "$d/name" 2>/dev/null)" = "$2" ]; then
			echo "$(basename "$d")"
			return 0
		fi
	done
	return 1
}

# nand.sh's nand_find_ubi()/nand_find_volume() only look under
# /sys/devices/virtual/ubi/, which does not exist on this kernel.  They are
# resolved at call time, so overriding them here also fixes nand_do_upgrade(),
# which is what actually writes the FIT (kernel) volume: without this it could
# not find the ubi device, so sysupgrade reported success while the kernel
# partition (volume "fit") stayed untouched.  Try the stock location first,
# then /sys/class/ubi/.
nand_find_ubi() {
	local ubidevdir ubidev mtdnum cmtdnum

	mtdnum="$(find_mtd_index "$1")"
	[ -n "$mtdnum" ] || return 1

	for ubidevdir in /sys/devices/virtual/ubi/ubi* /sys/class/ubi/ubi*; do
		[ -d "$ubidevdir" ] || continue
		ubidev="$(basename "$ubidevdir")"
		case "$ubidev" in *_) continue ;; esac
		cmtdnum="$(cat "$ubidevdir/mtd_num" 2>/dev/null)"
		if [ -n "$cmtdnum" ] && [ "$mtdnum" = "$cmtdnum" ]; then
			ubi_mknod "$ubidevdir"
			echo "$ubidev"
			return 0
		fi
	done
	return 1
}

nand_find_volume() {
	local ubidevdir ubivoldir dir

	for dir in /sys/devices/virtual/ubi /sys/class/ubi; do
		ubidevdir="$dir/$1"
		[ -d "$ubidevdir" ] || continue
		for ubivoldir in "$ubidevdir"/${1}_*; do
			[ -d "$ubivoldir" ] || continue
			if [ "$(cat "$ubivoldir/name" 2>/dev/null)" = "$2" ]; then
				ubi_mknod "$ubivoldir"
				echo "$(basename "$ubivoldir")"
				return 0
			fi
		done
	done
	return 1
}

airoha_require_ubi_layout()
{
	local ubidev volume fip_volume

	ubidev="$(airoha_find_ubi ubi)"
	[ -n "$ubidev" ] || {
		echo "UBI is not initialized; create the base volumes from U-Boot recovery."
		return 1
	}

	for volume in fip ubootenv ubootenv2 "$@"; do
		if ! airoha_find_volume "$ubidev" "$volume" >/dev/null; then
			echo "UBI base volume $volume is missing; repair the layout from U-Boot recovery."
			return 1
		fi
	done

	fip_volume="$(airoha_find_volume "$ubidev" fip)"
	fip_bytes="$(cat "/sys/class/ubi/$fip_volume/data_bytes" 2>/dev/null)"
	[ -n "$fip_bytes" ] || fip_bytes=0
	[ "$fip_bytes" -gt 0 ] || {
		echo "The fip volume does not contain BL31/U-Boot; install the boot chain first."
		return 1
	}
}

nokia_initial_setup()
{
	[ "$(rootfs_type)" = "tmpfs" ] || return 0

	fw_setenv bootcmd "flash read 0xc0000 0x800000 0x85000000; bootm 0x85000000"
}

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	nokia,xg-040g-mf-ubi)
		fit_check_image "$1"
		return $?
		;;
	*)
		nand_do_platform_check "$board" "$1"
		return $?
		;;
	esac

	return 0
}

# ---------------------------------------------------------------------------
# SAFE FIT upgrade: write the volume in place with ubiupdatevol.
#
# nand_do_upgrade() (the stock ubi path) deletes the target volume with
# ubirmvol and recreates it with ubimkvol.  On this board that is a one-way
# trip to a brick: the fit volume is ~120 LEBs and the free space on ubi0 is
# only a couple of LEBs, so if ubimkvol fails the volume is simply gone and
# BL2 can no longer find "fit" - exactly what happened on 2026-10-11.
#
# So never touch the volume layout: ubiupdatevol() rewrites the existing
# volume in place (UBI keeps the old contents until the new data is fully
# written and CRC-checked).  If the new image does not fit, grow the volume
# with ubirsvol() first - that resizes, it does not delete.
# ---------------------------------------------------------------------------
fit_do_upgrade() {
	local ubidev vol dev image="$1"
	local need=0 have=0 ebs=0

	export_fitblk_bootdev
	[ -n "$CI_METHOD" ] || {
		echo "Cannot determine the boot device; refusing to upgrade."
		return 1
	}

	case "$CI_METHOD" in
	ubi)
		ubidev="$(nand_find_ubi "$CI_UBIPART")"
		[ -n "$ubidev" ] || {
			echo "UBI device $CI_UBIPART not found."
			return 1
		}

		vol="$(nand_find_volume "$ubidev" "$CI_KERNPART")"
		[ -n "$vol" ] || {
			echo "UBI volume $CI_KERNPART not found; not creating it (refusing to touch the layout)."
			return 1
		}
		dev="/dev/$vol"

		# fitblk holds the volume; detach it so it can be written.
		[ -e /dev/fit0 ] && fitblk /dev/fit0
		[ -e /dev/fitrw ] && fitblk /dev/fitrw

		need="$(stat -c %s "$image" 2>/dev/null)"
		[ -n "$need" ] || need=0
		have="$(cat "/sys/class/ubi/$vol/data_bytes" 2>/dev/null)"
		[ -n "$have" ] || have=0
		ebs="$(cat "/sys/class/ubi/$vol/usable_eb_size" 2>/dev/null)"
		[ -n "$ebs" ] || ebs=126976
		echo "FIT volume $vol: image $need bytes, volume $have bytes"

		if [ "$need" -gt "$have" ]; then
			# Grow, do not recreate: round up to a whole number of LEBs.
			local leb
			echo "Image does not fit; resizing $vol to $leb LEBs ($(( leb * ebs )) bytes) with ubirsvol"
			ubirsvol "$ubidev" -N "$CI_KERNPART" -s "$(( leb * ebs ))" || {
				echo "ubirsvol failed; refusing to continue (volume left untouched)."
				return 1
			}
		fi

		echo "Writing image into $dev in place (ubiupdatevol)"
		ubiupdatevol "$dev" "$image" || {
			echo "ubiupdatevol failed."
			return 1
		}
		echo "FIT volume written: $(cat "/sys/class/ubi/$vol/data_bytes") bytes"
		return 0
		;;
	*)
		# Non-UBI boot devices keep the stock behaviour.
		[ -e /dev/fit0 ] && fitblk /dev/fit0
		[ -e /dev/fitrw ] && fitblk /dev/fitrw
		default_do_upgrade "$image"
		return $?
		;;
	esac
}

platform_do_upgrade() {
	local board=$(board_name)

	case "$board" in
	nokia,xg-040g-mf-ubi)
		airoha_require_ubi_layout bosa ri && fit_do_upgrade "$1"
		;;
	*)
		nand_do_upgrade "$1"
		;;
	esac
}

platform_pre_upgrade() {
	local board=$(board_name)

	case "$board" in
	nokia,xg-040g-mf)
		nokia_initial_setup
		;;
	*)
		;;
	esac
}
