# Q1000K OEM clock/acquisition/override audit v2

Date: 2026-09-15. Scope: same-unit OEM `phy_10g.ko.dis`, imported AN7581
`en7581_pma.c` / `en7581_reg.h`, current Q1000K checked lifecycle and prior
captures. This is static evidence and a bench design, not hardware validation.
No GPON/EN7571 address is used. All addresses below are physical AN7581 words.

## Principal finding

The previous `oem-order` experiment did **not** implement OEM CDR acquisition.
It retained public L2R/L2D and TDC-off writes, public digital reset ordering,
and public FLL reset. Its capture confirms `cdr_control=0x01010101` before and
after. OEM L2R/L2D/TDC-on instead ends with the mask `0x01010101` equal to
`0x00000101`: lock-to-data remains selected but the direct LPF-reset override
is released. This is a concrete untested difference.

The OEM also has fewer TX-PCW override changes in TDC-off, a different reset
order, and meaningful settle delays absent from the public sequence. These
differences should be tested as both isolated probes and a combined sequence.

## Sources and extraction

- OEM disassembly:
  `/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/oem-pon-reference-20260915/phy_10g.ko.dis`.
- Public source:
  `/home/odin/local/q1000k/openwrt-q1000k/package/kernel/airoha-pon/src/xpon-en757x/xpon_phy_10g/src/en7581_pma.c`.
- Public field definitions: adjacent `inc/en7581_reg.h`.
- Read-only analysis helper `/tmp/q1000k-oem-extract.py` extracted immediate
  `IO_SPHYA_REG_BITS(w0,w1,w2,w3)` calls; `/tmp/q1000k-oem-calls.txt` and `.json`
  contain offsets and names. Branch-dependent values are left unknown.
- OEM delays decode `__const_udelay(n*4295)`, cross-checked against public
  matching calls. E.g. 0x68dbc=100us, 0xd1b78=200us, 0x147aeb8=5000us.

## Exact acquisition comparison

### CDR direct override: `0x1fa8b818`

Public `XPON_RX_L2R` lines1718–1733 writes bit0=0, bit8=1, waits100us, writes
bit24=1, bit16=0→1. Public `XPON_RX_L2D` lines2262–2280 writes bit24=1,
bit16=0→1, waits200us, then bit0=1,bit8=1. Public TDC-on ends with another
bit24=1/bit16=0/bit24=1/bit16=1 sequence. Result: `0x01010101` on these fields.

OEM `XPON_RX_L2R @0x26984`:

| Call offset | Field | Value |
| --- | --- | --- |
| 0x269a0 | b818[16] direct LPF reset value | 0 |
| 0x269b8 | b818[24] direct LPF reset force selector | 0 |
| 0x269d0 | b818[0] direct lock-to-data value | 0 |
| 0x269e8 | b818[8] direct lock-to-data force selector | 0 |
| 0x269f4 | delay | 100us |

OEM `XPON_RX_L2D @0x27b48`: b818[0]=1 at0x27b64, b818[8]=1 at0x27b7c,
**then** 200us at0x27b88. It does not force or toggle LPF reset.

### TDC-off

OEM `XPON_TDC_off @0x26508`:

1. 0x26524: b034[8]=1 (`SS_LCPLL_TDC_FLT_3`, NCPO load).
2. 0x2653c: b02c[0]=0 (`SS_LCPLL_TDC_FLT_1`, GPON select).
3. 0x26554: b010[0]=0 (`SS_LCPLL_TDC_PW_0`, TDC digital power).
4. 0x26560: wait1000us.

Public lines1568–1588 additionally sets b794[24]=1 (forced TXPLL PCW), sets
b864[8]=1 (PCW-change force selector), toggles b864[0]0→1 and omits the delay.

### TDC-on and shared PLL

OEM `XPON_TDC_on @0x28a94` is the first nine public register writes, with
100us after NCPO-load release,100us after autopower-NCPO release, then5000us:

`b040[9:8]=0; b028[1:0]=1; b864[8]=0; b794[24]=0; b024[16]=1;
b034[8]=0; wait100; b03c[16]=0; wait100; b02c[0]=1; b010[0]=1; wait5000`.

Offsets are0x28ab0,0x28ac8,0x28ae0,0x28af8,0x28b10,0x28b28,
0x28b34,0x28b4c,0x28b58,0x28b70,0x28b88,0x28b94.
No final CDR reset writes occur. The old `oem-order` already omitted the last
four public TDC-on writes and used5000us, but public L2D had already forced
LPF reset and never released its selector.

