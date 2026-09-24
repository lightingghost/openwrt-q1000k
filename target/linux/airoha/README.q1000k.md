# Quantum Fiber Q1000K

The `quantum_q1000k-ubi` profile targets the AN7581SIT with 512 MiB RAM,
512 MiB SPI NAND, one internal-switch 1 GbE LAN port (`lan1`) and one
RTL8261N 10 GbE LAN port (`lan2`, MDIO address 8, reset GPIO 27).
Both copper ports belong to `br-lan`. The optical PON connection is the
upstream interface; no copper WAN interface is configured.

The internal-switch port-1/PHY-9 mapping for `lan1` follows W1700K's first
GbE port (`gsw_port1`, called `lan3` there); W1700K's second GbE port is
`gsw_port2`/PHY 10. Comparing `q1000k_oem.dts` with `w1700k-oem.dts` confirms
the removal of external PHY 5 and the retention of PHY 8 with reset GPIO 27,
but neither OEM tree describes the internal switch ports. The Q1000K boot
log and HTTP-loader diagnostics establish GDM1 as the 1 GbE path. The supplied
September 12 OpenWrt boot log further shows PHY 9 attached to `lan1`, a
1 Gb/s full-duplex link and bridge forwarding. The GPIO 33/43 LEDs belong
to the 1 GbE jack and now follow `lan1`; the RTL8261N drives the 10 GbE
jack LEDs separately. The PHY node now requests TX and RX SerDes polarity
inversion, matching the Q1000K OEM driver and W1700K. These corrections
still require hardware validation; see the
[Ethernet investigation](../../../../Q1000K-ethernet-bringup.md), including
the LED configuration update needed when retaining existing settings.
On `q1000k-xgspon`, this profile includes the validated XGS-PON runtime,
its optical board wiring, OMCI, continuous service and LuCI. Both normal
image formats use the same PON implementation. See the
[normal PON image guide](XGSPON-NORMAL-IMAGES.q1000k.md) for build commands,
shared firmware, UBI factory data, private RAM overlays and remaining hardware checks.
`q1000k-dev` and the upstream support branch are unchanged.

## Build outputs

Select `CONFIG_TARGET_airoha_an7581_DEVICE_quantum_q1000k-ubi=y`,
`CONFIG_TARGET_ROOTFS_INITRAMFS=y` and `CONFIG_TARGET_ROOTFS_SQUASHFS=y`,
then run `make defconfig` and `make -j$(nproc)` in the OpenWrt checkout.
Select `CONFIG_PACKAGE_luci-ssl=y` to include the LuCI web interface, uHTTPd
and the required RPC modules. The `q1000k-build/user/q1000k/config.diff`
profile enables this bundle. Without it, a source/snapshot build can boot
and provide LAN/SSH access while having no web interface at `192.168.0.1`.
The images in `bin/targets/airoha/an7581/` are:

- `openwrt-airoha-an7581-quantum_q1000k-ubi-initramfs-recovery.itb`: Linux and
  a RAM root filesystem for bring-up/recovery. This is not an automatic
  installer and does not contain the persistent SquashFS root filesystem.
- `openwrt-airoha-an7581-quantum_q1000k-ubi-squashfs-sysupgrade.itb`: a FIT with
  a gzip-compressed kernel, Q1000K DTB, external SquashFS data and OpenWrt
  metadata. This is the firmware upload for the Q1000K HTTP loader and the
  image for subsequent OpenWrt sysupgrades.

Image prefixes can include the configured OpenWrt version. The old
`initramfs-uImage.itb` and `squashfs-sysupgrade.bin` outputs use the previous
recipe and must not be mistaken for these UBI images. Renaming an old image
does not convert its format.

The sysupgrade metadata supports `quantum,q1000k-ubi`, the first compatible
in the Linux DTS. The upgrade handler validates the FIT and uses
`fit_do_upgrade`, which resolves `chosen/rootdisk` to the `fit` volume in
the `ubi` partition. The HTTP-loader upload limit is 256 MiB; the image
recipe enforces this limit including metadata.

