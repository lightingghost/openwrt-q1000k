# Q1000K: one-image receiver acquisition bench

Date: 2026-09-15. Branch: `q1000k-xgspon`. Hardware results are pending.

## Findings that change the test plan

The stock Q1000K PHY releases the direct CDR reset override in
`0x1fa8b818[24]`; our previous `oem-order` trial retained the public driver's
forced reset selector. Stock L2R/L2D also differ in order and settling time.
The previous trial therefore did **not** test the stock CDR acquisition state.

The initial receiver path already performs full receiver power-up and five
calibrations. Those calibration writes mostly match the stock binary. Blanket
clearing of force bits is unjustified: many are deliberate power/ready settings.
The useful differences are specific clock selectors, post-calibration exits,
eye-scan peaking encoding, and a PrCal call that stock performs unconditionally
but public code gates on chip ID.

The EN7572 family source documents electrical receive-output swing/emphasis
controls feeding the SoC. Three fixed source-table profiles now test this path.
These settings are not optical transmit drive and are not established Q1000K
defaults. The stock controller loader also uses A0 for MD32 control/address
words, unlike the native loader's A2 path; a separate initialization variant
tests that difference with identical verified firmware and calibration bytes.

Source detail and offsets:

- [OEM clock/calibration audit](XGSPON-OEM-CLOCK-AUDIT.q1000k.md)
- [OEM controller/electrical audit](XGSPON-OEM-CONTROLLER-AUDIT.q1000k.md)
- [PR 24577 applicability](XGSPON-PR24577.q1000k.md)
- Pinned electrical-output source:
  [EN7572 command implementation](https://github.com/Sirherobrine23/airoha_xpon_en757x/blob/950199a8de6b75e76906a7c1b39b7a9a3e2913f9/v2/lddla/en7572_cmd.c).

## Experiments in the same image

Each connected case initializes the controller and PHY independently, records
the baseline, applies at most one immutable PHY experiment after ten consecutive
light/no-sync polls, then records the result. Controller variants apply during
initialization before PHY calibration. Every case ends with checked teardown,
controller power-off, and removal of private inputs before the next case.

| Case | Changes and question |
| --- | --- |
| `cdr-auto-release` | Exact stock L2R/L2D direct CDR sequence; does releasing the reset override allow acquisition? |
| `cdr-internal-auto` | Return the documented internal CDR reset/lock modes to automatic control and release direct overrides. |
| `prcal-finalize` | Replay the source-defined injection/LPF/IDAC release and learned-IDAC load tail. |
| `fll-auto` | Release the named FLL reset force-enable selector. This is an automatic-control hypothesis, not a stock preset. |
| `rx-sequence-auto` | Hand four documented ready/calibration gates to the normal receiver sequencer. Normal mode sets these bits to one. |
| `post-eye-ready` | Restore the PI-calibration-ready tail after the startup eye scan left it forced low. |
| `oem-clock-cycle` | Stock TDC/L2R/L2D/reset/PLL/ready ordering and waits using only documented reset bits0–6. |
| `oem-peaking` | Translate the selected public peaking code from bits19–17 into stock's bits19–16 encoding. |
| `oem-rx-acquire` | Peaking translation, stock gain1, PrCal finalization, PI-ready tail, and stock known-field clock cycle. |
| `combined-auto` | Add documented internal CDR/FLL/sequencer automatic modes to the preceding combined recipe. |
| `prcal-rerun` | One bounded source calibration search, with per-step measurements, finalization and stock clock cycle; no retry. |
| `oem-md32` | Load/start/verify MD32 through the stock A0 control/address bank. |
| `rx-output-400-flat`, `rx-output-600-flat`, `rx-output-600-boost` | Three documented electrical output profiles: 400mV/0dB, 600mV/0dB, 600mV/2dB. |
| `oem-acquire-*` | Pair each electrical output profile with `oem-rx-acquire`. |
| `oem-md32-acquire-*` | Add stock A0 controller loading to each preceding combination. |

The ten original immutable probes, public reacquisition, and two baselines
remain in the same connected suite for comparison. `checker-dark` is a
separate physical control: initialize with light, confirm disconnection, arm a
fresh receiver checker after ten consecutive dark polls, then confirm
reconnection. The default collector also runs a passive light/dark/reconnect
control. Neither control invokes vendor LOS power-save/insertion handlers;
the old passive plan's claim otherwise was incorrect.

## How to run

Boot the generated RAM-only FIT by the established bench boot procedure.
Management is `192.168.255.1`; use a directly attached host such as
`192.168.255.2/24`. Optical TX inhibition is immutable, NAND is disabled, MAC
registration/interrupts remain disabled, and no optical service is activated.

Run the generated single-file collector from the host:

```sh
python3 q1000k-rx-collect.py --inputs /path/to/q1000k-inputs.tar \
  --output /path/to/new-results --serial-device /dev/ttyUSB0
```

The private input archive remains separate from the image and result archive.
Use `--serial-log /path/to/live-serial.log` instead when another process already
records the console. The collector opens its own serial device read-only.

Default: all connected experiments, then prompted fresh-dark-checker and passive
reconnect controls. Keep the fiber connected throughout the connected suite.
Allow roughly 60–90 minutes for the complete run; the exact plan is printed by
`--dry-run`. Physical steps require explicit typed confirmation and observed
LOS transitions; elapsed time never counts as confirmation.

For unattended connected experiments only, use
`--fiber-connected --skip-live-control`. For one explicitly selected repeat,
use `--case CASE --fiber-connected`. The default never retries a failed case.

## Evidence and decisions

- Actual PHY synchronization and advancing frame/PCS/FEC counters establish
  progress. An initialized controller, completed checker, in-window frequency
  word, or successful setting readback alone does not.
- Diagnostic schema3 adds eleven calibration-release/eye-state/peaking words
  to the preceding34. Controller schema2 adds electrical settings/originals,
  selected initialization profile, APD voltage, RSSI, OCP, temperature and supply.
- Preserve raw serial and per-case status, paired diagnostics, before/after
  reports, private-input cleanup and exact runtime hashes. The single result
  archive includes partial evidence and an explicit not-run list after a stop.
- A case with stable sync before its trigger records that the intervention did
  not run. It must not receive credit for that existing synchronization.
- Successful isolated CDR cases identify a clock/override issue; electrical
  profiles distinguish receiver-output sensitivity; only combined success
  identifies an interaction and needs narrower follow-up.

Unknown stock reset bits11–7 and controller `0x110[8]` remain unwritten because
their receive-only semantics are unproven. Software cannot establish actual
optical wavelength/modulation, differential wiring, or an electrical eye.
Those remain physical follow-ups if all supported acquisition cases stay quiet.

## Validation boundary

Register restoration restores saved fields; it does not undo every internal
effect of reset/calibration pulses. Independent initialization and controller
power-off separate cases.

Production-code fault injection checks access/readback errors, restoration,
TX/generator/loopback guards and single-attempt lifetime rules. Kernel UML checks
callback/stop concurrency. Image inspection checks the exact packaged modules,
RAM/NAND/TX settings and helper. These validate the implementation and artifact;
the Q1000K optical result still requires the collected bench run.
