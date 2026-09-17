# Q1000K TX output bench: board gate and correlated sensors

## Leading hypothesis and new source finding

Treat failure to produce the expected optical output as a leading hypothesis.
The previous bench cleared EN7573 `0x3e0[9]`, but all recorded BEN-status
samples were zero. The factory MCU then forces published TX power to code 1.
That floor is not an independent optical measurement.

The NAND firmware exposes a second, board-level TX disable: **GPIO38**.
Our earlier controller driver did not own or explicitly release this GPIO.
Whether it was physically inhibiting transmission is a hardware hypothesis,
not yet a proven explanation. This bench directly tests it.

### Exact NAND source

The read-only `scripts/q1000k/oem-nand-audit.py` extraction verified the
512 MiB `q1000k-nand-backup.bin`, SHA-256
`41f08f7e71c5c08835fd1f923a925e10bf56add1239b1e35ad35df6da8e76c50`,
and its primary FIT at `0x602100`. Local evidence is preserved under
`build-artifacts/q1000k-xgspon/tx-output-nand-20260917` in the workspace.
No OEM code was executed and no device/NAND write was performed.

- NAND DT `leds/phy_tx_power_disable`: second GPIO bank, offset 6,
  active high, default state on. This maps to GPIO38.
- NAND `/userfs/led.conf`, `7581gtled.conf`, `7581ptled.conf`:
  logical `LED_PHY_TX_POWER_DISABLE` 42 maps to GPIO38, on/off mode.
- NAND `phy_10g.ko`, `phy_tx_ctl()` at text `0xd8d0`: enable=1 calls
  `ledTurnOff(42)`; disable=0 calls `ledTurnOn(42)`.
- NAND `tcledctrl.ko`, `ledTurnOff()` at `0xd60`: resolves the logical
  entry to its GPIO and dispatches value 0 through the LED notifier.
  The GPIO LED is active high, so this releases TX_DISABLE.
- `gpio_BOSA_Tx_power_on()` calls logical LED102. All three NAND LED
  tables mark entry102 unused; it is not evidence of another required rail.
- NAND `en7572.ko`, `ddmi_tx()` at `0x1ff0`: separately toggles internal
  `0x3e0[9]`. It also overwrites TX calibration using a supplied optical
  reference. The bench does not run that calibration routine.
- NAND `AdaptivePon()` at `0x58a0`: forces BEN off, restores a calibrated
  eye, restarts the loop and returns BEN to normal. The existing OEM eye
  option is retained. No arbitrary bias/modulation code is introduced.
- NAND `mpd_current()` at `0x4aa4`: selects the monitor mux/gain, waits
  50+5 ms, reads `0x33c`, computes `uint16_t(256-(raw>>7))`, and looks up
  monitor current. Its 156-entry `.rodata+0xf8` table exactly matches the
  previously reviewed downloadable OEM build. Key256 (including raw zero)
  is unavailable, not zero current.

NAND module hashes differ from the downloadable OEM reference:

| Module | NAND SHA-256 |
|---|---|
| en7572.ko | `db0065680707791fdd2275e98f7c8236626a6223795f3f647806711836832d67` |
| phy_10g.ko | `f06f41a1bfcf25b3e9e072045113e86569c91704a863c8da5fbc2ba679849c28` |
| tcledctrl.ko | `5503abb220f4f5dcad78b59160f90f265360f35939ded2ac170754396d2fe155` |

The NAND MCU program/data and DSD calibration match the existing bench inputs.
The MCU TX routine at PM `0x858` samples `0x3a4`, shifts seven bits, publishes
LE16 TSSI at mailbox `0xf0`, and uses BEN `0x488[0]` to choose reporting state.
Mailbox `0xfe`: 1 normal-calibration path, 2 equal/invalid calibration
endpoints, 3 BEN-off floor. The routine's filtering and update timing mean
a fresh mailbox read alone cannot establish a fresh sensor conversion.

### Sirherobrine comparison

The fetched current branch is still commit
`2e2cf91fe84467d77649efebd99a28284f2124b3`. Sources and hashes are preserved in
`build-artifacts/q1000k-xgspon/tx-output-research-20260917/sources.json`.

- [Optical frontend core](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/core.c):
  owns optional `tx-disable-gpios`, initially asserted. `optical_frontend_tx_enable()`
  asserts the external interlock before provider shutdown, and releases it
  after provider enable. It can report the GPIO state separately.
- [PON MAC](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c):
  calls the optical frontend TX API; optional rearm returning EOPNOTSUPP is
  accepted. SFP-managed paths also use the SFP lifecycle. This source is a
  GPON/EPON implementation; it does not establish working Q1000K XGS-PON.
- [EN7572 initialization](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_main.c):
  loads MCU firmware/calibration and releases the MCU. Its operations do not
  implement a separate EN7572 `tx_rearm` callback.
- [EN7572 eye loader](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_loop.c):
  forces BEN off, restores calibrated current/feedback settings, copies the
  selected eye's four-byte TSSI calibration point, restarts the loop and
  returns BEN to normal. This is the Sirherobrine eye comparison.
