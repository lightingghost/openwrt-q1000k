# Explicit experimental build preparation

The separate builder repository now has a `q1000k-xgspon` branch with checkpoint
`e768c6a` (`q1000k: add isolated revision-pinned XGS-PON build profile`). The
normal builder checkout remains on `main` at `fe45fdb`, and its normal profile
and workflows still select OpenWrt `q1000k-dev`.

The experimental builder is available locally at
`/home/odin/local/q1000k/q1000k-build-xgspon`. Its
`user/q1000k-xgspon/README.md` documents the manual preparation and verification
commands. `scripts/q1000k-xgspon-build.py prepare` requires a full source
revision, creates a new checkout, verifies membership in `q1000k-xgspon` and
selects that revision detached. It refuses existing output directories and
requires all package sources. A local repository can be selected explicitly
for unpushed checkpoints. A manifest records source, revision and profile.

The profile combines the normal Q1000K image configuration with these packages:

- `kmod-q1000k-pon-control`
- `kmod-airoha-xpon-en757x`
- `kmod-q1000k-omci`
- `q1000k-xgspon`
- `q1000k-omci-tools`
- `luci-app-econet-xpon`
- `q1000k-xgspon-service`
- `q1000k-xgspon-wan`

It enables the experimental Kconfig visibility needed by the BROKEN packages.
It does not alter device-tree nodes, supply firmware/calibration/identity,
enable the service or start WAN interfaces. The helper has no device,
publication, flash or automatic workflow operation. Independently fetched
feed revisions must also be recorded for a reproducible build.

After feeds setup and Kconfig resolution, `verify` rejects source revision or
tracked-file changes, altered profile/manifest data and dropped or changed
package selections. It does not compile the firmware or claim runtime safety.

Validation on 2026-09-14:

- Seven tests with disposable Git repositories passed, including a pinned
  ancestor, unrelated branch, missing packages, existing output preservation,
  changed source and configuration loss.
- The helper prepared actual source revision
  `cf1c8a2d6f46bae660f053c2079caca405dcdb1b` in a fresh local clone.
- The existing prepared OpenWrt Kconfig graph resolved the profile into that
  clone's `.config`, and `verify` accepted every required selection. Kconfig
  emitted existing recursive-dependency diagnostics for the unrelated
  `librespeed-cli`/`luci-app-librespeed` feed packages; its exit status was zero.
  This was configuration validation, not a clean image build.
- The active development `.config` hash remained
  `06b504ec6da6c377a27ac06be034acd4074a85b15dd54773c35cc3a6f97bd356`.
  No source ref, normal builder profile, device or firmware was changed by
  the validation.

All hardware access remains read-only and firmware flashing remains forbidden.
The current [acceptance status](XGSPON.q1000k.md#current-acceptance-status--2026-09-14)
lists the tests that still require hardware changes and therefore cannot run
under the current restriction.
