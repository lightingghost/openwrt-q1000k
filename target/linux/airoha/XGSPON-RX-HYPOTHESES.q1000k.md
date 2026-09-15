# Q1000K light-present/no-frames hypotheses and bench tests

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
