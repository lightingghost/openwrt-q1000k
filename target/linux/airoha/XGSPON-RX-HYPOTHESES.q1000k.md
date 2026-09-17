# Q1000K light-present/no-frames hypotheses and bench tests

**Latest discovery/recovery result, 2026-09-16:** image `be8e34c5f6`
reproduced passive RX reconnect failure and recovered it twice with the
checked PMA out/in action alone. Normal registration callbacks also restored
frames after reconnect for 30.452 seconds before a transient fast diagnostic
read stopped that capture; normal reconnect acceptance remains incomplete.
Required XGS burst profiles reach hardware and the MAC reports SN requests
and responses, but no assigned ONU ID or O5. A fixed-40 comparison recorded
36 SN-sent interrupts without any reset and still timed out, with no event
sequence gaps. The 600-second extension recorded 94/94 SN IRQs and two
fixed-40 resets, still without assignment. Prioritize physical TX output,
burst enable/timing, hardware response content and a controlled comparison
of repeated profile installation. TX-disable readback is not an optical
measurement. Keep the proven receiver initialization and MCU/DSD inputs fixed.
See [full discovery results and coverage](XGSPON-DISCOVERY-RESULTS-20260916.q1000k.md).

**Next image specification:** [TX/discovery hypotheses, reference comparison
and test map](XGSPON-TX-DISCOVERY-PLAN.q1000k.md). Prepared after normal internet
recovery using the requested PR, both Sirherobrine23 trees, 8311 and Q1000K
OEM disassembly. It identifies omitted TX diagnostics and separate burst
gates; new tests are specified, not yet implemented or built.

**Previous consolidated bench result, 2026-09-16:** image `280555a076`
automatically receives frames at fresh startup: 90/90 startup/reload samples
and 180/180 soak samples synchronized with TX inhibited. TX-enabled activation
retained sync but timed out in O1/O2 with PLOAM rejections; it did not reach O5.
The confirmed passive reconnect test failed to resume frames despite restored
light and the OEM startup bit remaining set. A fresh stack initialization on
the same boot recovered sync in 30/30 samples. Registration ID now defaults to
36 zero bytes, matching 8311; no separate AT&T credential was required to run
the test. Prioritize PLOAM rejection diagnostics and bounded LOS-clear receiver
recovery. See [complete activation/reconnect evidence and next tests](XGSPON-ACTIVATION-RESULTS-20260916.q1000k.md).

**Implemented image plan:** [discovery, recovery and evidence collection](XGSPON-NEXT-BENCH-PLAN.q1000k.md)
packs 25 test/control entries into one image. Follow-up comparison demotes the
OCP value as a fault clue, finds evidence of some accepted profile processing,
and distinguishes deliberately passive RX reconnect from normal-mode recovery.

**Earlier isolated result, 2026-09-16:** the isolated OEM controller
`0x110[8]=1` step recovered downstream synchronization and frames in two
freshly initialized receive-only runs. The longer confirmation sustained
about 8,000 frames/second for 192 seconds after the change, with TX disabled.
The combined calibration case stopped on a diagnostic guard; it is incomplete,
not a negative optical result. See the [results and exact coverage](XGSPON-RX-RESULTS-20260916.q1000k.md).

**Earlier replacement:** `8fee7f3b48`, 58 cases, [repeated acquisition plan](XGSPON-REPEAT-RX.q1000k.md).
All software checks passed; hardware collection is recorded above. The [MCU loader
audit](XGSPON-MCU-LOADER-AUDIT.q1000k.md) confirms the short OEM PM/DM inputs
are expanded and fully verified, as required by the original startup layout.

**Community comparison, 2026-09-16:** [Sirherobrine23 source audit](XGSPON-SIRHEROBRINE-AUDIT.q1000k.md).
The inspected implementation is GPON/EPON and does not supply an AN7581 XGS
receive path. Its startup fixes support auditing local MAC/PHY dependencies;
our cold-start path already initializes and releases the MAC before sync.
Its periodic no-LOS/no-ready recovery adds a bounded repeated-acquisition
hypothesis beyond our one-attempt cases. Its generic calibration/firmware
loader rejects the unit's unchanged OEM inputs. These are source findings,
not new bench results. The requested [58-case replacement and repeated recovery
plan](XGSPON-REPEAT-RX.q1000k.md) adds the bounded experiment to the same bench.

