# Q1000K discovery and receiver recovery: 2026-09-16

## Outcome

Image `be8e34c5f6` receives downstream frames reliably at fresh startup with
TX inhibited. After a physical outage, deliberately passive RX mode fails
to reacquire; the checked PMA out/in sequence restores frames. An independent
outage reproduced that result with the same action alone. Normal activation
callbacks also restored frames after the third reconnect, although a transient
diagnostic-read failure ended that observation early.

The activation comparisons have not established O5 or subscriber service.
Accepted XGS burst profiles reach hardware, and the MAC reports discovery
requests and SN-response-sent interrupts. These local events do not prove a
correct optical burst reached the OLT.

Normal internet has recovered. The follow-up source comparison and next-image
test specification are in [TX and discovery plan](XGSPON-TX-DISCOVERY-PLAN.q1000k.md).
Hardware collection has ended and cleanup is verified; the fiber is now
disconnected. No hardware experiment was performed during this offline analysis.

## Reproducibility

- Firmware source: `be8e34c5f69893294a669519c50b1484e41bcdbe`.
- FIT SHA256: `cd3b4c13f094c8b05ebfac62003297ae165250cb200dffcddf824e586219a8f2`.
- FIT size: 8,388,608 bytes; RAM boot, NAND disabled.
- Boot ID: `d2a043e8-2901-4ec8-bc7f-fb74941c3cae` throughout these runs.
- Current portable collector: `q1000k-collect-be8e34c5f6-v4.py`, SHA256
  `a5306ed2d9aee104d6a5e843abbdf9a9bfb213597672913ae25de306dc5f358c`.
- Collector source revision: `3218bda5b7`; later documentation changes do not
  change the running firmware or the portable collector.
- The original connected controls used the collector packaged with the image;
  the successful physical controls and longer-threshold comparison use v4.
- Identical unit DSD calibration (513 bytes), OEM PM (15,232 bytes), OEM DM
  (56 bytes), automatic OEM receiver output initialization, subscriber identity
  and BGW320 presentation remain fixed. Registration uses 36 zero bytes.
- Private inputs remain separate from firmware, evidence and Git. No optical
  controller/firmware/calibration replacement occurred during the winning
  PMA-only action. All 16 pinned runtime files remain unchanged.

## Tests, hypotheses and actual coverage

Hypothesis IDs refer to the [test map](XGSPON-DISCOVERY-BENCH.q1000k.md).

| Test / collector case | Hypothesis | Actual result | Interpretation / limit |
|---|---|---|---|
| B01: preflight, input and runtime guards | Known inputs | Image/runtime/input checks pass | Same RAM firmware and verified OEM inputs across comparisons. |
| B02: `rx-startup`, `rx-repeat-1`, `rx-repeat-2` | H5/H6: receiver initialization | 90/90 live samples synchronized | First power reading −18.48 dBm, LOS clear, advancing frames, TX inhibited. |
| B03: `rx-soak` | H5–H7: stable reception | 180/180 synchronized samples, about 210 seconds | No sampled LOF or uncorrected FEC; approximately 1.67 million frames. |
| A01: `activation` | H1–H4: profile, response, assignment, reset | 223/223 RX samples synchronized; activation timed out | 35 SN-request and 35 SN-sent interrupts; no assignment/ranging/O5; threshold reset observed once. |
| A02: `activation-quiet` | H7: diagnostic overhead | 85/85 RX samples synchronized; activation timed out | 42 SN-request and 42 SN-sent interrupts; no assignment/ranging/O5; two threshold resets. Reduced full-snapshot rate did not resolve activation. |
| A03: `activation-long-sn` | H4: reset discards discovery progress | 222/222 RX samples synchronized; activation timed out | 36 SN-request and 36 SN-sent interrupts, no reset, no assignment/ranging/O5; zero event sequence gaps. |
| A03 extension: `activation-long-sn`, 600 s | H4: observe the fixed-40 transition and longer discovery | 513/513 RX samples synchronized; activation timed out | 94 SN-request and 94 SN-sent interrupts, two threshold resets, no assignment/ranging/O5; 25,449 missing event sequence positions. |
| A04: profile shadow decoder, queue/commit events | H1/H7: decoding or repeated installs | Required-rate profiles accepted; byte shadow matches actual bitfields; many identical reapplications | Hardware profile installation works. Repeated commits quiesce TX; causality for activation failure remains unproved. |
| R00 + R01: `rx-reconnect` | H5/H6: passive recovery and PMA state | No passive reacquisition; action 1 restored frames | 36 confirmed dark samples; 60-second bright passive window; 26 healthy recovery samples. |
| R08: `rx-confirm` | H6: independent PMA-only confirmation | Same failure and same recovery reproduced | 37 confirmed dark samples; fresh failure, action 1 alone, 25 healthy recovery samples. |
| R09: `activation-reconnect` | H5: normal callback behavior | Frames returned without a manual RX action; capture incomplete | 174 confirmed dark samples; 30.452 seconds of synchronized advancing-frame observations after the observed bright edge. Fast diagnostic read returned EAGAIN before the 60-second window finished; cleanup passed. |
| R02–R07: larger recovery actions | H6: PLL, analog, PHY, MCU/DSD or full stack | Not run | Action 1 succeeded, so the ladder stopped. Do not claim these hypotheses were independently tested here. |
| R10/R11: dark startup, short/long outage variations | H5/H6 | Not selected | No additional physical controls justified by the reproduced PMA result. |
| D01–D06: stable O5, OMCI provisioning, IP/traffic | H8 | Prerequisites not met | No subscriber-service claim. Directional GEM counters were not actually captured; see correction below. |
| V4 cancellation control | Collection teardown | Intentional Ctrl-C preserved postmortem and completed cleanup | One postmortem begin/end pair, cleanup passed, host record identifies intentional KeyboardInterrupt. |

