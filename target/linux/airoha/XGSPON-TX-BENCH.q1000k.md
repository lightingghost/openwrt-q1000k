# Q1000K internal TX and discovery bench

Implemented 2026-09-16 on `q1000k-xgspon`. Hardware results pending.
One activation RAM image, one pinned portable collector. No external optical
meter is available. No NAND writes or extra fiber outages are needed for the
default suite. Use the existing private unit MCU/DSD archive. Subscriber identity
is required only for the optional connected-fiber activation suite.

## Default: TX measurement experiments (`--suite measurement`)

One image retains all earlier suites. The default now runs 13 disconnected
cases, `isolated-19` through `isolated-31`. Every case uses a fresh PHY/controller
lifetime and this unit's verified MCU/DSD. No subscriber identity is required.

### Source findings

The static OEM reference is QKX001-06.00.44.00 `en7572.ko`, SHA-256
`86b8af889dab8f54b96ccaca0cb2b4973d5bafe28b9f5ecda0e8efed5a831a02`.
This is disassembly evidence, not a claim of running OEM firmware on the bench.

* OEM `ddmi_tx` at `0x1ff0` reads LE16 `0xf0`, stores calibration points at
  `0xb4/0xb6`, disables TX, waits, and stores a zero-power point at `0xb8/0xba`
  before enabling TX again. Its input is a supplied calibration power. It is
  **not** a refresh command. Without a measured reference power, do not execute
  it or substitute a guessed calibration value. The bench observes `0xf0` and
  preserves the existing calibration.
* OEM `mpd_current` at `0x4d10` saves `0x130[13:8]` and `0x208[6:4]`, sets
  `0x208[6:4]=4`, sets `0x130[13:8]=0x24`, waits **50 ms**, sets `0x120[26]=1`,
  waits **5 ms**, and reads `0x33c`. It restores monitor gain, clears the
  selection bit, and restores the loop mux. OEM translates `256-(raw>>7)`
  through a lookup table. This bench retains raw values and does not invent
  optical-power or current units. No conversion-ready flag was established.
* Sirherobrine's EN7572 `en7572_ddmi.c` reads MCU-maintained BE16 words at
  `0x60..0x6a`; its refresh functions update the host cache and do not trigger
  a conversion. `en7572_loop.c` reloads a calibrated eye and four TSSI bytes at
  `0xb4`, then restarts the loop. Neither is an EN7572 `mpd_current` equivalent.
  The older EN7570/EN7571 host ADC algorithms are different chip paths.
* OEM `bob_debug` at `0x5040` reads feature flags with **one-byte**
  `read1Byte(0xe8)`. The earlier four-byte diagnostic was incorrect; the new
  image reads one byte and preserves all bit patterns, including `0xff`.