**Earlier consolidated plan (all cases retained):** [deep receiver/OEM audit and 57-case collection plan](XGSPON-DEEP-RX-AUDIT.q1000k.md).
This adds 21 cases to the previous acquisition image, including the missing
OEM controller post-init bit, gain/peaking selection before calibration,
fresh eye observations and the exact OEM twelve-bit reset. Shutdown remains
a separate issue; the diagnostic-only image need not be booted first.

**Previous bench:** [receiver acquisition experiments and collection plan](XGSPON-RX-ACQUISITION.q1000k.md)

**Previous hardware evidence:** [2026-09-15 acquisition results](XGSPON-RX-RESULTS-20260915.q1000k.md).
All 34 connected cases and the fresh dark-checker control completed with valid
captures. None recovered frames. The separate passive reconnect control is
incomplete because its physical phases were not confirmed within its window.
The fresh checker remained idle in darkness and responded after reconnection;
its clock-tracking readback did not return to the original illuminated range.
These observations retain clock reacquisition, signal quality and unexplained
OEM initialization as open questions. They do not establish CDR lock or data
validity. DSD record transfer and the tested output/clock changes were verified.

The subsequent loader-fix image booted and loaded without the temporary alias.
Its 30-sample power check still saw light and no frames, but PHY shutdown failed
after RX draining. The collector stopped before the pending passive reconnect.
The exact failing shutdown operation was not logged. Diagnostic replacement
`ad6bc93b51` is built and software-validated; hardware boot/testing is pending.
Its diagnostics are now incorporated in the next consolidated image. A guard
or cleanup failure still stops collection, preserving the actual error. See the follow-up in the results above
and the build record in [the bench journal](XGSPON-BENCH.q1000k.md).

The prior passive plan is retained in [RX-NEXT](XGSPON-RX-NEXT.q1000k.md), with
its insertion-handler claim corrected. The sections below preserve the earlier
hypotheses and historical bench coverage; use the latest results above for
what has now been tested and what remains unresolved.

Prepared 2026-09-15. This is a diagnostic plan, not a claim of optical service.
The 90-sample PLL trial kept both LOS indications clear, but HUNT stayed zero,
frames/LOF/FEC/IRQs stayed zero, and one recovery did not change that. In the
preceding baseline, RX control was 0x00030202 (RX enabled, bit swap enabled),
PCS reset 3 (released), PMA reset 0x7f, and PLL PCW words 0x0fecdd0c. Forced
lock controls are not independent CDR lock measurements.

A receive-only bench cannot register or pass subscriber traffic. Its deliberate
MAC interrupt mask and O1 state are not evidence that no downstream optical
signal exists. The PHY frame/coding counters, rather than WAN packets, are the
useful observations at this stage.