## Receiver recovery evidence

| Observation | First outage | Independent repeat |
|---|---:|---:|
| Frames frozen after light returned | 262,370 | 654,726 |
| Both LOS sources clear, but passive reacquisition fails | Yes | Yes |
| Manual action sequence | `1` | `1` |
| Checked action duration | 20.714 ms | 20.696 ms |
| First fast sample after completion | 255.260 ms | 253.825 ms |
| First fast sample sync / frames | 2 / 2,109 | 2 / 2,097 |
| First full sample after action | synchronized, 2,408 frames | synchronized, 2,432 frames |
| Timing errors / event sequence gaps | 0 / 0 | 0 / 0 |
| Checked cleanup | Passed | Passed |

Action 1 calls `q1000k_phy_rx_reacquire(false, false)`: checked controller/PHY
health, `fiber_plug_reset(PLUG_OUT, wan_sel)`, then
`q1000k_phy_pma_reset()` through the existing PMA insertion branch, followed
by health checks. It is a bounded sequence, not a single-bit experiment.
The extra PLL-restoration and gain-restoration options are disabled. The
controller, its MCU programs and DSD calibration remain loaded.

The first fast sample bounds observed recovery to approximately 0.26 seconds
after action completion; it does not measure the exact instant of CDR lock.
Raw `rx_frequency`, `pll_status` and analog words changed, but are not a
measurement of clock frequency or proof of a particular analog mechanism.

Normal-mode reconnect gives a useful distinction. The last dark sample was
followed by a sample at monotonic 3,515,234 ms with both LOS sources clear,
sync present, TX enabled and 1,056 frames. Frames advanced to 244,675 at
3,545,686 ms. No `bench_recover` action ran. RX-only mode deliberately skips
the vendor polling/IRQ registration callbacks; normal mode dispatches them.
This supports absent recovery in passive RX mode as the explanation for that
test's persistent no-frame state. It does not establish long-term normal
reconnect reliability or post-O5 re-registration.

## Discovery evidence

### Profiles and repeated TX interruptions

The explicit-byte shadow decoder agrees with actual bitfields. Profiles
0, 1 and 3 with required rate 1 are accepted, queued and committed. The other
rate is rejected with `-EOPNOTSUPP`; this alone does not indicate rejection of
the required XGS profile. Observed profile verification succeeds. Cold MAC
identity readback matches the configured identity (only a boolean is exported).

| Detailed / quiet comparison | Normal | Quiet |
|---|---:|---:|
| Exact profile request count | 828 | 825 |
| Exact completed profile transactions | 550 | 555 |
| Retained requests with identical profile and tag | 816 | 816 |
| Retained TX-off intervals | 542 | 549 |
| TX-off interval range | 11.272–28.884 ms | 11.128–19.878 ms |
| Total retained TX-off time | 8,121.578 ms | 8,192.217 ms |
| Missing event sequence positions | 10,815 | 35,957 |

Interval counts/totals describe retained evidence and can undercount because
of missing events. Exact counters and retained first occurrences establish
that repeated identical work and threshold resets occurred, but do not restore
missing chronology. The quiet comparison still records events; it changes
full-snapshot frequency and therefore does not isolate event-instrumentation
overhead.

### Hardware discovery replies, resets and remaining gap

Normal and quiet runs report equal SN-request and SN-sent interrupt counts
(35/35 and 42/42). No assignment, ranging request, registration completion or
O5 is recorded. A local SN-sent interrupt is evidence from the MAC, not proof
that the OLT received a valid timed optical burst.

