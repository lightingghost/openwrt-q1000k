# Q1000K: deep receiver consolidated bench

**2026-09-16 extension:** the [58-case repeated-acquisition bench](XGSPON-REPEAT-RX.q1000k.md)
retains every case below and adds one bounded repeated OEM reset/clock case.

Prepared 2026-09-15 on `q1000k-xgspon`. This extends the previous acquisition
bench with **21 new cases in one image**, for **57 cases total** including two
physical controls. These are prepared experiments, not hardware results.
Shutdown diagnosis remains included as a separate issue; no intermediate
shutdown-only image is required.

## Findings that change the next test priority

1. **A specific OEM controller write is missing.** Both the unit's NAND OEM
   `xponconfig` and the newer standalone firmware run
   `echo xw 0x110 8 8 1 > /proc/lddla/debug` after loading `en7572`, `phy_10g`
   and `xpon_10g`. The most recent baseline read `0x01002c1f` at `0x110`, so
   bit 8 was clear. The field's meaning is undocumented. The new
   `oem-post-init` case executes exactly that masked write after native PHY
   initialization and ten no-sync observations. It retains the original bit,
   checks independent controller TX-disable before/after the operation and
   verifies the entire word. `oem-post-cal` combines it with analog recovery.
   Neither case enables optical TX or sends upstream registration traffic.

2. **Post-calibration gain changes do not test calibration at the right gain.**
   Earlier gain/peaking trials modified the result of public initialization.
   The new analog cases apply OEM gain 1 before rerunning OSCal, PI calibration,
   PDOS, FEOS and signal-detect calibration. FEOS uses the OEM's 200 us wait;
   the imported public routine uses 1 ms. Each of the eight OEM peaking codes
   0–7 is an independent case with fresh calibration and a fresh eye observation.
   These are the finite codes traversed by OEM `EO_Scan`, not an arbitrary
   analog register sweep. The imported scan tests gains 1–3 and writes peaking
   to bits 19:17; OEM fixes gain 1 and writes bits 19:16.

3. **Previous “OEM reset” cases covered only seven bits.** The NAND and newer
   OEM PHY both hold and release `SW_RST_SET[11:0]` in descending bit order,
   with the same 100 us clock-power wait and 10 us reset hold. The new
   `oem-full-reset` case tests this exact twelve-bit recipe separately from
   `oem-clock-cycle`'s seven-bit recipe. The functions of bits 11:7 remain
   unknown. This experiment has direct board-specific OEM evidence; it is
   not a full SCU reset or a search over reset masks.

4. **Shared clock initialization already runs in receive-only mode.**
   `pon_LinkControl()` calls `JCPLL_on`, `TXPLL_on`, `XPON_DIG_fm_on` and
   `XPON_TX_on` before RX setup. Optical TX is separately inhibited in the
   controller. Thus the simple theory that receive-only mode skips all
   TX-associated shared PLL setup is contradicted by the code. Incorrect
   tracking, reset order, calibration and later override ownership remain
   open. The existing clock cases remain selectable in the same image.

## NAND, controller firmware and DSD evidence

The original 512 MiB backup was opened read-only. SHA-256:
`41f08f7e71c5c08835fd1f923a925e10bf56add1239b1e35ad35df6da8e76c50`.

| Item | NAND result | Consequence |
|---|---|---|
| Primary FIT | Offset `0x602100`, 40,782,623 bytes; kernel SHA-1 node verified | Extracted actual installed OEM reference, not just an update image |
| OEM root filesystem | SquashFS starts at `0x9666d4`; build timestamp 2024-02-25 | Older than the standalone November 2024 update |
| `phy_10g.ko` | SHA-256 `f06f41a1bfcf25b3e9e072045113e86569c91704a863c8da5fbc2ba679849c28` | Used to cross-check calibration, eye and reset tables |
| `en7572.ko` | SHA-256 `db0065680707791fdd2275e98f7c8236626a6223795f3f647806711836832d67` | Retained with disassembly for controller comparison |
| `A60993.elf.pm` / `.dm` | Both hashes and lengths exactly match the current bench inputs | A different OEM MCU binary is not the identified difference |
| Unit DSD XGS record | Offset `0x412000`, 513 bytes; SHA-256 `f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c` | Exactly matches the bench's unit-specific input |

