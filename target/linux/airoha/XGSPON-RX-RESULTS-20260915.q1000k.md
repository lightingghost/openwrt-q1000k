# Q1000K receiver bench results — 2026-09-15

Branch: `q1000k-xgspon`. The receiver still has light without downstream frame
synchronization. The new clock, calibration-exit, controller-transport and
electrical-output recipes ran with checked readbacks; none recovered framing.
The fresh dark-checker control did produce a light-dependent checker response.
That narrows the evidence gap but does not establish a recovered data clock or
valid XGS-PON data.

## Collection status and evidence

The main collection has **35 valid cases out of the planned 36**:

| Part | Cases | Raw RX samples | Status |
| --- | ---: | ---: | --- |
| Connected experiments, including initial/repeated baselines | 34 | 2,940 | Valid captures; no downstream framing |
| Fresh dark-checker physical control | 1 | 180 | Confirmed and observed light/dark/reconnected phases |
| Passive `live-reconnect` physical control | 1 | 180 | Incomplete: missing physical confirmations and required transitions |
| Total captured | 36 | 3,300 | Collection stopped; archive preserved |

Across **all 3,300 raw samples**, synchronized state was false and the absolute
frame, FEC and seven PCS counter values were zero. This includes the 180 samples
from the incomplete passive control; those samples do **not** turn that control
into a valid reconnection experiment. There are 3,120 samples in valid cases.
The preliminary power check is separate and is not included in these totals.

The passive control's per-case capture finished and cleanup passed, but the
collector correctly marked the physical experiment failed. Its only confirmed
observed phase was illuminated. Overall `collection.json` remains `stopped`,
with an explanation, rather than claiming all 36 cases passed.

**Final passive-control status: incomplete; no repeat performed.** A repeat
requires coordinated physical disconnect/reconnect confirmations. The completed
evidence is preserved without crediting an unperformed control. The temporary
RAM filename alias was removed after verified idle state; the original module
and helper hashes remained unchanged.

Primary local artifacts:

- [Main collection and case status](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/collection.json)
- [Preserved collection archive](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01.tar.gz)
- [Preliminary power check](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-power-check-02/connected-baseline/receiver-report.json)
- [Dark-checker raw observations](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/checker-dark/attempt.log)
- [Dark-checker phase report](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/checker-dark/control-report.json)
- [Incomplete passive control checkpoint](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/live-reconnect/checkpoint.json)

Each case directory retains raw controller/RX/diagnostic observations, serial
capture, requested selection, reports, input staging and cleanup evidence.
A bench command's `failed` status can mean no stable downstream framing even
when its capture is valid. The collection's `observed` result and physical
phase validation distinguish that outcome from an incomplete experiment.

## What changed in the evidence

### Optical power and analog telemetry

The initial power observation was approximately **−18.51 dBm**. The separate
30-sample power-check-02 capture ranged from **−18.57 to −18.48 dBm**, with
controller and PHY LOS both clear. Its reported APD voltage was 31.375 V,
supply 3.3416–3.3457 V, and temperature 33.87–38.67 °C; OCP fault indication
remained clear. This reproduces the light/no-frame condition with additional
controller telemetry, without independently calibrating optical power.

The nine electrical-output cases plus standalone OEM MD32 transport case
produced these combined ranges:

| Controller measurement | Observed range |
| --- | --- |
| Optical power | −18.63 to −18.21 dBm |
| APD voltage | 31.500–31.625 V |
| Supply | 3.3339–3.3396 V |
| Temperature | 42.38–46.34 °C |
| RSSI current | 125–136 µA |
| OCP fault indication | Clear in every snapshot |
| Controller LOS | Clear in every RX sample |

There is no observed supply collapse, APD-off state or OCP fault in these
captures. These are controller-reported values, not direct measurements of
the differential receiver signal. Temperature rose over the sequence, so small
RSSI, bias or supply changes between cases are confounded by warm-up.

### OEM clock acquisition and calibration exits

The new experiments tested the actual documented OEM direct CDR state, unlike
the previous `oem-order` recipe. Readback confirmed:

| Case | CDR control before | CDR control after | Framing result |
| --- | --- | --- | --- |
| `cdr-auto-release` | `0x01010101` | `0x00000101` | No sync or frames |
| `oem-clock-cycle` | `0x01010101` | `0x00000101` | No sync or frames |
| `oem-rx-acquire` | `0x01010101` | `0x00000101` | No sync or frames |
| `combined-auto` | `0x01010101` | `0x00000001` | No sync or frames |

Thus the direct LPF reset override really was released for these measurements.
Successful readback without frames is a negative result for the implemented
recipes, not proof that clock acquisition in general is correct. The OEM reset
sequence was restricted to documented bits 6:0; unidentified bits 11:7 were
not replayed.

The baseline already had the source-defined PrCal finalization state:
injection off, LPF override word `0x01010000`, direct IDAC forcing released,
matching IDAC in the FLL and its load field set. The `prcal-finalize` experiment
did not recover framing. The full bounded `prcal-rerun` changed the learned
IDAC from **`0x51f` to `0x522`**, with matching FLL readback, 107 checked writes
and confirmed restoration. It still produced no synchronization or frame/PCS
activity. This distinguishes an executed calibration search from a no-op tail.