| # | Hypothesis | Test prepared in this image | Discriminating result / limit |
|---|---|---|---|
| 1 | Light is present but too weak, unstable or excessive for useful reception. | Read MCU RX power in every sample, report dBm/nW alongside both LOS sources. Compare against the gateway on the same fiber connection. | Power near the gateway's inferred -17 dBm makes gross optical loss less likely; it does not prove modulation quality or calibration. Zero/0xffff remain unavailable. |
| 2 | LOS polarity/forced signal detect or stale MCU state gives a false light indication. | Capture raw SFP status/polarity and PMA signal control. Run 30-sample disconnected and reconnected controls with explicit user confirmation of each physical state. | Both LOS sources should follow removal/reconnection. An unchanged LOS or power reading keeps this hypothesis open. Do not infer a working datapath merely because LOS agrees. |
| 3 | RX gain or equalization differs from the Q1000K requirement. | Capture actual gain, equalizer and frontend power controls. Compare baseline with one opt-in OEM gain trial; save and restore the affected fields. | New codewords, framing errors, synchronization or frames after the change localize the fault. No improvement rules out only this isolated change, not every analog setting. |
| 4 | CDR/PLL, RX rate/divider or reset sequencing has not made the receiver operational. | Capture RX rate/OSR, clock divider, CDR ratio, readiness, reset and PLL words. Existing plain and PLL-assisted recovery remain independently selectable; gain+PLL is available in the same image. | Compare configured fields with reference XGS-PON initialization. The isolated PLL trial already failed; no undocumented full-SCU reset is added. Raw frequency words are not converted to an invented MHz value. |
| 5 | RX input/controller route, bus packing or differential polarity is wrong. | Capture RX input mux, bus width, SerDes control, frontend power, bit-swap and controller selection/status. Audit against the OEM and imported reference. | A configuration mismatch gives a targeted next change. Bit-order swap, LOS inversion and differential polarity are distinct; no undocumented polarity bit or input-mux write is tested blindly. Board tracing/scope comparison may still be needed. |
| 6 | PCS framing/descrambling is misconfigured, RX is disabled, reset held, or counters held clear. | Capture PCS debug control plus the existing RX-enable/reset words; decode the documented enable/reset/clear conditions offline. Add PSync mismatch and SFC/PON-ID HEC counters. | Error activity without valid frames points toward framing/rate/quality. Enabled RX and released PCS reset already weaken the simplest gate/reset explanation. No counter clear/latch/probe writes are made. |
| 7 | The selected frame counter misses earlier reception, or delivery stops at a later boundary. | Read codeword start/end and SOF/EOF-to-MAC counters alongside frame-to-PHY and FEC. Report first/last values and modulo-32-bit deltas. | Activity in earlier stages distinguishes partial decoding from an entirely quiet PCS. Frame-to-PHY progressing but no SOF-to-MAC moves attention toward the boundary. MAC/WAN traffic is deliberately constrained, so it is not an optical acceptance criterion. |
| 8 | The external downstream is a different PON technology or wavelength. | Check the gateway optical mode and exact module part; compare observed configured RX rate. | The user's 3FE46901AC was identified as XGS-PON by the Nokia guide recorded in XGSPON-ATT.q1000k.md, so this is lower priority. Average power and LOS cannot identify wavelength/modulation. Do not switch to GPON or power the other controller as an unreviewed experiment. |
| 9 | Firmware/calibration or the optical/electrical frontend is unhealthy despite readable registers. | Retain verified firmware/calibration loading and before/after MCU/TX health checks; correlate seven controller words with new power/path measurements across cold starts and physical controls. | Readback and MCU enable prove less than analog health. Unchanging or contradictory measurements justify further board/OEM comparison. `ddmi_rx`/`ddmi_rx_done` write calibration and are excluded from sensor testing. |

These hypotheses overlap. Several tests narrow a fault domain rather than
conclusively proving a cause. Changing ONT identity, OMCI profiles, DHCP or VLANs
is not the next diagnostic step for the current pre-registration PHY result.
No active loopback, PRBS transmitter, optical TX, broad reset sweep, flash or
bootloader modification is included.

## One image, shared observations

Vendor r78/helper r12 use receiver schema 5: 43 control/status words and seven
additional PCS counters, together with MCU RX power, LOS, the existing frame/FEC
counters, controller health, OMCI state and fiber LED observations. All fourteen
new control words and seven counters are ordinary addresses from the reference
AN7581 PHY register table. FIFO ports, debug-probe selectors and clear registers
are excluded. Configuration reads reject all-ones; full-width counters may wrap.

The image preserves 192.168.255.1 management, RAM root, NAND disabled, immutable
TX inhibit and normal cleanup. No device was accessed to prepare this extension.
The earlier build was gracefully cancelled to add this coverage before delivery;
its wrapper verified that normal configs and protected branch refs were restored.

After the user RAM-boots the final image through second-stage http-uboot:

1. Verify the image revision/runtime hashes and idle state.
2. With fiber confirmed connected, run `bench-matrix.py --restore-gain`: 30
   baseline samples and 90 continuation samples. Only persistent light without
   sync selects recovery; an already stable baseline selects observation only.
