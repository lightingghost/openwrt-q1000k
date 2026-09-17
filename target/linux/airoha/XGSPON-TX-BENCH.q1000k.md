# Q1000K internal TX and discovery bench

Implemented 2026-09-16 on `q1000k-xgspon`. Hardware results pending.
One activation RAM image, one pinned portable collector. No external optical
meter is available. No NAND writes or extra fiber outages are needed for the
default suite. Use the existing private unit MCU/DSD archive and subscriber JSON.

## Collector cases

The portable collector defaults to `--suite tx`. With an identity, it selects:

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
are BE16; CSR words are LE32. Every field includes register, raw, value, unit,
validity and errno. Zero is retained; all-ones is unavailable with its sentinel
retained; failed transfers have no raw value. Whole-read begin/end timestamps
bound observation age but cannot establish the MCU's internal sensor age.
Read at initialized TX-off, during live snapshots, and after MAC stop at 0/1/2 s
before PHY removal powers the controller off. Cleanup continues if a final
sensor read fails, and explicitly records the missing observation.

MAC schema 2 omits reserved `0x5958/0x595c` and correctly identifies OMCI at
`0x5960/0x5964`, XGEM at `0x5968/0x596c`. Cold MAC generations separate counter
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
profile 82 matches the OEM constants; its polarity is observed, not guessed.

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
