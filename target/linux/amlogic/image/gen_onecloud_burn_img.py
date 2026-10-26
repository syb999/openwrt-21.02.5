#!/usr/bin/env python3
#
# Build an Amlogic USB Burning Tool image ("*.burn.img") for the Thunder
# OneCloud straight from the OpenWrt build tree.
#
# Layout (reverse engineered from hzyitc/AmlImg, both verified byte for byte
# against the images produced by the manual pipeline):
#
#   header  64 bytes, little endian:
#       crc32, version(2), magic(0x27B51956), size(u64), align(4),
#       item_count, reserved[36]
#       crc32 = ~zlib.crc32(file[4:])  (stored at offset 0)
#   items   576 bytes each (version 2), one per payload, in commands.txt order
#   data    payloads, 4 byte aligned
#
# The boot (FAT) and rootfs (ext4) partitions are stored as plain ("normal")
# images. They used to be converted to Android sparse images, but u-boot's
# sparse writer in this 2015 Amlogic u-boot does not advance the destination
# over DONT_CARE chunks: everything after the first all-zero 4 KiB block lands
# shifted, so the kernel image ended up with a "Bad Data CRC" even though the
# FAT metadata and the uImage header were intact. Plain images write
# sequentially and are what the bootloader/resource partitions already use.

import os
import struct
import sys
import zlib

AML_MAGIC = 0x27B51956
AML_VERSION = 2
HEADER_SIZE = 64
ITEM_SIZE_V2 = 576
IMG_TYPE_NORMAL = 0x00
IMG_TYPE_SPARSE = 0xFE

# Same order as the commands.txt of the known good image.
UBOOT_ITEMS = [
    ("USB", "DDR", "normal", "0.DDR.USB"),
    ("USB", "UBOOT_COMP", "normal", "1.UBOOT_COMP.USB"),
    ("ini", "aml_sdc_burn", "normal", "2.aml_sdc_burn.ini"),
    # aml-flash (Linux) 的 m8 分支在 check_file "$dtb_file" 处要求 image.cfg
    # 里存在 sub_type="meson" 的 dtb 项，随后 `mwrite <dtb> mem dtb`。
    ("dtb", "meson", "normal", "8.meson.dtb"),
    ("conf", "platform", "normal", "3.platform.conf"),
    ("PARTITION", "bootloader", "normal", "4.bootloader.PARTITION"),
    ("VERIFY", "bootloader", "normal", "5.bootloader.VERIFY"),
    ("PARTITION", "resource", "normal", "6.resource.PARTITION"),
    ("VERIFY", "resource", "normal", "7.resource.VERIFY"),
    ("PARTITION", "boot", "normal", "boot.fat"),
    ("PARTITION", "rootfs", "normal", "rootfs.ext4"),
]


def check_boot_scr(boot_fat):
    """确认 boot.fat 里的 boot.scr 真的是 uImage（0x27051956）。

    并行构建时三个镜像目标共用过同一个中间文件 boot.scr，mcopy 有机会读到
    刚被 truncate 的空文件，结果 FAT 里是个 941 字节全零的 boot.scr —— 开机时
    u-boot 只会打一句 "Wrong image format for autoscript" 然后掉到提示符。
    这里在打包前直接拦住，别再把坏镜像发出去。
    """
    with open(boot_fat, "rb") as f:
        bs = f.read(512)
        if len(bs) < 512 or bs[510:512] != b"\x55\xaa":
            sys.exit("%s: 不是 FAT 镜像" % boot_fat)
        bps = int.from_bytes(bs[11:13], "little")
        spc = bs[13]
        resv = int.from_bytes(bs[14:16], "little")
        nfats = bs[16]
        rootent = int.from_bytes(bs[17:19], "little")
        fatsz = int.from_bytes(bs[22:24], "little")
        root_off = (resv + nfats * fatsz) * bps
        data_off = (resv + nfats * fatsz + (rootent * 32 + bps - 1) // bps) * bps
        clu_sz = spc * bps
        f.seek(root_off)
        root = f.read(rootent * 32)
        for i in range(rootent):
            e = root[i * 32:(i + 1) * 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5:
                continue
            if e[0:8] == b"BOOT    " and e[8:11] == b"SCR":
                clu = int.from_bytes(e[26:28], "little")
                size = int.from_bytes(e[28:32], "little")
                f.seek(data_off + (clu - 2) * clu_sz)
                head = f.read(4)
                if head != b"\x27\x05\x19\x56":
                    sys.exit("boot.fat 里的 boot.scr 不是 uImage（%d 字节，头部 %s）—— "
                             "多半是并行构建把共享的中间文件写坏了" % (size, head.hex()))
                return size
    sys.exit("boot.fat 里找不到 boot.scr")


def pack(out, items):
    """items: list of (type, name, img_type, path)"""
    item_count = len(items)
    data_offset = HEADER_SIZE + ITEM_SIZE_V2 * item_count

    blobs = []
    for _t, _n, _it, path in items:
        with open(path, "rb") as f:
            blobs.append(f.read())

    offsets = []
    pos = data_offset
    for blob in blobs:
        offsets.append(pos)
        pos += len(blob)
        pos += (-pos) % 4

    with open(out, "wb") as f:
        f.write(b"\0" * HEADER_SIZE)
        for i, ((itype, name, imgt, _p), blob, off) in enumerate(zip(items, blobs, offsets)):
            reserved = bytearray(32)
            if itype == "PARTITION":
                reserved[0] = 1
            f.write(struct.pack("<IIQQQ", i, IMG_TYPE_SPARSE if imgt == "sparse" else IMG_TYPE_NORMAL,
                                0, off, len(blob)))
            f.write(itype.encode().ljust(256, b"\0"))
            f.write(name.encode().ljust(256, b"\0"))
            f.write(bytes(reserved))

        # payloads
        f.seek(0, os.SEEK_END)
        for blob, off in zip(blobs, offsets):
            assert f.tell() == off, (f.tell(), off)
            f.write(blob)
            f.write(b"\0" * ((-f.tell()) % 4))
        total = f.tell()

        f.seek(0)
        f.write(struct.pack("<IIIQII", 0, AML_VERSION, AML_MAGIC, total, 4, item_count))
        f.write(b"\0" * 36)

        f.flush()
        with open(out, "r+b") as rf:
            rf.seek(4)
            crc = 0
            while True:
                buf = rf.read(1 << 20)
                if not buf:
                    break
                crc = zlib.crc32(buf, crc)
            rf.seek(0)
            rf.write(struct.pack("<I", (~crc) & 0xFFFFFFFF))


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: %s <out.burn.img> <boot.fat> <rootfs.ext4> <uboot-dir>" % sys.argv[0])

    out, boot_fat, rootfs_ext4, uboot_dir = sys.argv[1:]

    check_boot_scr(boot_fat)

    items = []
    for itype, name, imgt, fname in UBOOT_ITEMS:
        if fname == "boot.fat":
            path = boot_fat
        elif fname == "rootfs.ext4":
            path = rootfs_ext4
        else:
            path = os.path.join(uboot_dir, fname)
        if not os.path.exists(path):
            sys.exit("missing input: %s" % path)
        items.append((itype, name, imgt, path))

    pack(out, items)


if __name__ == "__main__":
    main()
