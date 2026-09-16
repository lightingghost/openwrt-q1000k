# Q1000K: repeated clock acquisition in the consolidated bench

Prepared 2026-09-16 on `q1000k-xgspon`. The user requested the additional
hypothesis from the [Sirherobrine23 audit](XGSPON-SIRHEROBRINE-AUDIT.q1000k.md)
in the next bench. This adds **one case to the existing 57**, for **58 total**:
56 connected cases and two confirmed physical controls. Hardware results are
pending; software tests cannot establish recovered-clock lock or valid frames.

## Hypothesis and matched controls

**Hypothesis:** an illuminated receiver may need more than one clock/reset
acquisition attempt in the same initialized lifetime. The community GPON PHY
retries approximately every five seconds while syncing with LOS clear. That
implementation does not supply an AN7581 XGS-PON reset recipe. Our comparison
uses the exact Q1000K OEM sequence already selected by `oem-full-reset`.

| Collection case | Intervention | Hypothesis tested | Evidence and interpretation |
|---|---|---|---|
| `connected-baseline` / suite `baseline` | No recovery; 30 samples | Can the current initialization acquire without intervention? | Establishes power, LOS, sync, counters and clock readbacks before comparing recovery cases. |
| `oem-full-reset` | One OEM clock/reset acquisition after ten consecutive illuminated, unsynchronized polls | Is one exact OEM twelve-bit reset/clock sequence sufficient? | Same reset recipe as the new case; one-attempt control. |
| **`oem-reset-repeat`** | Up to six identical OEM acquisitions, at least 5,000 ms after each previous attempt finishes | Does repeated acquisition within one initialized lifetime change the outcome? | Per-attempt before/after counters and NCPO; timing, checked writes and terminal reason. First sync or frame activity only after attempts 2–6 supports further investigation of this hypothesis. |
| `oem-clock-cycle` | One existing seven-bit reset/clock sequence | Does the exact twelve-bit OEM reset matter? | Separates reset-mask differences from retry count. |
| `oem-post-init`, `oem-post-cal`, output combinations | Existing isolated OEM controller bit and combined analog cases | Is a board-specific initialization step missing? | Still included; see the [complete deep receiver mapping](XGSPON-DEEP-RX-AUDIT.q1000k.md). |
| `eye-current`, `oem-analog`, `oem-cal-reset`, `oem-cal-auto`, `oem-eye-0`…`7` | Existing fresh eye measurements and OEM analog calibration cases | Does analog initialization or gain/peaking prevent useful reception? | Fresh completion markers distinguish measurements from stale passive eye words. All earlier cases remain available. |
| `checker-dark`, `live-reconnect` | Confirmed connected/dark/reconnected phases in one running case | Does signal/clock/checker behavior follow physical insertion? | Operator confirmations are required; no timed assumption substitutes for changing the fiber. |
| Every connected case | Existing unit DSD/firmware loading, full PM/DM readback, RX power and controller health | Did the correct unit record reach the controller, and does its state remain coherent? | Tests record selection/transfer; not the physical accuracy of calibration coefficients. |

Separate cases unload and restore normally. **The six attempts inside
`oem-reset-repeat` do not unload, reload or reinitialize the controller.**
This preserves the distinction between repeated acquisition and repeated cold
starts. Six attempts are an experimental bound, not an OEM-prescribed count.
A negative outcome rules out this particular recipe/window, not every recovery
sequence or an analog fault. Repeat positive cases to establish reproducibility.

## Exact sequence and lifecycle

Mode 38 calls `probe_oem_clock_cycle_reset(true)`, exactly as mode 25:

1. OEM TDC off and RX L2R transition.
2. OEM twelve-bit reset (confirmed in both audited OEM binaries; the upper
   five individual bit meanings remain undocumented).
3. OEM RX L2D transition.
4. TDC-on prefix with OEM settling delay, excluding the public final forced
   LPF reset; no added FLL reset.
