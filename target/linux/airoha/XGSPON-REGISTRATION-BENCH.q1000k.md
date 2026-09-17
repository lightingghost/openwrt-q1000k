# Q1000K registration bench: hypotheses 1–5

## Problem and evidence

The previous connected-fiber bench receives matching Assign_ONU-ID and
Ranging_Time messages, reaches local O5, and then receives Deactivate_ONU-ID.
It has not received OMCI provisioning. Local O5 alone is not service success.

The final controlled capture had 14 assignments, 14 accepted ranging messages,
13 local O5 transitions, 14 deactivations and 14 Key_Control dispatches (one
rejected), with no Key_Report enqueue. Across three instrumented captures,
37 Key_Control dispatches produced no Key_Report enqueue. Required ranging
processing through ACK enqueue took 143.201–197.825 ms in the final capture.
These are local processing timestamps, not optical arrival times. The captures
support delayed processing as a hypothesis; they do not establish the OLT's
reason for deactivation.

Evidence is preserved in `build-artifacts/q1000k-xgspon/registration-hypotheses-20260917/`
and `hypothesis-control-generation-6470e31769-20260917/` in the workspace.
No hardware experiment in this document has yet run on the new image.

## One image and seven connected-fiber comparisons

Run the portable activation collector with `--suite registration`, the private
calibration/MCU archive and the already approved subscriber identity. The suite
begins with a TX-inhibited RX power/synchronization check. Keep the fiber
connected throughout. Each comparison loads a fresh stack and verifies cleanup.
The default registration observation is 300 samples per case, with full PHY/I2C
snapshots every fifth sample and lighter MAC/OMCI observations between them.
Negative cases also retain a 30-second aftermath. Allow roughly 45–60 minutes;
service/provisioning/traffic checks, if reached, can extend the suite.

| Collector case | Ranging operation | Key_Control handling | Purpose |
| --- | --- | --- | --- |
| `rx-startup` | TX inhibited | None | RX power, light/frame baseline |
| `activation-reg-reference` | Existing full table transaction | Existing queued transaction | Matched reference with diagnostics |
| `activation-reg-range` | Narrow TX drain/resync, clear command bits | Queued | Isolate H1 ranging delay and part of H4 |
| `activation-reg-keys` | Full table transaction | Inline checked inactive-bank update/report | Isolate H2 scheduling/install delay |
| `activation-reg-combined` | Narrow TX drain/resync, clear bits | Inline | Interaction of H1 and H2 |
| `activation-reg-resync-retain` | Narrow TX drain/resync, retain command bits | Inline | H4 completion policy |
| `activation-reg-oem-direct` | Checked direct EqD write on initial O4 | Inline | H4 NAND-derived initial-ranging sequence |
| `activation-reg-repeat` | Same as combined | Inline | Reproducibility after the comparisons |

All activation cases use the same identity, analog initialization, SN threshold
40, verified unchanged-profile coalescing in O2/3 through O5, completed-control
coalescing, diagnostics and polling cadence. The reference therefore matches
the last coalescing experiment, with the new diagnostic instrumentation.
Identity changes are not part of this matrix. Hardware modes are immutable for
the lifetime of each module load.

## Hypothesis-to-evidence map

| Hypothesis | Intervention / collection | Evidence that supports it | Evidence that weakens it or redirects investigation |
| --- | --- | --- | --- |
| **H1: a broad ranging/control transaction delays the ACK and subsequent RX handling** | Reference versus range; timestamps for Ranging_Time entry, control start/end, RCU wait, FIFO enqueue and latest local IRQ indication | Narrow path substantially shortens ACK delay and permits key exchange/OMCI, repeatably | Delay falls but OLT still deactivates at the same stage; investigate TX delivery/MIC or other control transactions |
| **H2: Key_Control waits behind work or is cancelled before reporting** | Keys and combined; inline Generate/Confirm preparation, checked inactive bank writes, explicit fallback/cancellation events, request-to-enqueue duration | Key_Report is enqueued before reset and OLT progresses to Confirm or OMCI | Valid reports are enqueued promptly without OLT progress; inspect H3/H4/H5. If inline eligibility fails, this comparison has not tested the proposed intervention |
| **H3: O4→O5 leaves upstream TX, grants or FIFO unusable** | All cases; 26 MMIO values at ranging, pre/post enqueue and immediately before reset turns TX off | Stop bits remain set, profile validity disappears, invalid-profile grant count rises, FIFO fails to drain, or burst/PLOAM counters stop despite grants | FIFO/counters advance with intact state. This still does not certify optical burst reception by the OLT |
| **H4: EqD units or resynchronization sequence are wrong** | Compare full, narrow clear, narrow retain and initial-O4 direct write; record received EqD, multiplied value, readback, state, profile/stop/FIFO/resync registers | One sequence reproducibly advances activation; mismatched EqD readback or stuck resync is directly observable | All eligible variants write identical correct EqD and fail similarly. The NAND uses the same ×4 conversion, reducing the basis for arbitrary scale experiments |
| **H5: upstream control message formatting or MIC inputs are wrong** | ACK/Key_Report type/sequence/ONU/PIK-selector validation, full integrity/wrapping key-bank readback equality, fixed wire-layout and independent crypto vectors on host, FIFO completion and response counters | Input/selector/readback audit fails, wrong sequence/length is detected, or corrected byte layout changes results | Host vectors and hardware inputs match yet no OLT progress. Actual emitted MIC and optical waveform remain unmeasured |

