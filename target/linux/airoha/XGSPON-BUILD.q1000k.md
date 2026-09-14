# Explicit experimental build preparation

The separate builder repository now has a `q1000k-xgspon` branch with checkpoint
`e768c6a` (`q1000k: add isolated revision-pinned XGS-PON build profile`). The
normal builder checkout remains on `main` at `fe45fdb`, and its normal profile
and workflows still select OpenWrt `q1000k-dev`.
Follow-up `74aeb41` also rejects untracked source inputs, such as a newly added
patch, while retaining normal Git-ignored build/feeds behavior. Its seven
tests and verification of the real resolved profile pass.

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
tracked/untracked source changes, altered profile/manifest data and dropped or changed
package selections. It does not compile the firmware or claim runtime safety.
Ignored feeds and user overlays require their own provenance records.

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

## Full experimental image build

A subsequent full build completed successfully at source revision
`cb0853acbc5b5ef178f7419b9926a6722f58df9f`, with the same resolved experimental
profile. This used the existing development build cache and recorded the feed
revisions; it is not a clean-room reproducibility claim. The source `.config`
and `.config.old` were backed up and restored byte-for-byte. A temporary,
owned `files/build_info` supplied the full source revision inside the image
and was removed after validation. No other overlay was supplied.

| Image | Size (bytes) | SHA-256 |
| --- | ---: | --- |
| Q1000K UBI initramfs recovery | 8,388,608 | `f25f0bb26c0d1e0770f6c196bf5c45a3d3c5ab94bed082a2f8dbc57cff937d34` |
| Q1000K UBI squashfs sysupgrade | 10,244,374 | `61fd07bed83b374feca7f4bc291efe3a17fa42e535ac6b56add361a4a4bd9459` |

The local artifact directory is
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/cb0853acbc`. It contains
the images, package manifest, source/profile selection, full resolved config,
feed/version/config build information, build log, checksums and inspection
report. The manifest includes vendor r61, core r13, controller r4, diagnostics
r5, supervisor r2, CLI r1, WAN r1 and LuCI r4.

`tests/q1000k/check_pon_images.py` reads the images without booting or executing
their contents. CRC32/SHA-1 FIT hashes pass for both kernels, both embedded
device trees and the sysupgrade root filesystem. Both device trees retain
disabled PON PCS, GDM2, PHY and MAC nodes. The extracted squashfs contains all
expected modules and userspace files, the unchanged disabled service config,
the inactive WAN setup script and no PON module autoload entries. The embedded
`build_info` matches the selected full revision. A deliberately corrupted FIT
payload is rejected before an inspection-success report can be written.

All 80 PON/WAN host tests pass again against the final prepared kernel/vendor
sources. The core UML identity/telemetry suite passed before this full image
build; the core code is unchanged. This closes local image construction and
offline content inspection. Image boot behavior, optical hardware operation,
actual AT&T provisioning, traffic, recovery and browser QA remain unverified.
