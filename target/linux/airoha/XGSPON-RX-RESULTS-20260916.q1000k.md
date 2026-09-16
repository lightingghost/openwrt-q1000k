# Q1000K RX bench results — 2026-09-16

## Finding: the isolated OEM controller post-init step recovers frames

The `8fee7f3b48` RAM bench received downstream XGS-PON frames after the
`oem-post-init` experiment set **EN7573 A2 register `0x110[8] = 1`**. Both
the original NAND and newer OEM `xponconfig` perform this step after loading
the PON modules. The experiment changes the control word from `0x01002c1f`
to `0x01002d1f`; its checked-write count is exactly one. The hardware field's
name and full electrical semantics remain undocumented.

The first 30-sample baseline had light, both LOS sources clear, no sync and
zero frame/FEC/PCS activity. The following 90-sample isolated experiment had
14 unsynchronized samples before the intervention and **76 synchronized
samples thereafter**. Its final frame count was **706,022**. Within the
post-intervention samples, 696,848 frames arrived over 87,105 ms:
**8,000.092 frames/second**. SOF-to-MAC reached 706,023 and EOF-to-MAC 706,022.
Sampled loss-of-frame, corrected/uncorrected FEC, PSync mismatch and
SFC/PON-ID HEC error counters remained zero.

RX power at the initial check was **14,900 nW / −18.268 dBm**, with controller
and PHY LOS both false. The first successful case remained between −18.39
and −18.27 dBm. These are controller-reported measurements, not an independent
calibration of optical power or signal quality.

Optical TX was inhibited and disabled, registration disabled and the MAC
interrupt mask zero throughout. Thus this bench does not require an upstream
transmission to acquire the observed downstream frames. Subscriber registration,
OMCI service and routed Internet traffic are outside this receive-only result.

## Initial collection and diagnostic stop

The original 58-case collection stopped during its third case:

| Case | Capture / outcome | Cleanup |
|---|---|---|
| `connected-baseline` | 30 samples; light, no sync, zero frames | Passed |
| `oem-post-init` | 90 samples; one checked write; sustained downstream reception | Passed; original field restoration verified |
| `oem-post-cal` | Incomplete: 14 RX samples; diagnostic guard stopped collection during the intervention | Postflight and private RAM input cleanup passed |

The combined case printed `Receiver probe diagnostics/guard check failed`.
Its serial log contains the checked controller write, a fresh eye observation,
normal field restoration and shutdown, without a logged kernel failure.
The last accepted RX sample was timestamped 382384 ms; the checked OEM write
was logged at 382505 ms, and the eye observation at 383901 ms. The two sysfs
reads used for RX status and probe diagnostics each take the PHY callback
mutex. A calibration can run between those reads. This is consistent with
exceeding the helper's 1,500 ms maximum age between paired snapshots.

**The exact rejected diagnostic record was not printed.** The timing-race
explanation is therefore an inference, not a confirmed diagnosis. The failed
case is not a valid negative optical result. The remaining 55 cases in that
original invocation were not run; no guard was relaxed and no automatic retry
was performed. Follow-up captures use independently selected existing cases
after the original postflight/cleanup passed.

## Evidence and reproducibility

- Source revision: `8fee7f3b48cb0a51376eaf55438d203235438599`.
- Image SHA-256: `0e326567ecdc0f067b4f4e3fd27a314b7c10dea02c0612d0cde1ddb2fe046db4`.
- Collector SHA-256: `4d622b412e770444b39bfd279b901c19081ff5cf0da8ad8ba72c023711886da4`.
- Initial capture: `build-artifacts/q1000k-xgspon/rx-8fee7f3b48-20260916-01/`.
- Derived cross-capture summaries: `build-artifacts/q1000k-xgspon/rx-results-20260916/`.

Paths above are relative to `/home/odin/local/q1000k`. Every capture retains
the pinned runtime manifest, attempted command output, controller/RX snapshots,
serial excerpt, postflight and input-cleanup evidence. Private calibration and
MCU payloads are excluded from collection archives.