OEM reconnect `fiber_plug_reset @0x23410`, case2 at0x234b0:
`DIG_reset → L2D → TDC_on → TXPLL_on → rxrdy → phy_status → R2T(normal)`.
TXPLL_on @0x25a00 is the same thirteen known public clock-control writes and
6us/500us delays. This is shared PHY clock configuration, with optical TX
still inhibited. Repeating it alone was already tested; combine it with the
actual OEM CDR state for new information.

### Digital reset and RX FIFO settling

OEM `XPON_DIG_reset @0x275f8` first sets b000[24]=1 at0x27614 (manual LCPLL
power) and waits100us. Holds b460 bits11→0, waits10us, releases11→0.
Fields11:7 lack public semantics; do not replay them.

A bounded known-field adaptation can preserve OEM order for bits6→0, the
100us preamble and10us hold. It is explicitly a subset, not full OEM replay.
The old probe instead used public release5,1,2,3,0,6,4 plus500us after bit2.

OEM `XPON_RX_rxrdy @0x27570`: b114[24]=1; b10c[24]=0; wait10us;
b460[0]=0→1; wait100us. Public same writes but no delays.

### FLL reset force

Public `XPON_FLL_Reset` lines2720–2736 cycles b17c[0]0→1 and b17c[8]0→1,
500us between. Comment names bit0 `rg_fll_dig_rst_force_en` and bit8
`rg_fll_dig_rst_force`; EN7580 counterpart calls bit8 `rstb_force`.
OEM has no equivalent function or b17c write in acquisition; public
`PLUG_OUT` and the old probes add it. A bounded b17c[0]=0 probe tests release
of the named force-enable selector. Keep bit8 unchanged/released1 and save
the old field. This is a public-field hypothesis, **not an OEM preset**.

## Calibration and retained overrides

### Public/OEM calibration sequences mostly match exactly

Register-call sequence comparison gave:

| Routine | OEM address | Exact matching writes |
| --- | --- | ---: |
| XPON_RX_OSCal | 0x26a04 | 13 |
| XPON_RX_pical | 0x26b5c | 22 |
| XPON_RX_pdos | 0x26d8c | 38 |
| XPON_RX_feos | 0x2713c | 18 |
| XPON_RX_sdcal | 0x2730c | 21 |
| XPON_phy_status | 0x27524 | 2 |

FEOS wait is200us OEM,1000us public; other listed calibration waits match.
Both deliberately retain multiple forced power/ready bits. A blanket
"clear force bits" operation would contradict these sequences.

### PrCal finalization: exact documented release/reload probe

OEM `XPON_PrCal_WK @0x25db4` and public lines1308–1458 both search seven
coarse IDACs then eight fine bits,5ms per sample, using XGS target0xa49a in
`RO_RX_FREQDET[31:16]`. They then release temporary injection/IDAC/LPF-C
overrides and load the selected IDAC into the automatic FLL path:

| OEM offset | Register field | Value / effect |
| --- | --- | --- |
| 0x26148 | a0f4[24] INJ_FORCE_OFF | 0 |
| 0x26160 | b820[24] LPF-R force selector | 1 |
| 0x26178 | b820[16] LPF-R value | 1 |
| 0x26190 | b820[8] LPF-C force selector | 0 |
| 0x261a8 | b820[0] LPF-C value | 0 |
| 0x261c0 | b794[16] IDAC force selector | 0 |
| 0x261d8 | b19c[0] FLL load enable | 1 |
| 0x261f4 | b174[10:0] FLL IPATH IDAC | selected IDAC |
| 0x2620c/0x26224 | b824[16] CDR PR power | 0→1 |
| 0x2623c | b824[24] CDR PR power force selector | 1 |

For an isolated finalization probe, capture b794[10:0] before writes and
use it as the already-selected IDAC, with a nonzero/range guard. For a full
recalibration probe, run the bounded source algorithm with checked access,
record all15 measured values and result, then this tail and OEM reacquisition.
Avoid calling raw vendor functions, whose writes are not individually checked.

Public RX_on only calls PrCal when `GET_PDIDR()==1`; OEM @0x26950 calls it
unconditionally and retries once if return!=1. Current captured b794=0x523
and RX frequency near0xa49a suggest PrCal ran, but they do not prove every
tail field. `GET_PDIDR` reads NP-SCU offset0x5c low16; do not add a guessed
address. Passive snapshots of a0f4,b820,b174,b19c,b17c close the gap.

### Eye scan leaves post-calibration state

