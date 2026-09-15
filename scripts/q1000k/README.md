# Reusable Q1000K RAM bench tools

These tools never flash, reboot, or boot a device. Keep the fiber disconnected
for controller/stack tests. The management address is fixed at 192.168.255.1;
192.168.1.1 belongs to the working router.

From the OpenWrt source checkout:

```sh
python3 scripts/q1000k/bench-build.py --output ../build-artifacts/q1000k-xgspon/bench-CHECKPOINT
python3 scripts/q1000k/bench-run.py status --artifact ../build-artifacts/q1000k-xgspon/bench-CHECKPOINT --output ../build-artifacts/q1000k-xgspon/bench-CHECKPOINT/preflight-01
python3 scripts/q1000k/bench-run.py stack --artifact ../build-artifacts/q1000k-xgspon/bench-CHECKPOINT --output ../build-artifacts/q1000k-xgspon/bench-CHECKPOINT/stack-01 --inputs /tmp/PRIVATE-INPUTS.tar --fiber-disconnected
```

Use a new output directory each time. `bench-build.py` requires an idle,
configured cache and committed tracked changes on `q1000k-xgspon`. It pins HEAD
through the separate experimental builder, restores `.config` and `.config.old`
even on failure, and preserves any pre-existing `files` overlay by refusing it.
It records the source/builder commits, protected branch refs, configs, build/test
logs, inspected image hash and runtime file hashes in the artifact directory.
The normal builder and upstream/PR branches are not modified. Local fakeroot
builds may require the environment's IPC permission.

`bench-run.py status` is read-only. Tests first check the exact boot revision,
all nine PON modules plus five userspace files, RAM root, absent NAND/UBI,
immutable TX inhibit, disabled services, idle PON modules and endpoint. The
private input tar must contain this unit's previously verified calibration,
firmware pair and checksum file; no private input contents are logged. The test
sets `kernel.panic=0` in RAM, verifies readback, and invokes the explicit bench
helper. This timeout remains zero for diagnosis after a failure. No optical TX
activation is requested. The helper unloads owned modules in dependency order;
failed cleanup never triggers force-unload or automatic recovery.

Each run records SSH logs, the new bytes from `/tmp/serial_output.log` (override
with `--serial-log`), and absolute artifact/capture paths in `checkpoint.json`.
The source serial log is never truncated. A successful postflight proves module
cleanup and endpoint state, not successful physical retirement: inspect the
attempt and serial logs too. Failed attempts are never automatically retried.
Verified staged inputs are removed only after the modules are confirmed idle.
Keep capture directories private; serial/kernel output may contain identifiers.

If the temporary private tar has been lost, reconstruct it locally from the
saved, verified inputs without contacting the device:

```sh
python3 scripts/q1000k/bench-inputs.py --source /absolute/path/to/private-inputs --output /tmp/PRIVATE-INPUTS.tar
```

The source directory contains `xgspon-calibration.bin` and the firmware pair
under `lib/firmware/airoha/q1000k/`. The helper checks the same exact sizes and
SHA256 hashes as the runner, rejects symlinked input files, creates a new 0600
archive with a checksum manifest, and validates it using the runner. It never
overwrites an existing output or prints private contents.

For a diagnosed vendor-only retry on the same bench kernel, add
`--modules-from /absolute/path/to/new/bench-artifact` to `bench-run.py stack`.
The new artifact must contain its verified `runtime/` files. Only
`phy_10g.ko`, `xpon_10g.ko` and `airoha_ecnt_xpon.ko` may be substituted; kernel configuration,
source changes and all other module hashes are checked first. Original modules
are saved in RAM and restored after successful unload, including a failed test
whose module cleanup succeeds. Any ambiguous staging/cleanup failure retains
evidence for inspection instead of forcing recovery. A kernel change requires
a user RAM boot of the new image.