Local audited public-source content hashes:
`en7572_ddmi.c`: `cfab2dc8426e8ed4c3dc54e5ec164f72a9be93f684491d9ab9cd2693a2a7ecab`;
`en7572_loop.c`: `d7d8b0e0291840cb76fac3d39febee87a23b70951a742b3cdeb9042fafdaec72`.
Public reference: [Sirherobrine EN7572 driver](https://github.com/Sirherobrine23/airoha_kernel/tree/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha),
revision `2e2cf91fe84467d77649efebd99a28284f2124b3`. Both local source blobs
match the cached Git tree.
The build uses the audited local source behavior; no live branch update is assumed.

### Experiment table

| Case | Transmit condition | Measurement / hypothesis |
|---|---|---|
| 19 | PRBS7, TX enabled | Passive monitor/DDMI reference; no monitor writes |
| 20 | PRBS7, TX disabled | OEM MPD probe with the independent laser gate off |
| 21 | PRBS7, TX enabled | Main active comparison against 19 and 20 |
| 22 | PRBS7, TX enabled, BEN forced off | Does the monitor response depend on burst gating? |
| 23 | PRBS7, TX enabled, PMA BEN polarity inverted | Is polarity preventing drive? |
| 24 | PRBS7, TX enabled, OEM eye 0 | Calibrated drive/monitor initialization |
| 25 | PRBS7, TX enabled, Sirherobrine eye 0 and TSSI reload | Calibration and DDMI conversion dependence |
| 26 | PRBS7, TX enabled, loop restart | Stalled loop versus missing data/gating |
| 27 | All-one pattern, TX enabled | Data-pattern dependence |
| 28 | All-zero pattern, TX enabled | Data-pattern dependence |
| 29 | Alternating pattern, TX enabled | Balanced-data comparison |
| 30 | PRBS7, TX enabled, existing clock state | Compare additional no-downstream clock preparation |
| 31 | Repeat case 21 | Reproducibility and drift |

Except case 30, these use the existing native no-downstream clock preparation.
Cases 19 and 21 differ only in whether the monitor probe actively selects its
measurement path. All cases sample three TX phases: before enable, about one
second into the five-second target window, and after disable/restoration.
An active probe separately captures before selection, after the OEM waits, and
after monitor restoration. Continuous passive DDMI sampling also remains enabled.
The script records actual timing; bus and scheduler latency can extend waits.

Each probe captures raw `0x33c`, LE16 `0xf0`, BE16 power/bias/modulation,
live bias/modulation codes, TX-control and BEN status. It records the saved
and selected monitor/loop settings, per-field valid masks, bus errors, and
restoration status. The getter only reads cached evidence. Missing evidence or
failed restoration stops collection; saturation is retained as unavailable.
The summary labels the raw monitor result `measured-response`, `measured-flat`,
or `measurement-unavailable`; none establishes conversion freshness or connector
emission. Compare the TX-disabled case before attributing a response to light.

The probe serializes controller access, verifies the lease and dark state,
and changes only the three audited masked fields. It preserves unrelated bits,
refuses an already active monitor selection, and attempts every restoration
on failure. It does not write TX enable, drive current or calibration itself;
the finite PHY experiment separately owns the authorized TX window. Dual LOS
checks surround each probe and continue during the test window. No MCU pause,
ADC algorithm from another chip, or factory calibration rewrite is introduced.

## Original disconnected-fiber pattern suite (`--suite isolated`)

Select `--suite isolated` to repeat the original 18 cases. Keep fiber disconnected
for the entire run. No physical actions or subscriber identity are needed. Each
case loads only the controller, provider modules and PHY. `ponraw` stays down;
MAC, registration and OMCI executors remain unloaded. An immutable PHY mode
rejects normal start. Every case initializes from the verified unit MCU/DSD pair.

| Script case / ID | Option compared | Reference / hypothesis | Diagnostic purpose |
|---|---|---|---|
| `isolated-1` / I01 | Controller TX enable, normal gate, no producer | OEM normal gate | Reference: does merely enabling the controller change drive/current or TX power? |
| `isolated-2` / I02 | PRBS7, existing cold-start clock state | Clock/power-save hypothesis | Contrast with I03 to isolate the additional no-downstream clock preparation. |
| `isolated-3` / I03 | PRBS7 plus native no-downstream clock preparation | Imported AN7581 vendor PHY | Main active reference without OLT grants. |
| `isolated-4` / I04 | PRBS23 | Native XGS equivalent of pattern diagnostics | Pattern dependence / sensor consistency. |
| `isolated-5` / I05 | PRBS31 | Native XGS equivalent of pattern diagnostics | Pattern dependence / sensor consistency. |
| `isolated-6` / I06 | All-zero pattern | PR 24577-style data/laser diagnostic | Contrast zero/one/alternating and internal modulation, bias, power. |
| `isolated-7` / I07 | All-one pattern | PR 24577-style data/laser diagnostic | Same; no arbitrary analog-current value. |
| `isolated-8` / I08 | Alternating pattern | PR 24577-style data/laser diagnostic | Balanced-data reference. |
| `isolated-9` / I09 | PRBS7, invert only PMA BEN polarity bit | Sirherobrine separate polarity controls; OEM profile 82 baseline | Is burst-enable polarity blocking the active pattern? |
| `isolated-10` / I10 | PRBS7, OEM `AdaptivePon(0)` eye fields | Q1000K OEM | Does explicitly applying this unit's calibrated XGS eye restore drive? |
| `isolated-11` / I11 | OEM alternate eye 1 | Q1000K OEM selectable eye | Validate availability; this unit's eye 1 fails field bounds and is skipped before writes. |
| `isolated-12` / I12 | Eye 0 plus TSSI calibration refresh | Sirherobrine EN7572 `load_eye` | Separate a DDMI calibration difference from a real drive change. |
| `isolated-13` / I13 | Eye 1 plus TSSI refresh | Sirherobrine alternate eye | Same eligibility guard as I11; expected unavailable on this DSD. |
| `isolated-14` / I14 | PRBS7, controller BEN forced off | OEM/Sir BEN field | Negative control: are current/power readings sensitive to gating? |
| `isolated-15` / I15 | PRBS7, loop restart only | OEM/Sir loop-enable toggle | Separate a stalled control loop from changed eye calibration. |
| `isolated-16` / I16 | PRBS7 restricted to normal timeslots | Native XGS generator gating | Negative control with no grants; compare ungated I03. |
| `isolated-17` / I17 | PRBS7 generator with controller TX disabled | Independent controller gate | Electrical pattern activity versus laser-enable dependence. |
| `isolated-18` / I18 | Repeat I03 in a fresh session | Temperature / drift / reproducibility | Check whether the active reference changes over the run. |

I03–I18 use native `FIRST_PLUG_IN -> PLUG_OUT -> 350 ms` preparation, from
AN7581's no-downstream PRBS path. This is an additional initialization pass, not
a claim that the serializer is locked. Readbacks retain PLL power/force/status
and the passive TX frequency result; a passive meter may be stale.

The kernel checks controller LOS and PHY LOS before emission and periodically
while it owns the PHY. Emission has a five-second target deadline; an in-flight
I2C transaction and the final disable can extend it. Actual TX enable/disable
boottime timestamps are recorded. There is no userspace-controlled indefinite
pattern mode. Signal detection ends the test. Cleanup disables the independent
controller TX gate, verifies it, and restores changed generator, polarity and
calibration controls. Any restoration failure stops the collection. A fresh
module/controller lifetime discards the clock-initialization state.

The script captures three TX-off samples before the test, samples controller
DDMI/control status about every 250 ms while the kernel operation runs, and
captures five after restoration. The kernel records 16 PHY register values at
before/active/after boundaries. `valid_phases` marks which snapshots completed;
unavailable slots are not observations. `collection.json` aligns entire I2C
samples with the actual test window; TX enable time is recorded separately
so the TX-disabled negative control is still measured. `isolated-summary.md` presents before,
active-test and after power/current ranges with valid sample counts. Raw errno,
control readbacks, result/restore errors and serial logs remain in the archive.

The known DSD's eye 0 passes the field checks; eye 1 fails current/monitor bounds.
The OEM reads the first 512 bytes directly into `flash_bob`; its eye 0 selects
bytes 256–511. No shift or invented fallback calibration is applied. Invalid
eyes report `unavailable-calibration` and the remaining cases continue.

### Reference mapping and limits

These are Q1000K implementations of the audited controls, not three complete
firmwares. OEM calibration maps to the extracted `AdaptivePon` instructions;
Sirherobrine adds a four-byte TSSI update at A2 `0xb4`. Both preserve this unit's
calibrated current limits. PR 24577 pattern diagnostics use the native XGS
`XG_CONTINUE_CTRL` at PHY `+0xa78`, not a copied GPON address. Sirherobrine's
GPON `0x058b/0x0577` response delays and GPON fine-delay defaults are not XGS
values and are not applied. The OEM XGS response time remains `0x1600`.

Disconnected fiber cannot establish valid optical burst timing, an on-fiber
serial-number response, OLT acceptance, ranging or O5. All those are marked not
run, not failed. The image retains the connected-fiber suite below to test
profile quiesces, hardware response state, received assignment and O5 later.
The build also exercises the existing serial encoding and PLOAM/profile fixtures;
those are software checks, not an OLT experiment. No registration ID is needed
for the disconnected suite.

## Optional connected-fiber collector cases

Select `--suite tx` and supply an identity to run:

| Case | Window | What it tests / collects |
|---|---:|---|
| `rx-startup` | 30 samples | TX-inhibited RX baseline, internal optical sensor and current references. |
| `activation-tx-baseline` | 600 s discovery deadline | Original profile behavior, threshold 40, normal authorized discovery TX; TX1–TX3, TX5–TX6. |
| `activation-tx-coalesced` | 600 s | Same inputs/threshold/cadence; skip only eligible unchanged broadcast profiles. Tests TX4. |
| `activation-tx-repeat` | 600 s | Original behavior again, to expose drift or a one-off OLT opportunity. |
| `activation-tx-quiet` | 600 s | Original behavior, four-second delay between snapshots instead of one; tests diagnostic overhead. |

All cases start with fresh ownership. Actual times and opportunities are evidence;
the configured deadline is not a guaranteed count of SN opportunities. Functional
negatives retain a further 30 samples. O5 automatically branches to the existing
OMCI/provisioning/DHCP/traffic checks. No identity selects RX only. `--cases` can
select a subset; `--suite legacy` retains the earlier recovery/physical cases.
A normal-registration reconnect check remains optional and needs one prompted
outage; it is not part of the default TX suite.

## Hypothesis-to-observation map

| ID | Hypothesis | Implemented signal | Interpretation limit |
|---|---|---|---|
| T01 / TX1 | Incomplete laser drive or missing light | Controller `transmitter_status`: 22 fields including bias `0x64`, internal TX power `0x66`, modulation `0x6a`, raw current codes, limits, loop, BEN, TX-disable, OCP and custom-function flags. | A new bus read does not prove MCU sensor refresh. Zero/constant DDMI during sparse bursts is inconclusive. Connector emission remains unverified. |
| T02 / TX2 | Burst gating, format, clock or serializer issue | MAC `TX_BST_CNT` at `0x5944`; 12 additional PHY TX control words; existing passive clock meters; controller BEN/drive fields and owned MAC/PHY error events. | MPI SOF is internal, BEN is instantaneous, passive frequency results can be stale. No optical waveform is measured. |
| T03 / TX3 | Hardware response template differs | `US_RATE_CAP`, response/random/extended delay, reply mode, profile valid/version/length, selected integrity-key indices; exact identity-write success event. | No identity/key words are exported. Register readback is not an on-fiber packet capture. |
| T04 / TX4 | Repeated unchanged profiles interrupt discovery | Immutable `bench_profile_coalesce`; baseline/coalesced/baseline; exact skip counts (event 24, IDs 0–3) and mismatch/error counts (IDs 4–7). | Removing quiesces does not by itself prove successful OLT reception. Compare assignment/ranging and opportunity counts. |
| T05 / TX5 | Assignment lost locally / event loss hides it | Existing owner-side PLOAM verify/dispatch, assignment and error accounting; independent 1024-entry critical stream alongside the 4096-entry main stream. | No diagnostic reader consumes FIFO data or clears IRQs. Each stream reports its own coverage gaps. |
| T06 / TX6 | Reset threshold or observer overhead | Fixed threshold 40, explicit reset events, sparse-sampling comparison, retry only explicit EAGAIN transition records. | Real controller/PHY faults still stop the run. Cached snapshots remain postmortem evidence. |

`transmitter_status` serializes 22 reads under the controller lock. DDMI words
are BE16; CSR words are LE32; the feature bitmap at `0xe8` is one byte
(as in OEM `read1Byte`). `0xff` is retained as a valid feature bitmap. Every field includes register, raw, value, unit,
validity and errno. Zero is retained; all-ones in multi-byte fields is unavailable with its sentinel
retained; failed transfers have no raw value. Whole-read begin/end timestamps
bound observation age but cannot establish the MCU's internal sensor age.
Read at initialized TX-off, during live snapshots, and after MAC stop at 0/1/2 s
before PHY removal powers the controller off. Cleanup continues if a final
sensor read fails, and explicitly records the missing observation.

MAC schema 2 omits reserved `0x5958/0x595c` and correctly identifies OMCI at
`0x5960/0x5964`, XGEM at `0x5968/0x596c`. `0x5920` is invalid-profile
burst grants and `0x5984` is transmitted ACK PLOAM; earlier placeholder labels
for these two are corrected. Cold MAC generations separate counter
resets from modulo-32-bit deltas. Fast schema 2 emits `available:false` only for
EAGAIN, with no fabricated frame counters; real errors propagate. Coherent
snapshot EAGAIN retries stop after five attempts and preserve every gap record.
RX/diagnostic schema 5 has additive TX fields; historical RX report schemas stay
frozen so old captures do not acquire new mandatory fields.

At discovery IRQ bits 2–5, the serialized MAC worker records 14 ordinary MAC
words immediately, with IRQ flags and cold generation. It performs no I2C and
reads no FIFO/ACK or private identity/key words. Critical records retain their
original main sequence and independent positions. The collector deduplicates
both streams without using critical coverage to conceal main-stream loss.

## Coalescing guard

Default driver behavior remains unchanged. The experimental case permits a
no-op only for an unacknowledged O2/3 broadcast matching a completed software
profile and PON tag, with valid keys, matching hardware key indices, healthy
protocol state, and no pending reset/profile/assignment/ranging/key/ACK work.
It then checks all six current PHY pattern/FEC words and MAC valid/version/length
readbacks. This live comparison replaces an assumed hardware generation match:
a reset or changed hardware field causes a real installation. Changed contents
under the same version, new tags and ACK requests still take the existing path.
Read errors contain the protocol rather than accept an uncertain no-op.

## OEM evidence used

The extracted QKX001-06.00.44.00 firmware is a static reference, not an OEM
hardware run. Its `en7572.ko` has `AdaptivePon` at `0x5b0c`, with BEN gating,
calibrated current/monitor loading and loop restart. Initialization conditionally
starts `reduce_Imod_task_wait` if `0xe8[0]` is clear. Its `bob_info` reads the three
DDMI words used here. `ddmi_tx` and `mpd_current` change calibration/TX/ADC state;
neither is used as a passive probe.

OEM `phy_tx_ctl` at `0xd8d0` calls `ledTurnOff(42)` for enable and
`ledTurnOn(42)` for disable. `tcledctrl.ko` resolves that through an eight-byte
notifier table. The static entry at `.data+0x1b8` has mode byte zero, making
that call a no-op until configuration changes the table. This does not establish
the runtime mapping or justify an arbitrary GPIO write. Existing Q1000K board
profile 82 matches the OEM constants; the isolated polarity case toggles only the audited PMA BEN bit and restores it.

OEM `xpon_10g.ko` (`bd01941044584af4eee9fda21541f089f1f26d4d0ba3765b7986ae16dd304017`)
has `gponDevSetSerialNumber` at `0x11538`, writing the same big-endian serial
words to `0x500c/0x5010`. Its mode defaults use response time `0x1600` for XGS.
Its profile handler calls MAC profile installation at `0x8fec` and PHY profile
installation at `0x906c`, without the replacement's full drain transaction in
that handler. The replacement's measured repeated TX-off intervals remain the
reason for the controlled coalescing experiment.

The factory startup sources FSAN from the DSD-derived environment. That factory
identity is not substituted for the user-supplied provider identity. No new
registration-ID requirement is introduced; an unspecified ID remains 36 zeros.

## Verification and release

Meaningful host fixtures cover read-only access, BE/LE conversion, zero/sentinel
and every field's transfer failure; current MAC/PHY profile readbacks, changed
payload/tag, ACK/reset/pending work and error containment; critical retention,
EAGAIN versus hardware errors, counter wrap/reset, SSH stderr separation,
physical-free case selection, cleanup and cancellation. The build wrapper runs
the full PON/status/UI checks and inspects the packed FIT, scripts, module symbols,
capabilities and hashes. Build results and artifact hashes belong to the release
checkpoint. Compilation is not an optical emission or O5 result.