The loader already verifies the complete 16 KiB PM and 4 KiB DM contents
before starting the MCU. The first 512 calibration bytes occupy DM
`0x600..0x7ff`, as in the OEM loader. This tests record selection and transfer;
it does **not** establish that the coefficients are physically correct or
that the analog/electrical frontend is healthy. No calibration or NAND writes
are part of this bench.

The new `q1000k_phy_probe_deep_steps.h` lists OEM disassembly call-site offsets
for every write/delay. Named calibration/eye/reset call sequences were compared
between the NAND and newer update; their normalized calls match. This is a
comparison of the selected routines, not a claim that the two entire binaries
are identical. Eye setup explicitly selects the XGS branch, and the eye
measurement selects its 5.5 ms branch. The conditional hidden LPF recovery in
`XPON_readout_EO` is deliberately excluded from measurement.

Reproduce extraction locally with:

```sh
python3 scripts/q1000k/oem-nand-audit.py \
  --nand ../q1000k-nand-backup.bin \
  --output ../build-artifacts/q1000k-xgspon/new-private-oem-audit
```

The private extraction is under
`build-artifacts/q1000k-xgspon/nand-rx-audit-v3/`; the reproducible extraction
is in its `reproduced/` directory. OEM binaries, NAND and calibration bytes are
excluded from the distributable bench kit.

## Hypothesis-to-collection mapping

All connected cases collect optical power, both LOS sources, controller health,
RX/PLL/TDC frequency words, NCPO, configuration/readiness fields, seven PCS
counters, frames and FEC. Diagnostics v4 adds eleven passive eye/FLL words.
Passive eye words may contain earlier latch results; only a validated serial
`RX eye fresh=1` record is reported as a fresh observation.

| Priority / hypothesis | Collector case(s) | What changes / what is measured | Discriminating result |
|---|---|---|---|
| High: missing OEM optical-controller initialization | **`oem-post-init`** | Only OEM `0x110[8]=1`, after native module initialization; independent TX-disable guard and saved-bit restoration | First sync, PCS activity or frames identifies a concrete missing prerequisite. No change rules out only this isolated write. |
| High: the missing controller step interacts with receiver setup | **`oem-post-cal`** | OEM controller bit plus gain-before-calibration, full OEM reset and fresh eye | Improvement only in this case identifies an interaction; its components have separate controls. |
| High: useful electrical data is absent or too poor at the SoC | **`eye-current`** | One bounded fresh eye measurement at the current frontend settings, with raw endpoints, DACs and completion flags | Compare with recalibrated/fixed-peaking cases and PCS activity. Failure to complete is retained as inconclusive, never zero-quality proof. |
| High: analog calibration was done under unsuitable gain/equalization | **`oem-analog`** | OEM preset and gain 1 before OSCal → PI → PDOS → FEOS → SDCal; known seven-bit reset, L2D → eye → TDC → ready | Compare with `oem-rx-acquire`, which changes settings after public calibration. |
| High: omitted OEM reset bits prevent clock/PCS operation | **`oem-full-reset`** | Exact twelve-bit reset in an otherwise OEM acquisition cycle | Compare with `oem-clock-cycle`, which uses only seven bits. Raw monitor changes alone are not CDR-lock proof. |
| High: calibration and reset order interact | **`oem-cal-reset`** | Same as `oem-analog`, using all twelve OEM reset bits | Isolates full reset on top of the new calibration recipe. |
| High: forced controls prevent normal CDR/sequence ownership | **`oem-cal-auto`** | `oem-cal-reset`, then release FLL/CDR/sequence ownership using the existing defined normal modes | Compare post-intervention frames/PCS/clock words. Its eye measurement precedes the final automatic-mode release. |
| High: OEM equalizer selection was lost or selected from an unhelpful public scan | **`oem-eye-0` … `oem-eye-7`** | Gain 1 and one fixed OEM peaking code before full calibration/reset; one fresh eye per case | Compare all eight under identical initialization. No automatic selection or retry obscures the individual observations. |
| High: controller electrical output needs a matching SoC initialization | **`oem-cal-400-flat`**, **`oem-cal-600-flat`**, **`oem-cal-600-boost`** | Known EN7572 output profiles combined with `oem-cal-reset` | Separates “output profile alone failed” from “profile plus correct initialization is required.” |
| High: controller post-init bit participates in that interaction | **`oem-post-cal-400-flat`**, **`oem-post-cal-600-flat`**, **`oem-post-cal-600-boost`** | Same three profiles combined with `oem-post-cal` | Tests the three-way combination without another firmware build. |
| Clock acquisition controls | Existing **`prcal-rerun`**, **`oem-clock-cycle`**, **`cdr-*`**, **`fll-auto`**, **`rx-sequence-auto`**, **`tdc-delay`**, **`pll-order`**, **`oem-order`** | Isolated oscillator/clock/override recipes retained unchanged | Negative old cases remain useful controls; PrCal is oscillator calibration, not input-data lock. |
| DSD/controller initialization and output controls | Existing **`oem-md32`**, **`rx-output-*`**, **`oem-acquire-*`**, **`oem-md32-acquire-*`** | Verified same-unit DSD/firmware in every case; separate OEM A0 transport and fixed output combinations | Tests actual transfer/addressing choices. Does not validate unknown coefficient semantics. |
| Physical light/clock response | **`checker-dark`**, **`live-reconnect`** | Operator-confirmed connected → dark → reconnected phases, 15 minimum samples each; checker armed once in darkness or fully passive control | Completes the previously missing passive control; distinguishes fresh checker response from a stale latch. |
| Framing/control and reproducibility | **`connected-baseline`**, **`baseline-repeat`**, remaining legacy gain/PCS cases | Unchanged controls and repeat baseline | Confirms that an apparent improvement is not merely a different optical state or stale sample. |

