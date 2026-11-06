#!/usr/bin/env python3
#
# Build a bootable SD-card image for Amlogic GXBB/GXL boards that keep their
# stock Amlogic u-boot:
#
#   sector 0            MBR, 1 partition table entry (type 0x0c = FAT32 LBA)
#   1 MiB               FAT boot partition (boot.scr, uImage, dtb, autoscripts)
#   1 MiB + boot_mb     ext4 rootfs partition (last, so it can be grown to the
#                       end of the card on first boot)
#
# Usage: gen_mesongx_sd_img.py <out.img> <boot.fat> <boot_mb> <rootfs.img> <root_mb>

import struct
import subprocess
import sys

SECTOR = 512
START_OFFSET = 1 * 1024 * 1024          # 1 MiB, keeps clear of any boot block


def chs(lba, heads=255, sectors=63):
    cyl = lba // (heads * sectors)
    if cyl > 1023:
        return bytes((0xfe, 0xff, 0xff))
    h = (lba // sectors) % heads
    s = lba % sectors + 1
    return bytes((h, s | ((cyl >> 2) & 0xc0), cyl & 0xff))


def entry(status, ptype, start_lba, sectors):
    return struct.pack('<B3sB3sII', status, chs(start_lba), ptype,
                       chs(start_lba + sectors - 1), start_lba, sectors)


def main():
    out, bootfat, boot_mb, rootimg, root_mb = sys.argv[1:6]
    boot_mb = int(boot_mb)
    root_mb = int(root_mb)

    boot_lba = START_OFFSET // SECTOR
    root_lba = boot_lba + boot_mb * 1024 * 1024 // SECTOR
    boot_sectors = boot_mb * 1024 * 1024 // SECTOR
    root_sectors = root_mb * 1024 * 1024 // SECTOR
    total = START_OFFSET + boot_mb * 1024 * 1024 + root_mb * 1024 * 1024

    mbr = bytearray(SECTOR)
    mbr[440:444] = struct.pack('<I', 0x5a5a1234)
    mbr[446:462] = entry(0x00, 0x0c, boot_lba, boot_sectors)
    mbr[462:478] = entry(0x00, 0x83, root_lba, root_sectors)
    mbr[510:512] = b'\x55\xaa'

    with open(out, 'wb') as f:
        f.truncate(total)
        f.seek(0)
        f.write(mbr)
        f.seek(START_OFFSET)
        with open(bootfat, 'rb') as b:
            f.write(b.read())
        f.seek(root_lba * SECTOR)
        with open(rootimg, 'rb') as r:
            f.write(r.read())

    # MBR signatures are known to the ecosystem; make the layout visible.
    subprocess.run(['/sbin/fdisk', '-l', out], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    print('%s: %d MiB (boot %d MiB @ %d, rootfs %d MiB @ %d)'
          % (out, total // 1024 // 1024, boot_mb, START_OFFSET,
             root_mb, root_lba * SECTOR))


main()
