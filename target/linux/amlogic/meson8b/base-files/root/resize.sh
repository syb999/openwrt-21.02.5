#!/bin/bash
#
# Grow the rootfs partition to the end of the medium on first boot.
# Works for both media this image can boot from:
#   eMMC -> root=/dev/mmcblk1p2   (mmc1 in the device tree)
#   SD   -> root=/dev/mmcblk0p2   (mmc0 in the device tree)

root="$(sed -n 's/.*root=\([^ ]*\).*/\1/p' /proc/cmdline)"
[ -b "$root" ] || root=/dev/mmcblk1p2

disk="${root%p*}"
[ "$disk" = "$root" ] && disk="${root%[0-9]}"

parted "$disk" resizepart 2 100%
losetup /dev/loop0 "$root"
resize2fs -f /dev/loop0
echo "# Put your custom commands here that should be executed once
# the system init finished. By default this file does nothing.

exit 0">/etc/rc.local
rm -rf /root/resize.sh && reboot
