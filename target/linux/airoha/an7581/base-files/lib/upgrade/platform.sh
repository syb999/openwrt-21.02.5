RAMFS_COPY_BIN='fitblk fit_check_sign ubiattach ubiupdatevol ubirsvol'

REQUIRE_IMAGE_METADATA=1

# This kernel's UBI sysfs layout puts the device at /sys/class/ubi/ubiN
# (bearing mtd_num and the ubiN_M volume dirs).  nand_find_ubi() and
# nand_find_volume() from /lib/upgrade/nand.sh look under
# /sys/devices/virtual/ubi/, which does not exist here, so they always return
# nothing.  airoha_require_ubi_layout()
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
	[ "$(cat "/sys/class/ubi/$fip_volume/data_bytes")" -gt 0 ] || {
		echo "The fip volume does not contain BL31/U-Boot; install the boot chain first."
		return 1
	}
}

platform_check_image() {
	local board=$(board_name)

	[ "$#" -gt 1 ] && return 1

	case "$board" in
	gemtek,xg2010g|\
	fiberhome,hg5382a|\
	fiberhome,hg5585f-ct|\
	fiberhome,hg5585f-cu|\
	unionman,ung00a|\
	znxt,zn504xg-d|\
	znxt,zn515xg-d|\
	nokia,xg-040g-md-ubi|\
	nokia,xg-040g-tf-ubi|\
	quantum,q1000k-ubi)
		fit_check_image "$1"
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
	local ubidev vol dev need have image="$1"
	local fipvol

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

		need="$(stat -c %s "$image")"
		have="$(cat "/sys/class/ubi/$vol/data_bytes" 2>/dev/null || echo 0)"
		echo "FIT volume $vol: image $need bytes, volume $have bytes"

		if [ "$need" -gt "$have" ]; then
			# Grow, do not recreate: round up to a whole number of LEBs.
			local leb ebs
			ebs="$(cat "/sys/class/ubi/$vol/usable_eb_size")"
			leb=$(( (need + ebs - 1) / ebs ))
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
		gemtek,w1700k-ubi|\
		quantum,q1000k-ubi)
			fit_do_upgrade "$1"
			;;
		gemtek,xg2010g|\
		fiberhome,hg5382a|\
		fiberhome,hg5585f-ct|\
		fiberhome,hg5585f-cu|\
		unionman,ung00a|\
		znxt,zn504xg-d|\
		znxt,zn515xg-d)
			airoha_require_ubi_layout factory && fit_do_upgrade "$1"
			;;
		nokia,xg-040g-md-ubi|\
		nokia,xg-040g-tf-ubi)
			airoha_require_ubi_layout bosa ri && fit_do_upgrade "$1"
			;;
		*)
			nand_do_upgrade "$1"
			;;
	esac
}