The same unit DSD record and short OEM PM/DM inputs were used in the successful
case. No firmware or calibration replacement was necessary. This supports the
working transfer/initialization path described in the
[MCU loader audit](XGSPON-MCU-LOADER-AUDIT.q1000k.md); it does not establish the
physical accuracy of every calibration coefficient.

## Follow-up status

### Independent longer confirmation

`rx-8fee7f3b48-20260916-confirm-post-init` completed all 180 samples after a
fresh controller/driver initialization. The first 14 samples again had no
sync and zero frames. All remaining **166 samples were synchronized**, with
one checked intervention. Final frames were **1,542,510**; the synchronized
sample span contained 1,533,272 frames over 191,658 ms, or **8,000.042
frames/second**. FEC total reached 967,154,205. The sampled error counters
listed above remained zero. RX power ranged from −18.57 to −18.36 dBm.

Both successful cases passed field restoration, postflight and private input
cleanup. This demonstrates repeatability across module/controller initialization
on the same RAM boot; it is not a repeated power-cycle or fiber-reconnect test.

### Matched clock/reset and eye comparisons

| Case | Samples | Interventions / checked writes | Outcome | Restoration / cleanup |
|---|---:|---:|---|---|
| `oem-full-reset` | 90 | 1 / 62 | No sync; zero frames, FEC and PCS counters | Passed |
| `oem-reset-repeat` | 90 | 6 / 372 | No sync or counter activity after any attempt; stopped at the configured budget | Passed |
| `eye-current` | 90 | 1 / 81 | No sync or frames; fresh eye result lacked horizontal/vertical ready flags | Passed |
| `oem-analog` | Incomplete | Calibration executed; no complete paired post-intervention sample | Same diagnostic guard failure as the original combined case | Passed |

The repeated case used the identical OEM twelve-bit reset/clock recipe as
the one-attempt control. Its five measured intervals from an attempt's end
to the next start were 6,153, 6,136, 6,251, 6,145 and 6,247 ms, all above the
5,000 ms minimum. All twelve serial boundary records and the terminal
`reason=budget attempts=6` marker were validated. No frame/FEC/PCS activity
appeared in the boundary records or sampled intervals. Original fields were
saved/restored once across the series. This is a valid negative result for
the new community-inspired repeated-recovery hypothesis under the unmodified
controller-bit condition.

The fresh eye case had `x_done=y_done=true` but both ready flags false.
Its measurement was recorded as incomplete, not as a valid zero-width eye
or a physical BER result. The successful isolated post-init cases show that
this extra eye/calibration operation is not required to recover downstream
frames under the tested conditions.

### Calibration guard investigation

The separate `oem-analog` failure reproduced the same helper error and clean
teardown, so the batch stopped before its planned `baseline-repeat` case.
A forensic collector variant is prepared under the derived-results directory.
It changes only the host invocation from `q1000k-pon-bench ...` to
`sh -x /usr/sbin/q1000k-pon-bench ...`. The on-device helper, image, runtime
manifest, all hardware operations, guard conditions and cleanup are unchanged.
The generator retains provenance and the complete dry-run plan/metadata was
checked equal to the delivered collector. Shell tracing can affect sampling
timing; its result is recorded separately from the original captures.

The first traced invocation (`rx-8fee7f3b48-20260916-trace-post-cal`) reached
the helper's 90-sample completion marker and passed hardware postflight and
cleanup. However, mixing shell trace messages with normal stdout interleaved
JSON records. The host validator rejected it with `Extra data: line 1 column
1561 (char 1560)`. It is retained as forensic evidence and **excluded from
validated optical results**; reconstructed values are not used to establish
the finding above.

A second trace variant kept stderr in a separate, initially absent RAM file.
`rx-8fee7f3b48-20260916-trace-separated-post-cal` completed a **30-sample**
window with the normal sample/report validators unchanged:

- 12 unsynchronized pre-intervention samples; 18 synchronized post-intervention
  samples; 174,529 final frames and 271 checked writes.
