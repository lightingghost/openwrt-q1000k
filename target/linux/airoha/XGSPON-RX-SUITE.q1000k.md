# Consolidated Q1000K receive bench

This image supports the remaining software experiments together. One user RAM
boot, followed by normally unloaded/reloaded cases, is sufficient. It does not
claim that software can conclusively test wavelength, absolute power calibration
or physical differential wiring. Those hypotheses have explicit external controls
below and stay open until evidence is supplied.

## Cases on one image

Run `scripts/q1000k/bench-suite.py` with the verified artifact, private input tar,
new capture directory and `--fiber-connected`. `--dry-run` prints the complete
coverage catalog without accessing the device. The default is 30 samples for
baseline cases and 90 for each recovery/probe case (1,050 samples total, plus
startup/cleanup time). Every case collects optical power, both LOS sources,
receiver/analog controls, frequency-monitor controls/results, seven additional
PCS counters and independent PMA receiver-checker status.

| Case | Change / question |
|---|---|
| baseline | Current cold initialization with no recovery or probe. |
| public-recovery | Existing single public out/in sequence, control for recovery itself. |
| bit-order | Toggle the documented RX data bit-order field; no TX bit-order change. |
| descrambler | Toggle only RX descrambling; compare PSync/HEC and frame activity. |
| fec-oc | Apply the public RX FEC OC-reference encoding. |
| fec-off | Apply the public RX FEC disable encoding. The baseline uses its enabled encoding. |
| gain-auto | Remove forced frontend gain and observe the hardware result. |
| gain-low | Force the legal gain value zero; values one and startup two/three were already observed in previous trials. |
| tdc-delay | Public recovery with the TDC settling interval extended from 0.5 to 5 ms. |
| pll-order | Restore documented PLL clocks before final digital-reset release / RX-ready. |
| oem-order | Release the seven documented reset bits before L2D/TDC, use OEM 5-ms settling and omit the public extra CDR retoggle; restore PLL before RX-ready. |
| checker | Enable only the RX PRBS checker, with generator and loopback bits verified off. |
| baseline-repeat | Fresh initialization after all trials; detect residual/intermittent differences. |

The OEM reconnect disassembly actually calls digital reset, L2D, TDC-on,
TXPLL-on, RX-ready in that order. It does **not** establish “PLL before final
PCS release.” `pll-order` is an independent timing candidate; `oem-order` is
an approximation restricted to known fields. The OEM's five additional reset
bits are not understood and are never replayed. This corrects the earlier
hypothesis note's ordering description.

The public `FEC_FORCE_OFF` macro name is misleading: the enable handler sets
bit 4. Use handler behavior, not that name, to interpret the modes. RX data bit
order is also not an electrical differential-polarity control. No supported
polarity bit or board test point was established by this source audit.

## Execution and evidence

Only a connected, TX-inhibited RX bench can select a probe. Mode selection is
immutable once configured. A probe consumes the existing single attempt after
ten consecutive light-present/no-sync polling observations. Already synchronized
receivers do not trigger; their case is recorded as not triggered, not a passed
intervention. Incomplete/no-light cases and safety failures stop the suite.

Every changed field is saved before the first write, checked after each write,
and restored in reverse order after callbacks have drained and TX is off. The
restore marker, full diagnostic count, matching mode, sample times, write count,
normal module teardown and private-input removal are required evidence. A
missing downstream-sync result alone permits the next case; kernel faults,
read/write errors and cleanup failures never do. No forced unload or automatic
retry is used. The original public recovery remains a separate control.

New `rx_bench_diagnostics` is a bounded, separately versioned read-only snapshot
so the original RX status does not exceed a sysfs page. `bench-probe-report.py`
keeps all raw values, before/after sets and provenance. The RX monitor's upper
16 bits are compared with its configured window; this is not a measured MHz
conversion or an independent CDR-lock assertion. Normal XGS-PON is not PRBS:
checker activity can expose a blind spot in PCS counters but cannot measure
optical BER. Error-count changes are calculated separately before and after
the attempt; no delta crosses a recovery or checker restart. A quiet checker does not prove a broken receiver.

Reports go in the suite directory and individual case directories. `suite.json`
retains completed, untriggered, failed and not-run cases and lists external tests
as pending. `collection-complete` means collection finished, never O5, OMCI
interoperability or subscriber service. All modes retain optical TX inhibit,
unassigned O1, a zero MAC IRQ mask and no service activation.

