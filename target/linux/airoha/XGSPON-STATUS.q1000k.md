# Q1000K XGS-PON implementation checkpoint

2026-09-12, branch `q1000k-xgspon`, based on `q1000k-dev` at
`b287be4f00581e04ddee27f1157a4897455078e5`.

**Factory access and development diagnostics work; optical service does not.**
The backend and adapted LuCI app build for AN7581. The factory reader and
complete RPC backend have passed live checks on the Q1000K. The vendor
kernel modules do not build as a complete usable stack, the EN7573 loader
is not integrated, and OMCI/service provisioning remains unimplemented.
No firmware image was flashed, PON module loaded, optical control changed,
or package persistently installed during these checks.

## Imported references

| Import | Local commit | Original source |
| --- | --- | --- |
| Complete `package/kernel/airoha-pon` source import | `b7ffe704f6` | [coolsnowwolf/lede f7fd86e](https://github.com/coolsnowwolf/lede/commit/f7fd86eaa58c29fed97da04ab219c74a835a9358) |
| Complete `luci-app-econet-xpon` source import | `17a58803da` | [AKoo7/openwrt e27eee8](https://github.com/AKoo7/openwrt/commit/e27eee81fddad217e111ce67bc7a8102b00b24b4), from [OpenWrt PR #24577](https://github.com/openwrt/openwrt/pull/24577) |

The import commits retain original authors and source trailers. Follow-up
changes move LuCI to `package/luci-app-econet-xpon`, use the existing LuCI
build system and replace its EN7528 backend with `q1000k-xgspon`. The new
backend follows the PR's UCI identity concept but uses the Q1000K factory
layout and a new status schema. The EN7528 driver/configuration package,
OMCI daemon and unrelated PR changes were not imported.

## Dependency matrix

| Component | Evidence and current state | Required next work |
| --- | --- | --- |
| Hardware | User reports AN7581SIT and two EN7573AN. Linux identifies `quantum,q1000k-ubi`. | Confirm the role and control wiring of each optical controller. |
| Factory identity/calibration | Read-only C reader finds the unique `factory` volume by name under the `ubi` MTD parent. Live serial, WAN MAC and all 513 calibration bytes match the original NAND backup. | Retain this data path for the loader; verify cold-boot and upgrade preservation when images are tested. |
| Original DSD fallback | Offline `--dsd-file` input is implemented and tested. Automatic raw MTD fallback is excluded. | Establish the logical NAND/BBT/BMT view before adding direct DSD fallback on older installations. |
| Optical GPIOs and I2C | OEM script shows separate selection and enable sequences. Linux exposes `i2c-0`; the documented ID read currently returns ENXIO. | Confirm mux, power, reset and TX-disable functions/polarities before requesting GPIOs or enabling a controller. |
| MD32 firmware | OEM program/data files are available locally and extracted with exact size/SHA-256 verification. | Feed the matching pair to the future loader; no firmware redistribution is included. |
| EN7573 loader | The LEDE package omits `en7572.ko`/LDDLA. A separate vendor source tree contains a reference loader; it declares `MODULE_LICENSE("Proprietary")`. | Resolve the implementation/source licensing path, port the protocol to Linux I2C and DT GPIOs, and validate both controller paths. No loader source was copied into the new GPL userspace package. |
| BSP/PHY modules | Hook declaration and AN7581 IOMUX table-size bugs fixed in patches. Normal Linux 6.18 compilation still fails. | Fix remaining C/kernel API issues and remove AN7583-only modules from the AN7581 dependency graph. |
| PON MAC | Diagnostic compilation reaches modpost with 41 unresolved symbols. Four resource accessors exist in an unbuilt BSP source; the others have no export in the imported tree. | Integrate resource ownership, interrupts, QDMA/FE, XG-PON events, packet metadata and management traffic. |
| OMCI | PR #24577's native daemon cross-compiles for AArch64, but has EN7528 transport, baseline-only OMCI and a DZS/H660GM-A MIB. It was not run or installed. | Implement/choose an AN7581 transport and Q1000K service model, extended OMCI as needed, correct errors and procd lifecycle. |
| LuCI/RPC | Airoha dependency chain builds. Unknown optical values remain JSON null; failed polling clears old data. Identity overrides are validated in UI and backend. | Browser QA after installation; add genuine driver/OMCI status only when those APIs exist. |
| Experimental builds | Optional diagnostics configuration fragment provided. Normal builder remains on `q1000k-dev`. | Enable a full PON image only after loader, kernel and service integration pass their gates. |

The reference loader was inspected at
[airoha_xpon_en757x 950199a](https://github.com/Sirherobrine23/airoha_xpon_en757x/tree/950199a8de6b75e76906a7c1b39b7a9a3e2913f9/v2/lddla).
It reads the controller family ID at I2C address `0x51`, register `0x0408`,
expecting `0x1388`. Register addresses are two bytes, most significant byte
first; the two data bytes form a little-endian word. This is a family check,
not proof of which EN7573AN is selected. The loader uses MD32 PM/DM registers
at `0x3000` through `0x3018` on address `0x50`, and consumes a 512-byte BOB
payload. The OEM's full 513-byte calibration record is preserved by our
reader; no padding, truncation or substitution is done at the storage layer.
The reference loader's firmware hashes differ from the local OEM pair, so
its bundled firmware must not be substituted just because filenames match.

The alternative
[Airoha kernel xPON source at 2e2cf91](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c)
was also inspected. Its implementation covers GPON/EPON; its resource-size
comment explicitly leaves XGSPON for later. It is an architectural reference,
not an already working AN7581 XGS-PON substitute.

## Live hardware observations

SSH checks used the user-provided `root@192.168.1.1`. The running image reports
Linux `6.18.44`, OpenWrt revision `r36173+21-7e7d8a4d19`. The factory volume
is currently `/dev/ubi0_2`, with 65536 data bytes, on MTD 2 named `ubi`.
The reader discovers these values; neither number is hardcoded. Its FSAN
at `0x9000`, binary WAN MAC at `0x5000` and XGS-PON record at `0xb000`
match the original DSD data. Identifiers and calibration are omitted here.

The live RPC check ran the built AArch64 reader, production shell backend,
CLI and rpcd dispatcher under a temporary `/tmp` directory. Only their
program paths were redirected to that directory. Real UCI, jshn, factory,
sysfs and firmware paths were used. The result reported valid factory data,
both PON modules absent, both firmware files unverified, and all optical
states unavailable. The temporary files were removed afterward.

The board has one 64-line GPIO controller. PON candidate offsets 8, 9, 10,
11, 45 and 46 are not requested by the current kernel. The OEM script uses
global numbers 461/462 for mode selection, 488/489 for enable controls and
490/491 for LOS. Its preceding GPIO-mode commands associate these with
local offsets 45/46, 8/9 and 10/11 respectively; this is a software-derived
mapping, not a confirmed schematic. OEM XGS selection uses 461/462 high,
then 488 low and 489 high. No such writes were made on OpenWrt.

I2C adapter `1fbf8000.i2c0` exists at `/sys/bus/i2c/devices/i2c-0`, with
`/dev/i2c-0`; absence of `/sys/class/i2c-adapter` alone does not mean I2C
is unavailable. The driver reports 100 kHz. One combined address-pointer
write/read transaction (`0x51`: write `04 08`, read two bytes) returned
ENXIO. No register data, GPIO, mux, power or reset was changed. This result
cannot distinguish an unpowered controller, an unselected path or another
wiring issue. It does not prove defective hardware. Successful detection
of both chips is still an unmet bench milestone.

## Kernel audit

`kmod-airoha-xpon-en757x` is gated by `BROKEN`, remains unselected and has
no autoload entry. `KBUILD_MODPOST_WARN=1` was removed from the MAC build
command. The package must not produce a nominally successful build
with unresolved symbols. No compatibility no-ops were added to satisfy
missing runtime interfaces.

The two Q1000K follow-up patches fix the `__ECNT_HOOK` declaration mismatch
and unused local, and derive the IOMUX table's bound from its actual size
(AN7581 has nine initializers, versus seven on the other branch). All
package patches apply to freshly prepared source. Normal compilation still
fails on vendor missing prototypes and other warnings treated as errors.

For diagnosis only, separate direct kernel builds used `KCFLAGS=-Wno-error`
while keeping modpost failures fatal. This is not a shipping build setting.
The BSP reached module linking; the PHY still failed on implicit declarations,
pointer-to-integer assignments, an incompatible kthread entry function and
value returns in void functions. Examples include `delay1ms`,
`XPON_DIG_ref_release`, `SET_FORCE_GPIO32_EN`, missing `vmalloc`/`vfree`
declarations and shared PHY command prototypes. No PHY symbol table was
available for the MAC check.

The MAC object built in that diagnostic configuration but failed modpost.
Comparing its undefined references against the target kernel and BSP export
tables gives these 41 missing symbols (excluding kbuild's `__this_module`):

| Integration area | Unresolved symbols |
| --- | --- |
| PON resource owner | `get_xpon_data`, `set_xpon_data`, `get_xpon_dev`, `get_xpon_irq` |
| Frame engine | `get_frame_engine_data`, `set_frame_engine_data` |
| WAN/QDMA and offload | `macSend`, `qdma_wan_fwd_timer`, `dropCpuTxPktsFlag`, `storm_ctrl_shrehold_wan`, `is_hwnat_dont_clean`, `wan_speed_test_hook` |
| Factory/flash/OEM identity | `GetMacAddr`, `get_ethaddr`, `get_onutype`, `flash_base`, `ranand_read_byte`, `spi_type` |
| Vendor command API | `cmd_register`, `cmd_unregister`, `subcmd` |
| Legacy kernel API | `random32` |
| PON event/timing/OMCI | `XGPON_MAC_EVENT_HANDLER`, `gpon_tod_adjust`, `omciIkIdxExchange`, `omciMicErrSwCnt` |
| NG-PON control state | `ng2_ignore_disable`, `ng2_man_set_09`, `ng2_mon_not_gnt`, `ng2_no_rollback`, `ng2_o4_to_09`, `ng2_o8_to_05`, `ng2_tun_resp_key` |
| Vendor diagnostic/control state | `drop_print_flag`, `masko_on_off`, `max_cnt`, `rdk_gtc_dbg`, `rdk_mic_err_dbg`, `sw_resync_flag`, `xgpon_fast_mode_flag`, `xpon_mac_print_open` |

The four resource functions are exported by `bsp/core/ecnt_xpon.c`, which
the imported BSP Makefile does not build. Merely adding it is insufficient:
it expects an `econet,ecnt-xpon` DT binding and must share resources correctly
with the existing upstream Ethernet/PCS drivers. None of the other 37 symbols
has an export in the imported source, including the unbuilt PHY sources.
Some are obsolete optional test hooks, while others are required runtime
interfaces; each call site needs a real port or removal of its unsupported
feature. Turning them all into zero-return stubs would not provide service.

The imported compatibility patch also returns success from flow-mapping
hooks without programming hardware and stores vendor metadata in `skb->cb`.
These remain known integration gaps even after symbol resolution. The
unconditional AN7583 combo-PHY module needs separate review on AN7581.

## Build and use the diagnostics

Use an existing Q1000K build checkout with its feeds and toolchain prepared:

```sh
test "$(git branch --show-current)" = q1000k-xgspon
git merge-base --is-ancestor b287be4f00581e04ddee27f1157a4897455078e5 HEAD
make -j8 package/network/utils/q1000k-xgspon/compile CONFIG_PACKAGE_q1000k-xgspon=m V=s
make -j8 package/luci-app-econet-xpon/compile CONFIG_PACKAGE_luci-app-econet-xpon=m CONFIG_PACKAGE_q1000k-xgspon=m V=s
```

Both commands pass using the local GCC 14.4.0 musl toolchain. Outputs are
`bin/packages/aarch64_cortex-a53/base/q1000k-xgspon-1.apk` and
`bin/packages/aarch64_cortex-a53/base/luci-app-econet-xpon-1-r2.apk`.
LuCI depends on `luci-base` and `q1000k-xgspon`; it has no dependency on the
EN7528 kernel package or the broken vendor AN7581 package.

For a diagnostics image, add [q1000k-xgspon.config](q1000k-xgspon.config) to
an existing `quantum_q1000k` configuration on this branch and run
`make defconfig`. Pin the checkout revision for any shared test image. This
fragment does not select PON kernel modules. The normal `q1000k-build`
repository/settings were not changed. No full XGS-PON image was produced.

After installing the diagnostics packages, the UI is Network → XGS-PON.
The CLI provides:

```sh
q1000k-xgspon status
q1000k-xgspon validate
q1000k-xgspon prepare
```

`status` returns schema version 1; unknown LOS, registration, OMCI, service
readiness and optical measurements are null. `validate` checks the selected
factory/override serial and MAC. Blank `/etc/config/q1000k-xgspon` overrides
use this unit's factory data. `prepare` requires valid factory calibration,
identity and the exact OEM firmware pair, then stages the 513-byte record
in a new mode-0700 RAM directory and prints that path. It does not activate
optics. Remove that temporary directory when finished. `start`, `restart`
and `reload` explicitly fail until optical integration exists; no init
service is installed yet.

To supply local firmware from the user's extracted OEM image:

```sh
python3 scripts/q1000k-extract-pon-firmware.py \
  --squashfs ../QKX001-06.00.44.00.bin_extract/3566568-40229864.squashfs_v4_le \
  --output-dir /tmp/q1000k-pon-overlay
```

Choose a new output directory. The extractor invokes `unsquashfs -cat`,
runs no OEM executable, verifies both blobs before publication and emits
an OpenWrt overlay under `lib/firmware/airoha/q1000k/`. It does not copy the
overlay into the checkout or a device automatically. The validated baseline
is QKX001-06.00.44.00:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `A60993.elf.pm` | 15232 | `5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1` |
| `A60993.elf.dm` | 56 | `21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4` |

## Validation and remaining acceptance gates

- Ten Python tests pass for the production C reader, shell backend, CLI and
  extraction logic. They cover invalid/truncated/duplicate records, unchanged
  inputs, firmware corruption, absent state, overrides, private staging and
  failed-stage cleanup.
- Node tests pass for the production LuCI views, including missing data,
  LOS true/false, failed polling, unknown schema and identity validation.
- Both AArch64/noarch userspace packages build. All kernel patches prepare
  successfully; the complete vendor kernel package fails as detailed above.
- Live factory and RPC checks pass. The I2C family-ID probe returns ENXIO;
  controller detection, calibration loading and optical activation do not pass.
- No browser QA, image flash, OLT registration, OMCI provisioning, optical
  traffic, reconnect/reboot reliability or accelerated PON traffic test has
  been claimed or completed.

Continue the [implementation plan](XGSPON.q1000k.md) with confirmed board
selection/power/TX-disable wiring and an EN7573 loader, then resource/QDMA
integration and OMCI. Keep `pon_pcs` and `gdm2` disabled until their owner and
initialization order are implemented. Keep optical packages optional until
the bench, registration, service and recovery gates pass.
