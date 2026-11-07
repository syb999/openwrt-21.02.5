# Amlogic GXBB/GXL OpenWrt boot script.
#
# Entered from the stock Amlogic u-boot (2015.01) through its own environment:
#   start_autoscript = usb start; start_usb_autoscript; mmcinfo; start_mmc_autoscript
#   start_mmc_autoscript = fatload mmc 0 1020000 s905_autoscript; autoscr 1020000
#   start_usb_autoscript = fatload usb <n> 1020000 s905_autoscript; autoscr ...
#
# Those use "fatload <dev> <addr> <file>" without a partition number, which the
# stock u-boot resolves to the whole device; a card with an MBR needs the
# explicit ":1".  Both are tried, SD first and USB afterwards, and the root
# device is derived from whichever medium actually carried the kernel.

# Addresses taken from the stock environment itself: loadaddr=0x1080000 is
# where the vendor u-boot always puts a kernel, dtb_mem_addr=0x1000000 is
# where it puts the FDT.  Loading below 0x1000000 aborts (BL30/BL31 live at
# 0x01000000..0x01400000 and the region under them is not accessible from
# u-boot), which is why the meson8b-style 0x00208000 must not be used here.
setenv krn 0x01080000
setenv dtbaddr 0x1000000
setenv condev "console=ttyAML0,115200n8 no_console_suspend consoleblank=0"
setenv bdev ""
setenv rootdev ""

if fatload mmc 0:1 ${krn} uImage; then setenv bdev "mmc 0:1"; setenv rootdev "/dev/mmcblk0p2"; fi
if test "${bdev}" = ""; then if fatload mmc 0 ${krn} uImage; then setenv bdev "mmc 0"; setenv rootdev "/dev/mmcblk0p2"; fi; fi
if test "${bdev}" = ""; then if fatload usb 0:1 ${krn} uImage; then setenv bdev "usb 0:1"; setenv rootdev "/dev/sda2"; fi; fi
if test "${bdev}" = ""; then if fatload usb 0 ${krn} uImage; then setenv bdev "usb 0"; setenv rootdev "/dev/sda2"; fi; fi

if test "${bdev}" != ""; then if fatload ${bdev} ${dtbaddr} dtb; then setenv bootargs "root=${rootdev} rootwait rw ${condev} fsck.repair=yes net.ifnames=0"; if printenv mac; then setenv bootargs ${bootargs} mac=${mac}; elif printenv eth_mac; then setenv bootargs ${bootargs} mac=${eth_mac}; elif printenv ethaddr; then setenv bootargs ${bootargs} mac=${ethaddr}; fi; bootm ${krn} - ${dtbaddr}; fi; fi
echo "OpenWrt: no bootable medium found"
