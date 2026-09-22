# H28K (RK3528) bootloader: build and usage

This directory holds everything needed to unbrick or reflash the board from
maskrom over USB.  The bootloader that the images actually boot is built by two
OpenWrt packages that live in `package/boot/`.

---

## 1. The U-Boot package: `package/boot/uboot-rockchip-rk3528`

U-Boot **2026.07** for RK3528 boards, producing the device **`generic-rk3528`**
(the H28K device definition selects it with `UBOOT_DEVICE_NAME := generic-rk3528`).

It is kept separate from `package/boot/uboot-rockchip` on purpose:

* `uboot-rockchip` is U-Boot **2021.01** and is used by the existing
  RK3328 / RK3399 devices.  It cannot build RK3528 at all.
* 2026.07 needs the newer `include/u-boot.mk` hooks: `UBOOT_CUSTOMIZE_CONFIG`
  and `ROCKCHIP_TPL` support.  Those hooks are a 12-line change in
  `include/u-boot.mk` (also part of this port).

### What it depends on

`rkbin-rk3528` (built from `package/boot/rockchip-rkbin`, the Rockchip rkbin
fragments vendored into that package):

| File in rkbin                       | Role                          |
|-------------------------------------|-------------------------------|
| `rk3528_bl31_v1.17.elf`             | ATF (BL31) linked into u-boot.itb |
| `rk3528_ddr_1056MHz_v1.10.bin`      | TPL, DDR init                 |

### What it produces

| Artifact                                   | Where it lands                                          |
|--------------------------------------------|---------------------------------------------------------|
| `generic-rk3528-idbloader.img`             | `staging_dir/target-*/image/`                           |
| `generic-rk3528-u-boot.itb`                | `staging_dir/target-*/image/`                           |
| the same two, inside the flash image       | `idbloader` at **sector 64**, `u-boot.itb` at **sector 0x4000** |

### How to build

```sh
# the whole firmware (what you normally want)
make -j4

# or just the bootloader chain, incrementally
make package/boot/rockchip-rkbin/compile V=s
make package/boot/uboot-rockchip-rk3528/compile V=s

# results
ls -l staging_dir/target-*/image/generic-rk3528-*
strings build_dir/target-*/u-boot-generic-rk3528/u-boot-2026.07/u-boot | grep -m1 U-Boot
```

### Two details worth knowing

1. **The dtc override matters.**  `include/u-boot.mk` points `DTC` at the
   kernel's dtc, which on this tree is **1.5.0**; the DTB it produces does
   **not** boot this board.  The package therefore overrides `DTC` on the make
   command line to U-Boot's own **1.7.2** dtc, which produces the DTB the
   working images were built with.  The later assignment on the command line
   wins, so the override takes effect.

2. **`CMD_EXT4_WRITE` is enabled on purpose.**  The board has no UART, so
   `boot.scr` uses `ext4write` to drop breadcrumb files into the ext4 boot
   partition (they can be read back in maskrom mode).  Without the command the
   breadcrumbs cannot be written.

### Changing the U-Boot configuration

`UBOOT_CUSTOMIZE_CONFIG` in the package Makefile is applied after the defconfig:

```make
UBOOT_CUSTOMIZE_CONFIG := \
	--disable TOOLS_MKEFICAPSULE \
	--set-str MKIMAGE_DTC_PATH $(PKG_BUILD_DIR)/scripts/dtc/dtc \
	--enable CMD_EXT4_WRITE
```

Add `--enable/--disable/--set-str` lines there (U-Boot's own
`scripts/config` syntax) rather than editing the defconfig, so the change stays
visible in one place.

---

## 2. The maskrom loader in this directory (a *different* thing)

`rk3528_loader_miniall.bin` is **not** the storage bootloader above.  It is a
loader for **maskrom mode**: the BootROM cannot bring up DDR by itself, so the
loader (DDR init + usbplug + SPL) has to be pushed into RAM over USB before
anything can be written to eMMC.

It is produced by Rockchip's `boot_merger` from the fragments in
`package/boot/rockchip-rkbin/files/` (`RKBOOT/RK3528MINIALL.ini`).  Two
containers can be built from the same parts:

| Container                          | Accepted by                                    |
|------------------------------------|------------------------------------------------|
| `rk3528_loader_miniall.bin` (BOOT) | the Linux tools: `rkdeveloptool`, `upgrade_tool` |
| `rk3528_spl_loader_*.bin` (LDR / RKNS) | Rockchip's Windows RKDevTool               |

The Linux tools verify the `BOOT` tag and their own CRC32, and reject the
`LDR ` container outright (`Open loader failed` / `Opening loader failed`).

---

## 3. Flashing from maskrom

Board into maskrom (hold the maskrom key/pad while powering up), then:

```sh
./flash-maskrom.sh <image.img>          # uses upgrade_tool + the BOOT loader
```

or by hand:

```sh
./upgrade_tool LD                        # expect Mode=Maskrom
./upgrade_tool DB rk3528_loader_miniall.bin
./upgrade_tool WL 0 <image.img>          # whole disk, from LBA 0
./upgrade_tool RD
```

`upgrade_tool` is statically linked and is the safer choice;
`rkdeveloptool` is dynamically linked and needs libusb.

**There is no UART on this board.**  Verify a flash by pinging the box, or read
the boot partition back:

```sh
./rkdeveloptool db rk3528_loader_miniall.bin
./rkdeveloptool rl 65536 32768 bootp.img     # start sector 65536 (=32 MiB), 32768 sectors (=16 MiB)
debugfs -R "ls -l /" bootp.img               # kernel.img / rockchip.dtb / boot.scr
```

A single whole-disk write of a large image is **not** reliable on this board:
a 168 MiB image dies part-way with `Write LBA failed!`, while the same image
written in chunks of a few tens of MiB completes every time.  `flash-maskrom.sh`
therefore writes the image in chunks (default **40 MiB**, override with
`CHUNK_MB=<n>`) and retries a chunk once if the tool reports a failure.  The
vendor tool has no file-offset option, so each chunk is cut out with `dd` and
written at the LBA matching its offset (1 MiB = 2048 sectors).

Before touching the board the script also sanity-checks the image, so a wrong
file is caught while the eMMC is still intact:

* sector 64 must hold an `RKNS` idbloader (i.e. it really is a Rockchip boot image);
* the device tree inside must name this board (`opc-h28k`);
* the file has to fit the board's 7.28 GiB eMMC.

The whole check costs ~40 ms on a 168 MiB image.  Set `SKIP_CHECKS=1` to bypass
it (for example when deliberately flashing something else).

---

## 4. Files here

```
flash-maskrom.sh              wrapper: LD -> DB -> WL 0 -> RD
rk3528_loader_miniall.bin     maskrom loader, BOOT container (461 KB)
rkdeveloptool                 Linux flashing tool (dynamic, needs libusb)
upgrade_tool                  Linux flashing tool (static, preferred)
```
