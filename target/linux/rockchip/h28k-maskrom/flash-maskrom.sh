#!/bin/sh
#
# Flash an RK3528 board (HINLINK OPC-H28K) from maskrom over USB.
#
# The board has to be in maskrom mode (Rockchip BootROM USB mode).  The loader
# is pushed into RAM first, because the BootROM itself cannot bring up DDR -
# without it nothing can be written to eMMC/SD.
#
# Loader choice matters:
#   rk3528_loader_miniall.bin        classic "BOOT" container   <- used here
#   rk3528_spl_loader_*.bin          new "LDR "/RKNS container  (RKDevTool/Windows)
# The Linux tools (rkbin's rkdeveloptool, upgrade_tool) verify the "BOOT" tag
# and their own CRC_32, so they reject the "LDR " container with
# "Open loader failed" / "Opening loader failed".  Both containers are built
# in-tree from the same DDR + usbplug + spl parts.
#
# Usage:
#   ./flash-maskrom.sh <image.img>
#
# Environment overrides:
#   UPGRADE_TOOL=/path/to/upgrade_tool
#   LOADER=/path/to/rk3528_loader_miniall.bin
#   CHUNK_MB=40          write the image in chunks of this many MiB

set -e

here=$(cd "$(dirname "$0")" && pwd)
tool=${UPGRADE_TOOL:-$here/upgrade_tool}
loader=${LOADER:-$here/rk3528_loader_miniall.bin}
image=$1
chunk=${CHUNK_MB:-40}

if [ -z "$image" ]; then
	echo "usage: $0 <image.img>" >&2
	exit 1
fi
[ -r "$image" ] || { echo "cannot read image: $image" >&2; exit 1; }

# Sanity-check the image before anything touches the board:
#   * sector 64 must hold an "RKNS" idbloader (i.e. it is a Rockchip boot image);
#   * the device tree inside must name this board ("opc-h28k"), so flashing a
#     different board's image is caught here instead of after the eMMC is gone;
#   * it has to fit the board's eMMC (7.28 GiB).
# The scan costs ~40 ms on a 168 MiB image.  Set SKIP_CHECKS=1 to bypass.
if [ -z "$SKIP_CHECKS" ]; then
	tag=$(dd if="$image" bs=512 skip=64 count=1 2>/dev/null | head -c 4)
	[ "$tag" = "RKNS" ] || {
		echo "[!] sector 64 is not an RKNS idbloader (got '$tag')" >&2
		echo "    this does not look like a Rockchip boot image" >&2
		exit 1
	}
	grep -aq "opc-h28k" "$image" || {
		echo "[!] no opc-h28k device tree inside '$image' - wrong board?" >&2
		exit 1
	}
	image_size=$(stat -c %s "$image")
	[ "$image_size" -le 7800000000 ] || {
		echo "[!] image is $image_size bytes - larger than the 7.28 GiB eMMC" >&2
		exit 1
	}
	echo "   image checks passed: $image_size bytes, RKNS idbloader, H28K device tree"
fi
[ -r "$loader" ] || { echo "loader not found: $loader" >&2; exit 1; }
[ -x "$tool" ] || chmod +x "$tool" 2>/dev/null || true
[ -x "$tool" ] || { echo "tool not executable: $tool" >&2; exit 1; }

# The vendor tools handle relative paths best, so work from their directory.
cd "$here"
loader_name=$(basename "$loader")

echo "== step 1/4: list rockusb devices (expect the board in maskrom)"
lsusb 2>/dev/null | grep -i 2207 && echo "   (USB 2207:xxxx = Rockchip device present)" \
	|| echo "   [!] no Rockchip USB device (VID 2207) seen by lsusb"
"$tool" LD

echo "== step 2/4: download loader (DDR init + usbplug): $loader_name"
"$tool" DB "$loader_name"

echo "== step 3/4: write image at LBA 0 (in ${chunk} MiB chunks)"
echo "   idbloader @sector 64, u-boot.itb @sector 0x4000, MBR + kernel + rootfs inside"
# A single WL of the whole image is not reliable on this board/tool pair: a
# 168 MiB image dies part-way with "Write LBA failed!" (measured around 24%).
# Writing the same image in smaller chunks completes every time.  The vendor
# tool has no file-offset option, so each chunk is cut out with dd and written
# at the LBA matching its offset (1 MiB = 2048 sectors).
total=$(stat -c %s "$image")
off=0
while [ "$off" -lt "$total" ]; do
	dd if="$image" of="$here/.flashpart" bs=1M skip=$((off / 1048576)) count=$chunk 2>/dev/null
	echo "   chunk @ $((off / 1048576)) MiB (LBA $((off / 512)))"
	if ! "$tool" WL $((off / 512)) "$here/.flashpart"; then
		echo "   [!] chunk failed, retrying once" >&2
		"$tool" WL $((off / 512)) "$here/.flashpart"
	fi
	off=$((off + chunk * 1048576))
done
rm "$here/.flashpart"

echo "== step 4/4: reset the board"
"$tool" RD

echo "done. NOTE: this board has NO exposed UART, so there is no console to watch."
echo "Verify by pinging it on the LAN, or read the boot partition back in maskrom:"
echo "    rkdeveloptool db $loader_name && rkdeveloptool rl 65536 32768 bootp.img"
echo "    debugfs -R 'ls -l /' bootp.img        # kernel.img / rockchip.dtb / boot.scr"