A useful endpoint is progression from Generate to Confirm and then received OMCI,
followed by provisioning and PON-bound traffic. The suite retains OMCI, service,
DHCP, traffic and cleanup diagnostics if those stages are reached. It always
honors OLT deactivation and never forces the state to O5.

## OEM and community evidence

The actual Q1000K NAND `lib/modules/5.4.55/xpon_10g.ko` was extracted and
statically disassembled. SHA-256:
`099d4ff0601c263d9a44f2f89d3604c770820936bc94041bf4763d5f10a5f1e5`.
It was not booted or executed. Local extraction/disassembly provenance is in
`build-artifacts/q1000k-xgspon/registration-next-20260917/`.

- `gponDevSetEqdValue` begins at `0x17c00`. The absolute XGS branch shifts the
  received EqD left by two at `0x17c78` and writes register `0x5114` at `0x17dac`.
- The initial O4 ranging handler calls this function at `0x98c4`, then transitions
  to O5 at `0x99fc`. This initial O4 path does **not** invoke software resync.
  Mode 3 adapts this operation with ownership/state/range/readback checks.
- For adjustments that need resync, `gponDevSwResync` at `0x19740` sets bits 8
  and 0 of `0x582c`, polls ready bit 31, and leaves command bits set.
- The complete helper at `0x19804` saves profile validity, stops MBI TX,
  invalidates profile bits, waits for TX alignment FIFO empty, stops MPI TX,
  performs resync, releases MPI, restores profiles and releases MBI. Our bounded
  modes are checked adaptations, not a claim of instruction-for-instruction
  equivalence. Mode 1 clears command bits after ready; mode 2 retains them.
- Sirherobrine's cached `airoha_xpon.c` EqD callbacks implement **GPON** byte and
  PHY-bit delay splitting. That arithmetic is not a valid XGS EqD alternative.
  PR 24577's GPON work remains useful for distinguishing internal laser/burst
  indicators from proven optical delivery; it does not supply an interchangeable
  Q1000K XGS ranging algorithm.
- The 8311 configuration references do not disclose the closed hardware's
  upstream scheduler/MIC implementation. They support retaining the approved
  identity recipe while testing this driver's transaction behavior.

Reference repositories: [Sirherobrine kernel](https://github.com/Sirherobrine23/airoha_kernel),
[OpenWrt integration](https://github.com/openwrt/openwrt/compare/main...Sirherobrine23:openwrt:airoha_en7523),
[PR 24577](https://github.com/openwrt/openwrt/pull/24577),
[8311 builder](https://github.com/djGrrr/8311-was-110-firmware-builder).
Source comparisons use cached downloads; the NAND-derived path has priority
for Q1000K-specific initial-ranging behavior.

## Retention and interpretation

New trace events 26/27 carry activation snapshots and control timing. The
independent critical ring holds 8192 records. `capabilities.json` contains the
field map, register list and boundary stage IDs. The collector deduplicates
ordinary/critical/first copies, groups by stack load, and never pairs requests
across a reset. It reports missing critical positions including truncated tails,
incomplete register blocks, ineligible/fallback paths and cancelled key work.

Registration case outcomes use the critical stream's completeness. Ordinary
ring loss remains recorded separately; it must not erase retained activation
evidence or be misrepresented as complete ordinary history. `registration-summary.md`
and each case JSON contain the paired timing samples and boundary readbacks.
The latest IRQ timestamp identifies a deferred local batch, not the arrival of
each PLOAM. Full I2C observations are deliberately outside the boundary snapshots.

Hardware key equality checks return only match/mismatch and selector metadata.
Neither private identity nor key/report payloads are included in the diagnostic
trace. Successful FIFO enqueue is distinct from hardware transmission, and both
are distinct from OLT acceptance. Internal power and burst counters cannot
certify individual optical bursts, modulation or their emitted MIC.

## Validation before release

The build must pass the host suite, shell validation, and extraction/inspection
of the actual initramfs image. New tests cover:

- all three narrow EqD modes, every MMIO-write failure, drain/state guards;
- inactive-bank-only Generate, bounded Confirm switch, preservation of the
  active key, provider failures and readback mismatches;
- backend fast-path eligibility/fallback, no O5/ACK after failed ranging,
  immediate report before subsequent reset, and failure containment;
- actual vendor ACK/Key_Report structures and formatters versus the new
  formatter and independent expected bytes; existing AES/CMAC vectors remain;
- snapshot read restrictions, deduplication, reset-separated timing, critical
  loss and the exact launcher parameter matrix;
- actual ELF symbols/module parameters in the finished image and portable
  collector execution without repository access.

This is a RAM-boot bench. Keep the firmware and single-file collector together
by their pinned revision/hashes. No subscriber identity or OEM binary belongs
in the distributable release bundle.