The default reset caller is now directly observed: `gpon_sw_resync()` fires
with state 2, SN count 21 and threshold 20. Normal and quiet runs have one and
two exact reset events respectively. The hardware PLOAM counter resets with
the MAC; its final value must not replace the cumulative interrupt count.

Early TX error status `4` is `tx_prof_invld_err` in the imported register
definition. Invalid-profile grants increase around startup/reset and then
stop increasing while valid profiles are installed. Large early IRQ/event
bursts overflow the ring, so the exact causal ordering is incomplete.

### Longer SN threshold comparison

The separately selected fixed-40 case completed with the same image, inputs
and bounded activation window, with fiber continuously connected:

- 222/222 live RX samples synchronized over 275.026 seconds including the
  post-timeout observation; 2,200,230 advancing frames between the first and
  last live sample. RX power was −18.48 to −18.07 dBm.
- 36 SN-request and 36 SN-sent interrupts; no threshold reset or other reset
  event, no assigned ONU ID, no ranging request and no O5. Sampled state
  remained 2. Activation timed out after its 240-second observation window.
- Zero event sequence gaps, zero timing errors, no retained PHY fault, no
  sampled LOF or uncorrected FEC, and successful cleanup.
- 831 profile requests, 828 already-identical profile/tag requests and 553
  completed transactions. The 552 retained TX-off intervals range from
  11.291 to 30.003 ms and total 8,284.151 ms.
- Invalid-profile grants reached 4,154 early and then stopped increasing.
  The final hardware TX PLOAM counter was 38, while the counted SN-sent
  interrupts were 36; these represent different counters and must not be
  presented as interchangeable measurements.

This capture shows an activation failure without any threshold reset. The
20-response reset therefore is not the sole blocker within this window.
The test did not reach count 41, so it does not verify the reset transition
at threshold 40 or rule out all effects of the reset policy. There is no
evidence here that simply increasing the limit enables registration.

### Extended 600-second observation

The host wrapper selected the same fixed-40 case with a 600-second activation
window, within the existing helper's supported range. It changed no firmware,
driver, calibration, identity or runtime file. Including aftermath:

- All 513 live RX samples synchronized over 636.184 seconds; frames advanced
  from 541 to 5,090,057 (delta 5,089,516). RX power was −18.477 to −18.097 dBm;
  sampled LOF and uncorrected FEC remained zero.
- Exact SN-request/SN-sent counters were 94/94, with no assignment, ranging,
  registration completion or O5. There were two fixed-40 threshold resets.
  The retained first reset records state 2, count 41, limit 40 and PHY-reset
  requested. A later ordinary event records the same transition at count 41.
  This verifies the threshold behavior that the shorter case did not reach.
- There were 1,900 exact profile requests and 1,261 completed transactions.
  Of retained requests, 1,888 had an already-identical profile and tag.
  The 1,252 retained TX-off intervals were 11.172–22.016 ms, totaling
  18,762.671 ms. This is a lower bound because 25,449 event positions are
  missing. The missing history keeps host chronology inconclusive even
  though the activation stage itself records a functional timeout.
- One FIFO-read event reports depth zero and the instrumentation's synthetic
  `-EMSGSIZE`; that trace site assigns this error to any nonpositive depth.
  It does not establish a malformed optical packet. No such event occurred
  in the reset-free fixed-40 case. Preserve and improve this observation.
- Cleanup passed, no retained PHY fault was recorded, and the captured serial
  interval contains no panic, Oops, BUG, Call trace, WARNING or KASAN marker.

All 36 request and 36 sent IRQ observations in the gap-free shorter case
occurred after the last recorded successful TX-enable event. The extended
case retains 92 of each IRQ, also after successful TX-enable events; exact
counters retain all 94. IRQ timestamps describe software observation, not the
optical envelope. Thus the current evidence does not show discovery replies
being issued while the recorded gate was off, and does not prove repeated
profile installation caused the failure. It remains a concrete source-level
disturbance worth an isolated comparison.

The PLOAM verification/dispatch counters contain only message type 1
(Burst_Profile). Verification is traced before legacy dispatch/local-serial
matching, and duplicate suppression has its own pre-verification trace.
This weakens a failure confined to the Assign_ONU-ID serial-match handler.
It does not prove the OLT never transmitted an assignment: MAC filtering,
FIFO delivery and missing event history remain boundaries to instrument.

## Collection limitations and corrections