- [PR24577](https://github.com/openwrt/openwrt/pull/24577) remains useful for
  separating PHY/BEN/burst controls from optical drive and registration.
  Its EN7571/GPON register values are not interchangeable with EN7573/AN7581.

## One image, seventeen new disconnected-fiber cases

`--suite output` is the portable collector's default. IDs32–48 supplement
the previous isolated/measurement/TX suites in the same image.

Every case uses a fresh controller/PHY lifetime, calibrated limits, checked
LOS, no MAC/OMCI producer, and a synchronous five-second target window.
Optical fiber stays disconnected for the complete suite. No identity is needed.

| ID / collector case | Hypothesis / comparison | Internal TX gate during window | Board GPIO38 during window | Monitor |
|---|---|---|---|---|
| 32 internal-only-passive | Internal enable alone is insufficient | Released | Asserted | Passive |
| 33 both-gates-passive | Missing OEM board gate blocks output | Released | Released | Passive |
| 34 board-only-passive | Internal disable still inhibits after board release | Asserted | Released | Passive |
| 35 both-gates-fixed-monitor | Main fixed-setting off/on/off correlation | Released | Released | Held selected |
| 36 internal-only-fixed-monitor | Board gate negative control at identical monitor gain | Released | Asserted | Held selected |
| 37 board-only-fixed-monitor | Internal gate negative control at identical monitor gain | Asserted | Released | Held selected |
| 38 oem-eye0-fixed-monitor | Factory calibrated drive/feedback initialization missing | Released | Released | Held selected |
| 39 sir-eye0-fixed-monitor | Eye/TSSI calibration reload changes acquisition/reporting | Released | Released | Held selected |
| 40 no-producer-fixed-monitor | Enable alone versus an actual continuous pattern | Released | Released | Held selected |
| 41 ben-inverted-fixed-monitor | SoC burst-enable polarity mismatch | Released | Released | Held selected |
| 42 existing-clock-fixed-monitor | No-downstream TX clock preparation dependency | Released | Released | Held selected |
| 43 ben-off-fixed-monitor | BEN forced-off negative control after both disables released | Released | Released | Held selected |
| 44 loop-restart-fixed-monitor | Control loop needs a restart | Released | Released | Held selected |
| 45 all-one-fixed-monitor | Data modulation/pattern dependence | Released | Released | Held selected |
| 46 all-zero-fixed-monitor | Complementary data-pattern control | Released | Released | Held selected |
| 47 both-gates-repeat | Reproducibility after a fresh controller lifetime | Released | Released | Held selected |
| 48 both-disabled-fixed-monitor | Monitor background with both disables asserted | Asserted | Asserted | Held selected |

PRBS7 is the common producer, except no-producer and fixed-pattern cases.
Existing-clock omits the native unplugged-fiber clock preparation.
The previous bench did not establish a source-backed EN7573 force-on BEN
encoding; this image does not guess one.

## Correlation and interpretation

Each case saves five snapshots in each of three phases: both gates off,
selected gate combination on, both gates off again. On-phase targets are
0/100/500/1500/4500 ms; off phases span one second. Actual begin/end timestamps
include I2C latency. In-flight bus transactions and gate-disable latency can
extend the five-second target; collection checks the observed duration.

The generator, analog eye and monitor settings are established before the
first off sample and restored after the last off sample. Fixed-monitor cases
perform the OEM monitor selection once, hold it across all three phases,
and restore the saved masked fields afterward. Passive cases never select it.

Each snapshot reads BEN, hardware TSSI, mailbox TSSI, `0xfe`, hardware TSSI
again and BEN again. It also records raw monitor ADC, published power/current,
actual drive codes, internal TX disable, GPIO38 readback, MCU idle/report flags,
analog controls, current limits, OCP control/status and live TX calibration.
The bracketed reads expose asynchronous updates; they are not atomic samples.
PHY generator, clock and SFP status are retained by the isolated owner.

The collector decodes hardware TSSI as `(CSR3a4 >> 7) & 0xffff` and uses the
NAND monitor table only when the held monitor selection is verified. It
reports raw data, decoded ranges, mailbox/hardware comparisons, FE states,
gate truth-table checks, changed analog fields, and missing observations.
It never converts invalid ADC zero to zero current or claims connector power.

Likely interpretations:

- GPIO release produces BEN and TSSI/current response: external interlock is
  implicated; evaluate whether the response approaches factory calibration.
- BEN stays zero despite both enables: investigate gate routing/polarity,
  electrical producer/clock path or a board-level condition.
- BEN asserts, hardware TSSI changes, mailbox stays flat: MCU publication,
  idle state, filtering or timing becomes the leading measurement problem.
- Hardware/mailbox TSSI agree, FE=3: reporting is deliberately BEN-suppressed.
- BEN asserts but calibrated drive codes remain zero: investigate analog loop
  acquisition, fault/limit controls and calibration mode.
- Only active-monitor cases respond: measurement selection or loop interaction
  is a confounder. Use the passive pair and changed-settings report.
- Healthy gate/drive activity without useful feedback: monitor configuration,
  optical hardware or the emitter remains possible. Internal telemetry alone
  does not prove wavelength, connector output, burst quality or OLT acceptance.

Both gates are disabled before cleanup; partial errors remove controller power.
The new pinctrl group reserves GPIO38 separately while retaining the other
five PON pins. Only the activation RAM DT opts into this change. NAND remains
disabled. Host failure injection and image inspection validate implementation;
live optical behavior remains for the next bench collection.