5. Existing TXPLL clock setup and OEM RX-ready release. The clock block runs
   while the independent optical-controller TX inhibit remains asserted.

First attempt requires ten consecutive eligible polls (nominally about
15 seconds). Further attempts wait at least 5 seconds after the previous
attempt completes; the 1.5-second polling cadence can make the observed gap
longer. The driver records actual times and the report verifies the bound.

The series stops for the module lifetime on:

- any sampled synchronization, including an IRQ or userspace snapshot;
- either LOS source asserting after the first attempt;
- an operation or guard error;
- normal shutdown after observation/recovery begins;
- consumption of the six-attempt budget.

A stop/start cannot clear the latch. A failed or partially applied operation
cannot be retried. The probe implementation independently enforces its six-call
budget and rejects mixing probe state. Each affected field is saved before its
first modification, checked around writes, and restored once during final
cleanup. The unchanged controller TX inhibit applies throughout.

## Collection and evidence

The single-file `q1000k-rx-collect.py` runs all 58 cases by default, with
5,280 seconds (88 minutes) of sampling plus initialization, cleanup and operator
time. `--case oem-reset-repeat` runs only the new case, including its own
pre-intervention observations, on the same firmware. The suite promotes this
case to at least 90 samples even with `--samples 30`; other cases retain their
selected durations. The low-level runner/helper rejects a shorter direct
request. `--samples 180` is available for a longer post-attempt observation.

The historical `--reacquire-once` low-level flag authorizes the selected
experiment. Only explicit probe 38 changes its attempt limit to six; all other
cases retain zero/one-attempt semantics. The portable collector supplies this
flag automatically from its immutable case selection.

Diagnostics schema **5** retains all 56 raw fields and identifies the new
mode/serial contract. Old capture schemas 1–4 remain readable; selecting the
new case on an older image is rejected before any device access. Package
releases: airoha-pon **84**, bench helper **18**, controller unchanged at **11**.

Each attempt produces a serial `RX recovery phase=before/after` pair containing
sample time, sync, both LOS values, frames, LOF, FEC, all seven PCS counters,
raw NCPO and cumulative checked writes. One terminal marker records the reason
and consumed attempt count. A sync observed by the post-attempt sample can
place that terminal marker before the matching `after` record.

`probe-report.json` adds `recovery_series`, including every raw boundary and
counter deltas **from the end of one reset to the start of the next**.
Missing, duplicate, malformed, out-of-order, early or over-budget records fail
validation. Counter differences across resets are not treated as reception.
`attempt_phases` groups checker observations; `receiver-report.json` groups PHY
words and PCS deltas by attempt. Raw NCPO is not a calibrated frequency or lock
flag. Sync, progressing frame/PCS counters, and retained power/LOS evidence
remain the useful discriminators.

Hardware errors, lost light and cleanup failures stop the suite and preserve
evidence. A normal no-sync outcome after six successful attempts is a valid
negative observation. Shutdown diagnosis remains a separate problem, with its
existing phase/error records retained in this image.

## Software validation scope

Host fixtures execute the production lifecycle and register probe code with
fake hardware. They cover the six-attempt bound and five-second minimum,
transient IRQ/userspace sync, LOS, errors, stop/start persistence, per-operation
failure injection on initial and subsequent attempts, readback mismatches and
restoration of the original values. Report fixtures cover valid and damaged
serial records, per-attempt counter grouping, image compatibility, sample
promotion and helper attempt limits. The UML kernel test exercises all probe
modes during concurrent poll/IRQ teardown. Exact image inspection verifies the
packaged helper, diagnostics schema and repeated-recovery serial code.

The build artifact contains the actual test results, image/runtime hashes and
source revision. No claim of successful hardware acquisition follows from
these software checks. OEM RAM boot and root access remain untested and are
not prerequisites for collecting these prepared OpenWrt experiments.