RX meter upper counts near `0xa49a`/`0xa49b`, changing NCPO words, and passive
FIFO status are retained as observations. They are not independent clock-lock
or calibrated baud-rate measurements. No eye opening was measured.

Evidence: [PrCal report](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/prcal-rerun/probe-report.json),
[OEM acquisition report](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/oem-rx-acquire/receiver-report.json).
Register provenance and the bounded reset interpretation are in the
[OEM clock audit](XGSPON-OEM-CLOCK-AUDIT.q1000k.md).

### Electrical output and MD32 transport

Each of the three EN7572-family output profiles ran in three combinations:
alone; with `oem-rx-acquire`; and with `oem-rx-acquire` plus OEM A0 MD32
transport. All **nine** cases verified the exact requested profile and
preserved unrelated register bits in every one of their 91 controller
snapshots. Each collected 90 RX samples. None recorded synchronization,
frames, FEC or any nonzero PCS counter.

| Profile | Actual A2 `0x110` | Actual A2 `0x114` | Three combinations |
| --- | --- | --- | --- |
| `400-flat` | `0x01002c5f` | `0x14001406` | All applied; no frames |
| `600-flat` | `0x01002c5f` | `0x1e001e06` | All applied; no frames |
| `600-boost` | `0x01002c1f` | `0x3608320e` | All applied; no frames |

The profile labels come from a public family-source table: nominal
400 mV/0 dB, 600 mV/0 dB and 600 mV/2 dB. They are not measured output swings
or established Q1000K defaults. See the
[controller/electrical audit](XGSPON-OEM-CONTROLLER-AUDIT.q1000k.md).

The standalone `oem-md32` case also loaded, verified and started successfully
through A0 control/address registers, with unchanged output words
`0x01002c1f`/`0x371f1b06`, and no frames. The requested `bench_md32_a0` value
matched every initialized status snapshot in the standalone and combined cases.
Across these ten cases, all 910 initialized snapshots reported firmware and
calibration supplied/verified, MD32 enabled, TX disabled and inhibited, and
error zero. The ten cases contain 900 RX samples.

Controller `0x110[8]`, which OEM `xponconfig` sets, remained clear in every
electrical case. Its semantics remain unidentified; these output masks do not
test that OEM difference.

### Fresh checker armed in darkness

This is the strongest new positive diagnostic observation:

1. The checker was restarted once after confirmed darkness. For **39 armed
   dark samples spanning 44.030 seconds**, checker event and error words stayed
   zero.
2. After light returned, the event word became **`0x00000101`** and the error
   word **`0x0000fff1`**, at **+3.468 seconds** relative to the sampled return of
   light. At **+4.631 seconds**, the event word became **`0x00010101`**.
3. The intervention count remained **1** and checked writes remained **2**;
   there was no second software checker restart on reconnection. Frames and
   all PCS counters remained zero.

The experiment establishes a checker response correlated with return of light
after a fresh dark arm, which is stronger than observing a pre-existing latched
completion bit. It does not establish correct XGS-PON data, CDR lock, or optical
BER: normal downstream traffic is not the configured PRBS sequence, and LOS
gating or associated clock enabling can also explain a light-dependent checker
response. The timing is relative to sampled state, not an instrumented physical
insertion timestamp. The separate passive reconnect control remains incomplete.

The raw NCPO tracking word also changed materially across this physical
control:

| Phase | Raw NCPO range |
| --- | --- |
| Before disconnection | 133,591,585–133,592,549 |
| Checker armed in darkness | 127,039,814–127,281,926 |
| After reconnection | 127,037,318–129,069,887 |

During the **84.602-second** observed post-reconnection interval, the word did
not return to its original illuminated range. NCPO is uncalibrated and this is
not proof of lock loss or a measured frequency offset. The bench omits vendor
LOS power-save/recovery handlers, so clock state and acquisition after insertion
remain open questions. This motivates the separate passive-control comparison;
the checker response alone does not resolve them. The stable-power ranges above
refer only to their named captures/groups, not every illuminated sample during
reconnection transients.

## DSD calibration: precisely what was tested

The private input record is the same **513-byte** XGS calibration extract
previously compared byte-for-byte with the original NAND backup at `0x412000`
(`DSD + 0x12000`). Its SHA-256 is:

`f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c`

The completed baseline's
[staging log](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/connected-baseline/stage.log)
records successful on-router checksums after RAM extraction for this record
and both pinned MD32 firmware files. The driver then checks firmware hashes,
loads the first **512** calibration bytes into **DM `0x600–0x7ff`**, and reads
back every word of the full **16 KiB PM and 4 KiB DM**, including calibration
and zero padding, before starting MD32. Initialization cannot report success
after a mismatch. All 31 initialized baseline status snapshots confirmed
calibration supplied, firmware verified, MCU enabled, TX disabled/inhibited
and error zero.