`bench-report.py /path/to/stack-01 /path/to/rx-connected-01` summarizes completed
captures without contacting the device. It requires successful postflight and
input cleanup, all five stack or thirty receive O1/OMCI observations, matching
controller TX-off/LOS samples, and no failure diagnostics in the captured
serial interval. Receive reports also verify TX/registration/IRQ guards,
sample freshness, polling and final downstream stability for connected fiber. Redirect
its JSON output beside the captures to retain the aggregate result. It does
not report optical service or traffic validation from disconnected-fiber runs.

`resources --fiber-disconnected` captures the provider's configuration/reset
diagnostic without starting the controller, PHY or MAC. It sets the RAM panic
timeout to zero, loads only hook/SCU/MAC resource providers, then unloads its
modules in reverse order. These providers map and read resources without
changing hardware clocks or resets. Private inputs are not used. It accepts
`--modules-from` under the same matching-kernel/dependency guards as `stack`.

`status --registers` reads a fixed list of seven configuration/reset words
only when `/dev/mem` is available. It never writes a value or reads interrupt
status/FIFO registers. Current RAM images omit `/dev/mem`, so this check stops
without reading hardware; resource-provider kernel diagnostics are used instead.

The `receive` action is a separate, bounded RX-only test (vendor r70,
controller r6, bench helper r5). It requires a new RAM image, including the
controller API and fiber LED DT wiring; do not substitute it into an older
boot using `--modules-from`. First run with `--fiber-disconnected` to validate
its guarded startup and shutdown. Once that passes, the same action accepts
`--fiber-connected` for a user-confirmed connected-fiber observation:

```sh
python3 scripts/q1000k/bench-run.py receive --artifact /absolute/path/to/new-bench --output /absolute/path/to/rx-dark-01 --inputs /tmp/PRIVATE-INPUTS.tar --fiber-disconnected
python3 scripts/q1000k/bench-run.py receive --artifact /absolute/path/to/new-bench --output /absolute/path/to/rx-connected-01 --inputs /tmp/PRIVATE-INPUTS.tar --fiber-connected
```

The PHY verifies the cached controller TX inhibit and live TX-off state before
accepting RX bench mode. The MAC protocol worker and its IRQ remain stopped,
MAC source enables remain zero, and external packet hooks remain unavailable.
Only RX LOS/ready/sync/LOF PHY interrupts are acknowledged; neither IRQ nor poll
callbacks dispatch vendor registration events. Thirty one-second observations
check the guards, fresh sample timestamps and poll activity; connected mode
requires the final five intervals to have both LOS indications clear, sync,
and advancing downstream frames. Full-width counters may wrap. FEC and IRQ
counts are recorded without clearing counters; a passing sample does not
establish acceptable BER or optical service. No subscriber credentials or
transmit request are used. Normal `stack`, `controller` and `resources` actions
continue to require disconnected fiber.

The helper also records the fiber LED class brightness during stack/receive
runs. The link indicator is red on LOS, blinking green during acquisition and
steady green when registered. It turns off when the stack releases it.
Software blinking can be sampled in either phase, so class brightness alone
is not visual confirmation. The separate GPIO24 activity LED remains unused.


`bench-record-uml.py` verifies and saves already completed local PHY/core UML
runs beside a completed image artifact. It checks source equivalence, including
the two OMCI fixture overlays, passing guest logs and the selected revision.
Existing evidence must match exactly; it is never overwritten with different
contents. It has no device access. For example:

```sh
python3 scripts/q1000k/bench-record-uml.py --artifact /absolute/path/to/bench-artifact --phy-run /tmp/q1000k-pon-phy-uml.RUN --core-run /tmp/q1000k-omci-core-uml.RUN
```

For a PHY-only change, use `--phy-only` in place of `--core-run`. This writes
`uml-phy-source-verification.json` and preserves only the new PHY evidence;
it does not claim a new OMCI core run.