## Physical controls using the same image

These require user participation or external equipment, not another image.
Keep the verified firmware/calibration inputs unchanged and never write flash,
EEPROM, factory data, boot environment or optical calibration commands.

1. **Optical level and line type:** record the working BGW320 optical mode,
   module part, RX reading/units and time. Compare its reading on the same fiber
   with a baseline Q1000K capture. A wavelength-selective, calibrated PON meter
   is needed to settle absolute level and wavelength if the discrepancy remains.
   LOS and mean power do not prove XGS-PON modulation. Preserve the user's prior
   gateway -17.0 dBm observation as a report, not a simultaneous measurement.
2. **Dark / reconnect:** after verified unload and controller-off, ask the user
   to disconnect; run a 30-sample `bench-run.py receive --fiber-disconnected`.
   After cleanup ask for firm reconnection, then run a connected baseline.
   Expect both LOS sources and RX power to change and recover. These transitions
   were already demonstrated by the previous image; repeat only to validate new
   diagnostics or investigate changed observations.
3. **Live LOS / IRQ response:** if needed, use one 180-sample connected baseline
   with no recovery/probe. Have the user mark disconnect/reconnect times within
   that capture and observe the LED. Compare raw LOS, polling, IRQ counts, power
   and LED state. Do not run a concurrent register experiment. The standard
   report retains mixed LOS states; the automatic all-connected suite rejects
   them. Lack of an IRQ with successful polling localizes notification behavior,
   not optical reception. Cleanup is still mandatory.
4. **Electrical receive route / clock / polarity:** identify the board's actual
   EN7573-to-AN7581 receiver connections from schematic or verified traces before
   attaching suitable high-speed measurement equipment. Establish differential
   activity, orientation and recovered-clock behavior. No test-point locations
   or polarity register are guessed here. Use a powered-down continuity check
   only where board access and the measurement setup make it appropriate.
5. **Analog health / calibration:** compare the same unit with known-good optics
   or independent measured receiver output. Firmware hashes, MCU-enable status
   and restored LOS/power only verify part of this path. A failed suite leaves
   equalizer/calibration detail and hardware quality open; it does not justify a
   calibration sweep or enabling the laser.

Keep external measurements with timestamps, equipment/method and raw readings
beside `suite.json`. Any unavailable physical control stays explicitly pending.
All independently read diagnostics share each case's observation window;
register experiments remain sequential because they operate on the same PHY.

## Completed execution on image 4ef30eb4d5

All 13 cases completed on 2026-09-15, on one RAM boot. The 1,050 samples never
showed sync, frames or PCS counter activity. Each selected probe attempted once
and restored its saved fields; all teardown and TX guards passed. See
`XGSPON-BENCH.q1000k.md` for the per-case results and capture paths, and
`XGSPON-RX-HYPOTHESES.q1000k.md` for the remaining limits.

The new checker result justified a static armed-checker physical control with
the user's participation. This used one completed setup followed by no further
probe writes during disconnection:

1. Start a connected 180-sample receive capture with
   `--reacquire-once --probe checker`. Read `bench-suite-progress.py CAPTURE` promptly until attempt 1,
   writes 2 and checker control 0x10005 are observed.
2. Signal disconnection and wait for the user's confirmation. Observe at least
   15–20 dark samples with both LOS sources asserted and unchanged write count.
3. Signal reconnection while sufficient samples remain and wait for the user's
   confirmation. Never infer a physical change from elapsed time. If the bounded
   window ends first, record that limitation and verify reconnection in a fresh
   baseline only after normal cleanup.
4. Use `bench-control-report.py CAPTURE --write`, not the all-connected suite's
   case validator. It preserves consecutive LOS/attempt phases and all normal
   safety/cleanup requirements.

The actual control captured 141 samples before sampled LOS assertion and 39
dark samples (43.515 seconds). LOS, RX power, SFP status, PHY IRQ count and
frequency-monitor words responded. Checker completion/errors stayed latched,
so they do not establish continuous reception. The timer expired before
reconnection was sampled; a separate 30-sample baseline verified restored light
readings and normal checker state after reinitialization. Live reconnection
IRQ/latency remains unmeasured. No further image build was required.