The OEM loader also consumes 512 bytes. The 513th record byte is retained at
staging but not transferred to MD32. This run used the verified RAM archive;
it did not reread live DSD. Readback precedes MCU start and is not a continuous
post-start checksum. These results verify record identity, transport and
startup; they do not validate the meaning, physical accuracy or firmware use
of each analog coefficient. Other DSD regions have not been established as
required inputs for this XGS receiver path. SoC PrCal is a separate receiver
oscillator calibration, not a validation of the DSD coefficients.

## Hypothesis summary and remaining limits

| Hypothesis / question | Executed test | Observed outcome | What remains unresolved |
| --- | --- | --- | --- |
| Light reading is stale or unrelated to insertion | Confirmed dark-checker light/dark/reconnected phases | LOS changes and fresh checker response after light returns | Absolute optical level, wavelength and modulation quality |
| Direct CDR override or known OEM clock ordering prevents acquisition | Isolated CDR release and documented OEM clock/acquisition recipes | Actual override release verified; no frames | Independent recovered-clock evidence, unidentified OEM reset fields and interactions |
| Calibration was left unfinished or needs a fresh search | PrCal tail plus bounded complete rerun | Final state already present; IDAC changed `0x51f` to `0x522`; no frames | Physical quality of the selected operating point |
| Controller MD32 control-bank selection is sufficient to fix reception | OEM A0 loader alone and combined with acquisition/output profiles | Verified load/start in all requested modes; no frames | MD32's analog behavior and additional OEM initialization semantics |
| One of three documented electrical output settings restores reception | Nine isolated/combined cases | Exact profile readback and preservation; no frames | Actual differential route, polarity, termination, amplitude and eye quality |
| Unit calibration is absent or corrupted in transport | Pinned unit-record checksum and full MD32 readback | Transfer verified on this run | Coefficient semantics, analog accuracy and firmware application |
| Tested PCS packing/descrambler/FEC choices alone restore framing | Bit-order, descrambler, FEC-OC and FEC-off cases | No sync, codeword, frame or other PCS activity | Earlier input failure or an undocumented combined configuration |
| Passive reinsertion response is adequate without intervention | Separate `live-reconnect` window | 180 samples saved; physical control incomplete | Confirmed passive light/dark/reconnected comparison |

No row excludes its entire hypothesis family. It excludes only the tested
recipe as a sufficient fix under the recorded conditions.

## Concrete next evidence priorities

1. Complete the passive control when an operator is ready. Require explicit
   physical confirmations and observed lit/dark/reconnected phases in one
   continuous capture, then compare with the fresh-dark checker result.
2. Obtain a known-good same-board OEM receiver snapshot or documented register
   definitions for `0x110[8]`, reset bits 11:7, input routing and post-calibration
   state. Explain each difference before proposing a new write.
3. Establish the EN7573-to-AN7581 differential receive route, polarity and
   termination using board documentation or identified measurement points.
   Measure electrical data/clock quality there if suitable equipment is
   available. Existing power, meter and checker words do not settle this.
4. Compare the working gateway's optical mode and power on the same line.
   Use wavelength-selective/calibrated measurement if the line identity or
   absolute level remains uncertain.
5. Map the existing unit calibration coefficients to documented OEM functions
   and compare their applied analog state with the same unit under OEM firmware.
   Preserve the original record; the current result gives no basis for replacing
   calibration values or borrowing another unit's record.

## Runtime provenance and corrected delivery

Hardware evidence above belongs to image revision
**`3e73559f54c3fcbd8e51d09fa40bc8779d8623ec`**. Parameterized module loading
initially used the kernel's underscore name rather than the installed hyphenated
filename. The collection ran with a temporary RAM symlink
`q1000k_pon_control.ko -> q1000k-pon-control.ko`. The original helper and all
runtime module bytes remained unchanged, as recorded in
[runtime-adjustment.json](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-3e73559f54-acquisition-collection-01/runtime-adjustment.json).

The corrected kit is revision **`4c84635a0956bd1a3fce467fe34a219fd5b12f2a`**.
Its helper resolves the installed absolute module filename and no longer needs
the alias. Focused helper validation passed 27 tests. The replacement build
passed 206 PON host tests, 13 status tests, UI checks, exact image inspection
and the portable collector dry-run. All 13 other runtime manifest entries,
including all nine kernel modules, are byte-identical to the original artifact;
only the bench helper changed. The earlier source-equivalent PHY UML evidence is
explicitly inherited, not a newly executed UML run.

**The corrected image has not been booted or hardware tested.** Do not relabel
the original-image observations as replacement-image results.

- [Corrected kit](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/q1000k-rx-acquisition-4c84635a09.tar.gz)
- [Corrected artifact README](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-4c84635a09-loaderfix/README.md)
- [Build and inspection verification](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-4c84635a09-loaderfix/verification.json)
- [Original experiment design](XGSPON-RX-ACQUISITION.q1000k.md)
- [PR 24577 applicability](XGSPON-PR24577.q1000k.md)

These are receive-only bench results. Optical TX remained inhibited,
registration remained disabled, and the RAM image disabled NAND. No optical
service acceptance or working XGS-PON support is claimed.