Vendor r73 includes thirteen receiver control/status words in the nested
`receiver` object of each `rx_bench_status` sample. Controller r7 and helper r7
also record `receiver_status` before PHY startup and at each observation.
These are read-only snapshots under the existing driver locks. They do not
change clocks, reset the receiver, or dispatch vendor registration callbacks.
Raw frequency/firmware words are not measured Hz or proof of running firmware.
The register map and next connected-test rationale are recorded in
`target/linux/airoha/XGSPON-BENCH.q1000k.md`. These controller changes require a
new RAM boot; the runner must not substitute them into the old image.

`bench-receiver-report.py /absolute/path/to/receive-capture` summarizes the
thirty RX and thirty-one controller snapshots, including hexadecimal register
values, even when downstream stability failed. It requires completed cleanup
and retains the original bench failure; it never produces a passing service
report. Keep using `bench-report.py` for acceptance.

`oem-pon-reference.py --image /absolute/path/to/OEM.squashfs --output /new/directory`
saves the two OEM PON controller/PHY modules, initialization script, disassembly
and input hashes for offline comparison. It reads only the fixed file list,
creates a private output directory, never executes the extracted OEM code,
and never contacts a device.

Vendor r74 / helper r8 add an explicit `--reacquire-once` option to connected
`receive` tests. The default is unchanged observation-only mode. With this
option, after ten consecutive PHY polls with both LOS sources clear and no
sync, the callback permits **one** reference PMA `PLUG_OUT`/`PLUG_IN` recovery
per module lifetime. It requires completed initial calibration and TX-off
intent. It does not rerun calibration, reset the SCU, enable TX, or enter the
vendor registration handler. Stopping/starting the PHY cannot replenish the
attempt budget. Snapshot JSON records `reacquire_enabled` and
`reacquire_attempts`; the reports retain separate pre/post receiver words.

After RAM booting a matching new image, opt in explicitly:

```sh
python3 scripts/q1000k/bench-run.py receive --artifact /absolute/path/to/new-bench --output /new/capture --inputs /tmp/PRIVATE-INPUTS.tar --fiber-connected --reacquire-once
```

The helper defaults to thirty samples and requires final sync plus advancing
frames. Reacquisition does not relax acceptance, TX/registration guards, or
reverse cleanup. The flag is rejected for status, controller, stack, resources
and disconnected-fiber tests before any device mutation.

### Multiple experiments on one RAM image

Vendor r75 / helper r9 collect 24 PHY control/status words in every RX sample
(`receiver_version=2`), together with the seven controller words, frame/FEC
counters, LOS, OMCI inactivity, protocol errors and fiber LED brightness.
The additional words cover forced clock-lock status, calibration controls,
receiver reset releases and PLL configuration. All are ordinary register
reads through the existing PHY owner; there are no arbitrary register accesses.

`bench-run.py receive --samples 30|90|180` selects the bounded observation count
without a rebuild. There is a one-second sleep between observations; device
reads add overhead, so these are sample counts rather than exact durations.
The host timeout scales with the count. The default stays 30 for old captures.
All reports require the requested number of observations and retain failures.
Host runners serialize staging through cleanup with a local device lock, in
addition to the existing device-side module ownership lock.

After a single matching RAM boot, the saved connected matrix runs a 30-sample
baseline, then a 90-sample follow-up. If the baseline already has stable frames,
the second run only observes. If light stays present without stable frames,
it opts into the existing single PMA reacquisition. Any incomplete capture,
TX/OMCI/controller guard failure, kernel diagnostic, uncertain light state or
cleanup failure stops the matrix. It never retries a failed stage. The two
runs unload and verify cleanup separately; there is at most one opt-in
reacquisition in the entire matrix.

```sh
python3 scripts/q1000k/bench-matrix.py --artifact /absolute/path/to/new-bench --output /new/matrix --inputs /tmp/PRIVATE-INPUTS.tar --fiber-connected
```