3. Read each stage's `observations.json`, `receiver-report.json` and
   `hypotheses.json`. The latter is generated by `bench-hypotheses.py` entirely
   offline. It records evidence and unresolved tests, not automatic fault claims.
4. If light reporting is contradictory, ask the user to disconnect/reconnect
   and run explicit 30-sample controls on this same image. Never treat elapsed
   time as confirmation of a changed fiber state.
5. If justified by step 3, select plain recovery, PLL recovery, or gain+PLL on
   the same image. Only one recovery per module lifetime; no automatic sweep or
   retry after a partial/error capture. Require clean unload and verified gain
   restoration before another stage. Do not repeat long unchanged windows.

The RX sampler and reports are source-tested for missing fields, failed reads,
full-width counter values, malformed power, mode mismatches and cleanup failure.
Kernel concurrency tests cover callback serialization and stop/quiesce during
all four recovery combinations. Hardware outcomes remain pending the new boot.

The delivered image is the validated `bench-7e9c0edc1d` artifact in
`build-artifacts/q1000k-xgspon/` in the parent workspace. Build, source-matched
UML and exact-image verification evidence is recorded in
`XGSPON-BENCH.q1000k.md`. The 30+90 sample run is the first comparison, not a
claim that every hypothesis can be conclusively resolved in two windows.
In particular, the clock/control audit also checks whether TX-disable handling
leaves a shared RX clock or frontend gate disabled; TX inhibit stays asserted
throughout this investigation. Proving electrical polarity or modulation
quality may require board/scope evidence beyond this RAM bench.

## Results from the first consolidated bench session — 2026-09-15

All five runs used image `7e9c0edc1d` on the same RAM boot, with 270 samples
total and normal verified cleanup between runs. The connected gain matrix,
disconnected control, and reconnected gain+PLL matrix are recorded in
`XGSPON-BENCH.q1000k.md`. No additional image or unsupported register write was
needed. The final connected sensor reading was 14,300 nW (-18.45 dBm).

| Hypothesis | Result and remaining limit |
|---|---|
| 1. Optical power | Connected measurements were available throughout (approximately -18.9 to -18.3 dBm across starts), with small within-window variation. Severe disappearance/fluctuation was not observed; absolute calibration, wavelength and modulation quality remain unproven. |
| 2. False/stale light | Both LOS sources followed the confirmed physical state. RX power fell to one count/100 nW disconnected and recovered to about 14,400 nW after reconnection. SFP status changed 0 -> 1 -> 0 with unchanged polarity. A stuck indication is strongly disfavored; live insertion timing/IRQs were not tested. |
| 3. RX gain/equalization | The isolated OEM gain setting and gain+PLL both applied successfully and restored their saved gain fields on shutdown, but neither produced synchronization or any receive-counter activity. This rules out those candidates as sufficient fixes, not every analog setting. |
| 4. Clock/rate/reset | Sampled rate/divider/bus/OSR fields match the imported 9/10G setup. The prior PLL-only trial and this combined trial failed. Actual recovered clock and reset/PLL ordering remain open; forced lock fields do not settle them. |
| 5. RX path/packing/polarity | Input, SerDes, bus and frontend controls were captured and remained stable. No physical route or differential-polarity proof was obtained. Keep this open for OEM/board comparison. |
| 6. PCS configuration | RX enabled, PCS reset released, counter-clear inactive and descrambling enabled throughout. These simple gate/reset explanations are weakened. Correct bit packing and all framing parameters are not yet proven. |
| 7. Counter blind spot | All seven extra codeword/HEC/MAC-boundary counters also stayed zero. They did not uncover earlier reception hidden by the original frame counter; missing data/clock or incorrect counter configuration remain possible. |
| 8. External technology | No new wavelength/rate measurement was made. The previously identified XGS-PON module keeps this lower priority, not eliminated. |
| 9. Firmware/calibration/hardware | Verified loading and controller health checks passed; power and LOS changed with the fiber state. Analog output quality and end-to-end electrical receive operation remain unverified. |