FIRST_PLUG_IN calls `EO_Scan(pon_Spd,0,7)` **after** `phy_status` and before
`TDC_on/rxrdy`; it is not passive instrumentation.

OEM `EO_Scan @0x27b98` scans gain1 and peaking0..7, writing b768[19:16].
Public scans gain1..3 and peaking0..7 into b768[19:17], retaining bit16.
The old gain-only probes do not cover this encoding and chosen-peaking gap.

Both `XPON_eye_EO` tails force PI-cal ready low: b31c[0]=0,b30c[0]=0.
They also force eyecount-ready low: b324[16]=0,b318[0]=0. Eye reset remains
asserted b330[8]=1; eye-top enable forced off b33c[16]=0,b330[16]=0.

Both `XPON_readout_EO` invalid-HEO path leaves an additional internal CDR
reset override: OEM0x28a3c b300[24]=0,0x28a54 b294[24]=0,wait500us,
0x28a78 b294[24]=1,wait500us. Direct b818 controls and this internal mode
are separate layers. Clearing b818[24] can expose a still-forced b294 value.

Thus useful bounded probes include:

- Restore post-PI-cal ready: b31c[0]=0,b30c[0]=1, precisely public/OEM pical
  tail (OEM0x26c38 /0x26d7c), after the startup eye scan. This is a sequence
  hypothesis, not a mismatch (both sources leave the low state after EO).
- Return internal LPF reset to normal mode: b300[24]=1; optionally independent
  full internal CDR automatic mode also b300[16]=1. These are explicit
  `NORMAL_MODE` macros at header1570–1573. Combine with releasing direct
  b818[24] (and b818[8] if testing fully automatic lock selection).
- Test normal sequencer ready/BLWC/OS-ready/SDCAL ownership: b10c bits
  24,16,8,0=1 via explicit `NORMAL_MODE` macros header1603–1610. Baseline and
  OEM use0=forced mode. Use a named independent variant, not claimed OEM fix.

Normal mode for DISB registers means **set1**, whereas direct force selectors
usually use0. Do not invert this distinction based on generic naming.

## Observed current values vs source-implied OEM values

From prior suite baseline and old `oem-order` reports:

| Field | Current observation | OEM source implication |
| --- | --- | --- |
| b818 CDR direct controls | 0x01010101 in both cases | 0x00000101 after L2R/L2D/TDC-on |
| b460 digital resets | 0x0000007f | low12 all1; upper5 meanings unknown |
| b10c sequencer mode | 0 | 0 (forced mode) |
| b114 sequencer force | 0x01010100 | ready/OS-ready/BLWC high, SDCAL low |
| b110 calibration force | 0 | 0 after calibrations |
| b840 OSCAL direct controls | 0x01000100 | selectors1, enable/reset values0 after SDCAL |
| b794 IDAC control | 0x00000523 baseline | bit16=0, bit24=0; learned IDAC variable |
| b864 PCW-change control | 0x00000001 | force selector0 after TDC-on |
| b768 peaking | not in old diagnostics | full nibble0..7, force selector1 |
| b820/a0f4/b174/b19c/b17c | not in old diagnostics | capture required |
| b294/b300/b30c/b31c/b318/b324 | not in old diagnostics | capture required |

Optical power/LOS success does not validate these high-speed fields.

## Concrete firmware probe matrix

All cases: immutable optical TX inhibit; exclusive sleepable callback owner;
PMA init complete; XGS mode; fresh controller check; save every written field
before first write; checked masked writes/readback; restore on teardown,
including partial-failure paths; one bounded attempt. Preserve all raw logs.

1. `oem-cdr`: OEM L2R→OEM L2D only, exact order/delays. Isolates direct CDR
   acquisition from TDC/reset differences.
2. `prcal-finalize`: capture current IDAC, replay exact finalization tail,
   OEM L2R/L2D, existing TDC-on OEM subset and rxrdy. Record complete before/
   after release fields and selected IDAC. A still simpler release-only case
   can omit L2R/TDC to isolate fields but may not retrigger hardware.
3. `fll-auto`: clear only b17c[0] force-enable, then OEM L2R/L2D. Keep a case
   without this variant so success cannot be wrongly attributed to OEM writes.
4. `post-eye-ready`: b31c[0]=0,b30c[0]=1 after scan; RX FIFO pulse optional
   explicit sequence. Isolates calibration-ready tail.
5. `cdr-internal-auto`: b300[24]=1, direct b818[24]=0; separate full-auto
   lock variant can also use b300[16]=1,b818[8]=0. Explicit public modes.