The first new case after baseline is `oem-post-init`, followed by its combined
case. Full collection has 57 cases, 5,190 seconds of sampling at the default
90 samples per acquisition case, plus initialization/cleanup and physical
prompt time. `--case NAME` selects one case from the same image and script.
`--samples 30` shortens acquisition observations; physical controls retain
their 180-sample windows.

## Measurement and execution limits

- Each probe gets one attempt per module lifetime, after ten no-sync/light
  polls. It does not run if the receiver is already synchronized.
- All changed fields are saved before their first write and checked on
  restoration. Calibration engine internal state is not reconstructible by
  restoring register fields; the next case performs normal fresh initialization.
- Fresh eye measurement performs finite setup, PI calibration, one 5.5 ms
  measurement and an explicit latch. Raw completion flags and endpoints are
  retained even for a failed measurement. There is no hidden LPF reset retry.
  Internal ticks are not calibrated volts, picoseconds or optical BER.
- The combined analog recipe reuses the initialized RX power/oscillator and
  explicitly finalizes its selected PrCal result. It replays the five OEM
  calibration stages and the reset → L2D → eye → TDC → ready ordering. It is
  not a claim to execute every OEM first-plug/module side effect.
- Exact register sequences have software failure-injection and restoration
  tests. Real hardware completion, register semantics and whether these changes
  recover frames remain untested until this image is booted.
- Existing shutdown diagnostics are included. An actual guard, shutdown or
  cleanup failure still stops collection and preserves evidence. Shutdown is
  not assumed to explain the earlier no-frame observations, and its failure is
  not silently converted into a successful test.

## OEM RAM boot and root access investigation

The NAND OEM kernel, DT, root filesystem and PON modules are extracted. The
kernel's embedded config has `CONFIG_BLK_DEV_INITRD=y`; its load/entry is
`0x80088000`. The FIT root filesystem node has type `filesystem`, **not**
`ramdisk`, so simply RAM-loading the stock FIT does not establish a RAM root.

All `CONFIG_RD_*` decompression options are disabled. A candidate external
initramfs must therefore be an **uncompressed `newc` CPIO**. A rebuilt FIT would
reference that CPIO as a ramdisk and pass `rdinit=/init` using the established
RAM-loading boot path. The custom `/init` would mount only proc/sysfs/tmpfs,
open pre-created `/dev/console` and `/dev/ttyS0` nodes, and execute an interactive
BusyBox shell as PID 1/UID 0. `CONFIG_DEVTMPFS` is disabled, so device nodes must
be included in the CPIO. This bypasses OEM `inittab`'s restricted `avec_console`
without cracking a password, modifying NAND, or depending on stock SSH login.

This is a concrete boot/root strategy supported by extracted configuration,
**not a tested OEM boot or tested root shell**. The built-in vendor SPI/NAND
drivers, watchdog behavior, DT exclusions and optical startup side effects
still need containment validation before executing that candidate. The next
bench does not run OEM binaries or ask for a second OEM boot image. Static OEM
comparison already supplies the specific new tests above.