1. **Transient diagnostic reads:** normal reconnect stopped on
   `cat: read error: Resource temporarily unavailable` from
   `/proc/q1000k-pon-fast`. The source returns `-EAGAIN` when PHY sampling is
   temporarily inactive and explicitly does not mark EAGAIN as a PHY fault.
   Profile transactions quiesce sampling; a transaction begins in the final
   retained events. This is consistent with a diagnostic/lifecycle race, but
   missing chronology prevents assigning the precise cause. The helper labels
   every fast-read failure `containment-failure`; retain that raw result.
   Cleanup passed and no retained PHY fault event was recorded. The next
   helper should distinguish bounded EAGAIN retries from hardware faults.
2. **Fractional delays:** the image's BusyBox rejects `sleep 0.2` and
   `sleep 0.25`. V4 embeds a verified 6,096-byte AArch64 helper in staged RAM
   inputs; observed checks measured 200 ms and 260 ms at 10 ms uptime
   resolution. Integer sleep delegates to BusyBox. The successful controls
   have zero timing errors. The firmware itself was not rebuilt or replaced.
3. **Event retention:** earlier host reporting overlooked retained first reset
   records after ring wrap. V4 uses retained first records and exact counters,
   while preserving gap/inconclusive flags. It also permits an explicitly
   selected fixed-40 comparison after a separate default-threshold capture.
4. **Register labels:** the image reads reserved `0x5958/0x595c`, which return
   `DEADBEEF`; exclude them. `0x5960/0x5964` are RX/TX OMCI counters.
   Actual RX/TX XGEM counters are `0x5968/0x596c` and were not collected.
   Correct the next image's register list and manifest. Original immutable
   image/kit records are preserved, with this correction alongside them.
5. **Interrupted earlier attempt:** an older host collector closed SSH during
   Ctrl-C, so postmortem output hit SIGPIPE before teardown. Subsequent normal
   ordered manual cleanup passed; this is separate from the earlier kernel
   shutdown problem. V4 preserves SSH output while sending TERM to the
   validated helper and draining its cleanup output. Hardware cancellation
   verification passed: intentional Ctrl-C retained exactly one postmortem
   begin/end pair, the cleanup stage passed, and the final host record has
   `cleanup_verified=true`. Evidence is in the cancel-check archive.
6. **Incomplete attempts:** the timing-affected attempt and an expired
   correctly timed physical window had no confirmed outage and contribute no
   recovery evidence. Preserve their original failed/inconclusive records.
7. **Boot ID formatting:** raw collector JSON includes an SSH known-host
   warning before the UUID. The UUID above is extracted without changing the
   raw records. A future host update should separate SSH stderr from values.

## Next focused work

1. Fix diagnostic EAGAIN handling and preserve reset/error/assignment events
   independently of repetitive IRQ bursts. Capture exact counts separately
   from a bounded chronological event stream.
2. Add one controlled comparison that avoids reapplying an unchanged profile
   and tag only when the installed hardware generation is still valid. Keep
   reinstall after reset, changed profile/tag and failure handling intact.
   Compare SN opportunities/responses, TX-off duration and OLT assignment.
3. Audit the hardware-generated Serial_Number_ONU response template, byte
   order, advertised capabilities, reply timing and active burst profile
   against the OEM path. Continue to export local-match/readback booleans,
   never subscriber identity bytes.
4. Determine whether actual optical discovery bursts reach the OLT with
   correct timing/levels using verified OEM behavior or suitable external
   measurement. A TX gate or SN-sent interrupt cannot answer this.
5. Keep proven receiver initialization and OEM MCU/DSD inputs fixed. Avoid
   another broad analog sweep without a specific contradictory observation.
   Package justified comparisons into one image and collector. Reuse the
   physical controls already collected; require a new disconnect only for a
   necessary new distinction.

## Evidence locations

All paths below are under
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/`:

- `capture-discovery-be8e34c5f6-connected-20260916/`: startup, soak, normal and
  quiet discovery, serial interval, raw records and checksums.
- `capture-discovery-be8e34c5f6-live-controls-20260916/`: both successful
  PMA-only controls and the incomplete normal-callback reconnect capture.
- `capture-discovery-be8e34c5f6-long-sn-20260916/`: fixed-40 comparison.
- `capture-discovery-be8e34c5f6-long-sn-600s-20260916/`: extended fixed-40
  observation; the later `helper-idle-observation.log` is preserved separately
  in the consolidated bundle, without changing the original archive.
- `capture-discovery-be8e34c5f6-cancel-check-20260916/`: hardware cancellation
  and cleanup verification.
- `q1000k-collect-be8e34c5f6-v4.py`: portable collector for this exact image.
- `physical-timing-interruption-cleanup-20260916.log`: ordered cleanup of the
  old interrupted attempt.

Raw archives remain immutable. The consolidated evidence bundle adds offline
analysis, this report and collector provenance without private firmware,
calibration or subscriber identity payloads.