## Chainloader, installer and sysupgrade

A **chainloader** is the second bootloader executed by the vendor loader.
It runs each time the device boots and supplies capabilities missing from
the vendor loader, particularly loading OpenWrt from UBI and HTTP recovery.
The Q1000K implementation is in the separate `http-uboot-q1000k` project.
Its FIT wraps U-Boot itself; the sysupgrade FIT wraps Linux and its rootfs.

An **installer** performs the initial persistent migration: prepares the
OpenWrt UBI region, preserves or migrates device-specific factory data,
creates volumes and writes the initial OpenWrt firmware. It is a one-time
role, although recovery can repeat it and discard the existing installation.
It need not be a separate executable if the bootloader implements it.

[OpenWrt PR #17869](https://github.com/openwrt/openwrt/pull/17869) uses a
chainloader because the W1700K vendor BMT/BBT view of raw NAND can diverge
from Linux's view after bad-block changes. Keeping a relatively stable
chainloader in the raw kernel slot and putting frequently updated firmware
in UBI lets UBI handle firmware bad blocks. The PR then boots a separate
Linux initramfs installer to prepare UBI and install OpenWrt.

The [W1700K installer](https://github.com/hurrian/w1700k-ubi-installer/blob/46df1f5282f53c6757752354d7b6702f6fa54ec5/files/installer/install.sh)
reconstructs a Wi-Fi factory volume from DSD/EEPROM, creates environment and
recovery volumes, and installs the sysupgrade FIT. Its model detection,
EEPROM/fan assumptions and hard-coded MTD numbering do not match Q1000K.
Do not run the W1700K installer on Q1000K.

The current Q1000K HTTP loader implements the initial-install role through
its Update U-Boot operation. It installs the chainloader, prepares UBI,
copies factory records and creates `ubootenv`, `ubootenv2`, `factory`,
`recovery`, `fit` and `rootfs_data`. A separate Linux installer is not
required for that path. Initial installation erases existing UBI contents;
the firmware and recovery volumes initially contain only empty placeholders.
See the [published Q1000K board guide](https://github.com/lightingghost/http-uboot-q1000k/blob/4b34476f2e768fd051c077fa0ae4c0d779870c78/doc/board/airoha/q1000k.rst) and the
[local installation instructions](../../../../q1000k-pr/INSTALL.md).
First download the NAND data backup, then explicitly select **Install U-Boot
and prepare UBI**; the default update-only operation does not initialize UBI.

Ordinary HTTP Firmware Recovery replaces `fit` and `rootfs_data`, preserving
the other volumes. Recovery Image replaces `recovery` and `rootfs_data`.
Both discard existing OpenWrt configuration. Routine OpenWrt sysupgrade
uses the upgrade helpers and can retain the selected configuration backup.

## Flash layout and boot handoff

| Region | Start | Size | Linux access |
| --- | --- | --- | --- |
| Vendor bootloader, environment and DSD | `0x00000000` | 6 MiB | Read-only |
| Chainloader | `0x00600000` | 1 MiB | Read-only |
| OpenWrt UBI | `0x00700000` | 438 MiB | Writable |
| ART | `0x1bd00000` | 3 MiB | Read-only |
| Vendor BMT/BBT reservation | `0x1c000000` | 64 MiB | Read-only |

This agrees with the Q1000K HTTP loader. It preserves the OEM-declared ART
boundary, which differs from the W1700K layout. UBI volume names, rather
than fixed volume numbers, identify the firmware and overlay. Preserving
DSD/ART bytes does not by itself restore PON service. The existing
chainloader passes factory MAC addresses to Linux as described below.

OpenWrt uses a writable SPI-NAND master so UBI can update firmware and the
overlay. The `spi_nand` node has no whole-device `read-only` property.
Individual partition `read-only` flags are handled by the standard
fixed-partition parser. No BMT/NMBM mode
is selected in this DTS, so `mtk_bmt_attach()` returns without attaching a
mapping layer. This Linux policy is separate from the chainloader's NAND
write guard.

## Factory data and Ethernet MAC addresses

The W1700K installer copies selected OEM data into a 64 KiB `factory` UBI
volume: the MT7996 Wi-Fi EEPROM (`0x1e00` bytes at offset `0`), WAN MAC
(`0x5000`), LAN MAC (`0x6000`), fan ID (`0x7000`) and serial number
(`0x8000`). These offsets are within the new volume, not raw NAND offsets.
It reads the identity fields from DSD at raw offset `0x00400000` and searches
for the EEPROM starting at `0x00405000`. W1700K's OpenWrt DTS describes the
EEPROM and MAC locations using NVMEM cells in that volume.

Q1000K's HTTP installation leaves the original DSD region
(`0x00400000..0x00600000`) and ART (`0x1bd00000..0x1c000000`) in place.
Its installer also creates a 64 KiB `factory` volume containing WAN MAC
(`0x5000`), LAN MAC (`0x6000`), unit serial (`0x8000`), PON FSAN (`0x9000`),
and 513-byte GPON/XGS-PON calibration records (`0xa000`/`0xb000`, copied
from raw NAND `0x00411000`/`0x00412000`). Q1000K has no Wi-Fi or fan;
those reference fields remain zero. Initial installation reformats the
complete `ubi` region, so existing firmware/settings there are lost.
Ordinary firmware uploads preserve the factory volume. DSD and ART
preservation is not a claim that all ONT provisioning has been identified.

In `http-uboot-q1000k/board/airoha/an7581/an7581_rfb.c`, Q1000K uses
`xg2010g_get_dsd_ethaddrs()` to read the first 16 KiB of the read-only `dsd`
MTD partition, parse `lan_mac=` and `wan_mac=`, and validate both addresses.
`board_late_init()` sets the RAM environment's `ethaddr` and `eth1addr`.
At Linux boot, `ft_board_setup()` calls `xg2010g_fixup_fdt_macs()` to insert
both `mac-address` and `local-mac-address` into the outgoing device tree:

| Linux interface/path | Factory value |
| --- | --- |
| GDM1, `/soc/ethernet@1fb50000/ethernet@1` | `lan_mac` |
| `lan1`, internal-switch user port | Inherits GDM1's MAC through DSA |
| `lan2`, GDM4 at `/soc/ethernet@1fb50000/ethernet@4` | `lan_mac` |
| Disabled `wan`, GDM2 at `/soc/ethernet@1fb50000/ethernet@2` | `wan_mac` |

Thus both copper LAN ports currently share the factory LAN MAC; the loader
does not assign a separate incremented address to each jack. The Airoha
Linux driver reads the DT through `of_get_ethdev_address()`. Q1000K has no
Linux NVMEM MAC cells or board script that reads DSD directly. If the
chainloader fixup is skipped or fails and no valid DT MAC is available, the
driver falls back to a random MAC (DSA `lan1` then inherits GDM1's address).
This handoff is implemented in the existing chainloader and still needs
verification on hardware when booting Linux.

## Boot handoff

The current HTTP-loader environment sets `bootcmd=q1000k_boot || http_recovery`
and uses RAM-only environment storage. After installation, it attaches UBI
read-only and tries `fit`, then `recovery`, falling back to HTTP if neither
boots. Hold Reset during chainloader startup to force HTTP recovery. A
kernel that starts and later hangs cannot trigger this fallback. The supplied
September 12 log confirms automatic boot of `fit` with NAND writes locked,
followed by Linux mounting the writable UBIFS overlay.

The reserved `ubootenv` and `ubootenv2` volumes are placeholders in this
Q1000K implementation; unlike W1700K, the chainloader does not load or save
them. Its explicit installer handles initial chainloader installation and
vendor boot selection. Neither the sysupgrade image nor an ordinary HTTP
firmware upload changes that selection.