6. `rx-sequence-auto`: set named b10c normal-mode fields after current
   calibration; no wholesale force-register overwrite.
7. `oem-clock`: OEM TDC-off +OEM L2R +known-bit OEM digital reset subset
   +OEM L2D +OEM TDC-on +TXPLL_on +OEM rxrdy +phy_status. No added FLL reset.
8. `combined`: OEM analog/electrical settings from companion audit, PrCal
   finalization, post-eye PI-ready, and OEM clock. Keep speculative normal-
   mode/FLL changes as a separately identified combined-auto variant so an
   untested automatic sequencer cannot obscure success of the OEM subset.

For full rerun of calibration, use the exact bounded public register lists
under ownership; no raw vendor write can escape save/restore. If invoking
all five calibration lists plus clock fields, raise saved-field capacity
based on distinct fields; current64 slots may be insufficient.

## Cold boot and reconnect interpretation

The Q1000K RX bench's poll/IRQ branches sample/ack state and **do not call**
ordinary vendor LOS/insertion handlers. Previous live dark/reconnect was
therefore a passive transition, not a test of vendor PLUG_OUT/PLUG_IN.
Report this correction wherever the older plan claimed insertion handling.

The new single-image suite should compare initial connected calibration,
the probes above, and live confirmed dark→light in one lifetime. Explicitly
trigger the selected bounded reacquisition after observed reconnection to
test that sequence. Keep baseline passive transition as a control. If no
case works, a separately cold booted baseline distinguishes inherited state
without confusing module reload with a full hardware reset.

Unresolved: same-unit OEM runtime snapshots, unknown reset bits11:7,
EN7573 electrical output route/quality, actual recovered-clock lock signal,
and analog eye quality. Source audit supports bounded experiments, not a
guarantee any will produce downstream frames.

## Implemented PHY probe map (this audit's follow-up)

Existing enums0–10 are unchanged. The following are now implemented in
`q1000k_phy_probe.c` and the new `q1000k_phy_probe_oem_steps.h`:

| Enum | Public name | Exact scope |
| ---: | --- | --- |
| 11 | cdr-auto-release | Exact OEM L2R then L2D; b818 ends masked0x101 |
| 12 | cdr-internal-auto | b300[24,16]=1; b818[24,8]=0; explicit public normal mode |
| 13 | prcal-finalize | Read retained IDAC, guard nonzero, exact finalization/reload tail |
| 14 | fll-auto | Clear b17c[0] only; retain b17c[8] and all other fields |
| 15 | rx-sequence-auto | b10c[0,8,16,24]=1 normal modes, preserving force values |
| 16 | post-eye-ready | b31c[0]=0,b30c[0]=1 |
| 17 | oem-clock-cycle | OEM TDC-off, L2R, known-reset subset, L2D, TDC-on, PLL, ready |
| 18 | oem-rx-acquire | Peaking translation +gain1 +PrCal tail +PI-ready +OEM clock |
| 19 | oem-peaking | Translate captured public [19:17] selected index to OEM [19:16], force24=1 |
| 20 | checker-dark | Alias of exact existing receiver checker arming; caller owns dark prerequisites |
| 21 | combined-auto | Case18 then FLL-auto/internal-auto/sequencer-auto |
| 22 | prcal-rerun | OEM TDC-off,7coarse+8fine5ms measurements,no retry,finalization+OEM clock |

The evidence-near integrated18 intentionally excludes the automatic-mode
hypotheses. Case21 combines them separately. The PrCal rerun logs each raw
meter/IDAC and selected IDAC; a zero or0xffff oscillator count stops the probe
with all already-saved fields available to normal cleanup. None of the raw
readings is reported as optical clock lock or BER. Save capacity is128 distinct
fields; every new operation uses the same checked owner/access/restore path.
The logged `final_meter` follows the final code write immediately, matching
the public/OEM ordering; it may be the last latched observation and is **not**
a freshly settled measurement of the final selected frequency. Only the15
numbered search observations each have their own preceding5ms settle delay.

Focused host verification passed: production C for every mode, per-operation
failure injection and exact register restoration, corruption detection,
OEM CDR ordering, all65 OEM clock events with reset order and settle times,
normal-mode masks, public→OEM peaking translation, a monotonic PrCal meter
model selecting0x523 with exactly15 observations plus final read, and invalid
calibration input/count rejection. The script was
`python3 -m unittest discover -s tests/q1000k -p test_pon_phy_probe.py -v`.
