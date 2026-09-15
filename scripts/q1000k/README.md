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
all nine PON modules plus four userspace files, RAM root, absent NAND/UBI,
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

For a diagnosed vendor-only retry on the same bench kernel, add
`--modules-from /absolute/path/to/new/bench-artifact` to `bench-run.py stack`.
The new artifact must contain its verified `runtime/` files. Only
`phy_10g.ko`, `xpon_10g.ko` and `airoha_ecnt_xpon.ko` may be substituted; kernel configuration,
source changes and all other module hashes are checked first. Original modules
are saved in RAM and restored after successful unload, including a failed test
whose module cleanup succeeds. Any ambiguous staging/cleanup failure retains
evidence for inspection instead of forcing recovery. A kernel change requires
a user RAM boot of the new image.

`bench-report.py /path/to/stack-01 /path/to/stack-02` summarizes completed
captures without contacting the device. It requires successful postflight and
input cleanup, all five O1/OMCI observations, fresh controller TX-off/LOS
samples, and no failure diagnostics in the captured serial interval. Redirect
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