Use `--dry-run` to print the sequence without SSH or file writes. Use
`--extended-samples 180` for a longer follow-up on the same image. `matrix.json`
records each completed stage, with full observations and receiver diagnostics
under each capture. A completed matrix with no stable downstream frames still
exits unsuccessfully; it does not claim optical service acceptance.

| Work | Scheduling |
| --- | --- |
| PHY clocks, calibration controls, controller, counters, LED and OMCI status | Collect together in each RX observation window, under their existing locks. |
| Baseline, optional PMA recovery, longer observation | Select at runtime from one image; execute sequentially on the shared PHY. |
| Full image build and PHY UML tests using a separate cached UML source/build | Can run concurrently while the committed source is held fixed. |
| Host tests that consume prepared vendor/kernel sources | Run after the image build finishes preparing those sources. |
| Analysis of completed captures and OEM disassembly | Can run alongside compilation; never executes OEM binaries. |
| New register writes, SCU recovery, optical TX or registration | Not part of this matrix; require separate implementation and review. |

Build again when kernel/driver code, ABI, device tree or packaged files change.
Do not build again just to change a supported sample count, choose observation
versus single recovery, rerun a host report, or compare captured registers.

`bench-matrix-report.py /absolute/path/to/completed-matrix` revalidates both
stages and produces a compact comparison, including the first observed
recovery sample, post-recovery observation time and changed PHY words. It
uses saved logs only and preserves each failed result. This can be rerun
without a new image, another hardware experiment or subscriber credentials.

### Optional PLL restoration in the same image

Vendor r76 / helper r10 add `--restore-pll` to the connected receive test.
It requires `--reacquire-once`; the default recovery is unchanged. After the
checked PMA out/in recovery, it performs the thirteen PHY clock-control updates
in the imported `TXPLL_on()` routine, with a controller/provider check before
each update and readback of each modified bit. It preserves the reference
6 and 500 microsecond delays. The original one-attempt budget still applies.

The OEM reconnect path includes this PLL routine, but also differs in digital
reset bits and TDC timing. This experiment isolates PLL restoration after the
existing recovery; it does not reproduce the complete OEM reset sequence or
claim a working receiver. The optical controller's immutable TX inhibit stays
asserted, and no registration is enabled.

The same image supports observation, existing recovery, and recovery with PLL
restoration. Add `--restore-pll` to `bench-matrix.py` to select the last option
for its recovery stage. The baseline never restores PLLs; a stable baseline
still selects observation only. No additional matrix stage or automatic retry
is introduced. Use `--dry-run` to review the selection without device access.

Every RX sample now has `pll_restore_enabled`, `receiver_version=3`, and 28
PHY words. Four new words cover `pll_force` (0x1fa8b854), `pll_measure`
(0x1fa8a04c), `pll_kband` (0x1fa8a094), and `pll_outputs` (0x1fa8a060).
Reports check the requested mode against every sample and retain compatibility
with saved versions 1 and 2. These raw controls are not measured clock
frequencies, calibrated RX power, or proof of PLL lock.

### RX power and optional OEM receiver gain

The next consolidated bench (vendor r77, helper r11) reports MCU RX power in
`rx_power_nw` and `rx_power_valid`, with a nullable dBm summary in saved reports.
CLI status also supplies `rx_power_dbm`; LuCI shows dBm/nW. Unavailable readings
remain null. The test records the sensor alongside the receiver diagnostics in
every observation window, including pre-registration O1.

`bench-matrix.py --restore-gain` selects the new isolated OEM RX frontend gain
setting for the 90-sample continuation only. It requires the same connected,
TX-inhibited RAM environment as the prior PLL trial. The helper accepts
`--reacquire-once --restore-gain`, optionally with `--restore-pll`. All use one
attempt after ten consecutive light/no-sync polls. Gain controls restore on
stop; report validation requires the serial restoration confirmation when an
attempt occurred. Schema 4 captures 29 PHY words, including `rx_frontend_gain`.

The full build wrapper now also runs status-backend and LuCI view tests and
stores their logs beside the existing PON test and image-inspection evidence.