Next work should audit the controller-to-PHY receive path and the complete OEM
clock/reset sequence. Known differences worth resolving before another build
are reset release before L2D/TDC, PLL restoration before RX-ready,
TDC settling/toggle order, and the OEM's upper five digital-reset fields whose
meanings remain unknown. Do not replay unknown fields or remove TX inhibit.
The public FEC_FORCE_OFF macro name is misleading: its enable handler sets
that bit, so 0x310 must not be labeled disabled FEC based on the name alone.
There is no evidence yet that http-uboot is responsible, and no basis to change
ONT identity, OMCI profiles, DHCP or VLANs to fix this pre-sync PHY state.


## Consolidated next-image coverage

See [the receive suite](XGSPON-RX-SUITE.q1000k.md) for all 13 cases, ten bounded
new probes, shared raw diagnostics and physical controls. The suite's coverage
catalog maps every hypothesis above to on-image observations/experiments and
identifies the external evidence still needed. All cases have now been run on
image `4ef30eb4d5`; results are recorded below. No additional http-uboot change
is indicated.

## Full suite and live physical-control results — 2026-09-15

All 13 cases completed on one RAM boot (1,050 samples), followed by a
180-sample connected-to-dark checker control and a separate 30-sample
reconnected baseline. All attempted probes restored their fields and every
run unloaded normally. TX remained inhibited. Complete evidence paths and the
per-case table are in `XGSPON-BENCH.q1000k.md`.

| Hypothesis | New evidence | What remains open |
|---|---|---|
| 1. Optical power | The connected suite stayed between -18.70 and -18.15 dBm. Dark readings fell to 100 nW; the last reconnected reading was -18.51 dBm. Gross disappearance was absent during the suite. | Absolute calibration, wavelength and modulation quality. The user's earlier gateway -17.0 dBm reading was not simultaneous. |
| 2. False/stale light | A live disconnect changed both LOS sources, SFP status, power and the PHY IRQ count (0 -> 1). Reconnected startup recovered the light readings. | A stuck light indication or completely dead PHY interrupt path is contradicted by this control. Live reconnection IRQ/latency was not captured. |
| 3. Gain/equalization | Automatic gain and legal forced-low gain both applied and restored without synchronization, codewords or framing errors. Earlier OEM gain=1 and gain+PLL also failed. | These settings are not sufficient fixes. Unknown equalizer/analog/calibration dependencies remain. |
| 4. Clock/rate/reset | Public recovery, extended TDC delay, PLL-before-final-release and restricted OEM ordering all failed. The frequency-monitor words changed during live darkness and returned after reconnected startup. | Actual recovered clock/data and the OEM's five unknown reset fields. A frequency upper count inside the configured window is not a lock test; a prior dark start also produced that count. |
| 5. Path/packing/polarity | RX bit-order toggle did not improve any PCS counter. RX-only checker logic entered comparison/completion, with upstream generator and loopback verified off. | Electrical route/differential polarity and live high-speed data. The checker completed result stayed latched through darkness, so it does not establish input data continuity. |
| 6. PCS framing | Descrambler toggle and both alternate public FEC encodings had no effect on sync or any of seven codeword/framing/boundary counters. | Those isolated changes are insufficient; combinations or an earlier receive-path failure remain possible. |
| 7. Counter blind spot | All seven PCS counters remained zero in every case. The PMA checker produced nonzero status/errors, but its result stayed unchanged with no light. | The checker is operational but is not a continuous data-activity meter. Neither its result nor quiet PCS counters locates the electrical failure conclusively. |
| 8. External technology | No new wavelength or modulation measurement. | The previously identified XGS-PON module keeps a wrong line type lower priority; LOS/power cannot eliminate it. |
| 9. Firmware/calibration/hardware | Verified controller inputs and health checks passed throughout; optical sensors respond to removal and reconnection. | Analog output quality, board routing, calibration accuracy and end-to-end receive operation. |

No receive synchronization, frame, FEC or PCS error activity was observed in
any of the 1,260 samples. The receive-only operating restriction still excludes
registration and subscriber service; it does not explain away absent downstream
PHY synchronization. Further work should obtain controller-output/SoC-input
and clock evidence, or identify a documented OEM configuration difference,
before building another register experiment. None of the results justifies
unknown reset writes, TX activation or a bootloader modification.