- 173,551 frames over 21,693 ms within the synchronized samples, approximately
  8,000.323 frames/second. These short-window rates include sampling granularity.
- The first synchronized sample already contained LOF 1, uncorrected FEC 1,535,
  PSync mismatch 3, SFC HEC 3 and PON-ID HEC 2. None of those counters increased
  over the subsequent synchronized samples. This differs from both isolated
  single-bit runs, whose sampled error counters remained zero throughout.
- The fresh eye measurement still lacked ready flags even while frames were
  received. That incomplete measurement cannot be used as proof of an absent
  receive path.
- All status/diagnostic pairs were 78–88 ms apart. The original timing failure
  did not reproduce under tracing. This result does not establish that the
  default collector's pairing issue is fixed.
- Guard checks, restoration, postflight and private input cleanup passed. The
  separate 10,708,486-byte trace was retrieved with matching local/device SHA-256
  `0628a6f75fa55cf62b69105d45b4397f0a6a0dc9896f37f4da12511f4f7e7c75`, then its
  exact temporary RAM file was removed.

Tracing required no helper/module replacement or firmware rebuild. Both
diagnostic collector variants, their generators and provenance are retained.
The original collector and all original capture archives remain unchanged.

## Final control and coverage

`rx-8fee7f3b48-20260916-final-baseline` completed 30 samples with the normal
collector after restoration and fresh initialization. Controller `0x110`
was again `0x01002c1f`. Both LOS sources were clear, RX power was −18.54 to
−18.24 dBm, and synchronization, frames, FEC and all seven PCS counters were
zero. Postflight and private input cleanup passed. This strengthens the
association with the OEM bit rather than a lasting change in the fiber or
controller state.

The session contains **11 case invocations across eight distinct planned
cases**: eight validated captures covering seven distinct cases, two incomplete
untraced calibration captures and one rejected mixed-trace forensic capture.
The 58-case full sweep did not complete. **50 distinct cases were not run** in
this session, including both physical disconnect/reconnect controls.
`oem-analog` still has no complete validated capture on this image.
`evidence-index.json` records every archive/hash and the explicit not-run list.

All eleven invocations passed postflight and private input cleanup. No reboot,
flash or replacement firmware was needed. The previous shutdown fault did not
recur in these runs; this does not establish a permanent shutdown fix. The final
state has the controller off, owned PON modules unloaded, `ponraw` down and
private inputs removed. The separate trace RAM file was also removed after
verified transfer.

## Interpretation and next implementation step

1. **Missing OEM controller initialization is now a demonstrated receive-path
   prerequisite in this setup.** The isolated write recovered frames twice;
   full and repeated clock/reset recovery with that bit clear did not.
2. **The existing MCU/DSD inputs can support downstream reception.** The
   successful runs used the unchanged unit-specific inputs and verified loader.
   There is no evidence here requiring replacement firmware, zero-padding
   changes or new calibration coefficients.
3. **Additional analog recovery is unnecessary for the demonstrated fix.**
   The combined recipe can receive too, but introduces more changes and had
   nonzero transition counters. The two isolated runs are stronger evidence
   for the minimum necessary startup correction.
4. **Upstream transmission is unnecessary for the observed downstream
   acquisition.** All validated samples retain TX inhibit, TX disabled and
   registration disabled. Activation/OMCI/subscriber service still need their
   own later validation.
5. Integrate the exact checked OEM bit write into normal Q1000K XGS startup at
   the OEM-equivalent point after PHY/MAC initialization. Preserve TX policy,
   error handling and lifecycle ownership. Validate automatic startup and
   reconnect behavior before extending testing to registration.
6. Before another full matrix run, make paired snapshot capture robust against
   calibration between the two reads, and log the exact rejected check/record.
   Do not merely increase the permitted age or classify guard failures as
   negative optical tests.

The bit is currently applied only by the selected bench experiment and restored
at cleanup. This session identifies and verifies the receive-side correction;
it does not install it into normal startup.
