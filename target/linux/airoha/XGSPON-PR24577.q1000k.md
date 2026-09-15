# PR #24577 reuse review for Q1000K

2026-09-15 RX follow-up: the live head remains
`d7569c5e26551084e7643b0e83ecda9c31f49f11`, open and unmerged. The
[remaining-hypothesis plan](XGSPON-RX-NEXT.q1000k.md) records the renewed
source review and its hardware limits. EN7571 optical bring-up, high-speed RX
initialization and CDR acquisition are separate in the reference driver;
responsive LOS alone does not validate the electrical receive path. No EN7528
register sequence or laser default was imported for the next Q1000K bench.

Reviewed 2026-09-12 for the [Q1000K XGS-PON plan](XGSPON.q1000k.md).
Source: [OpenWrt PR #24577](https://github.com/openwrt/openwrt/pull/24577),
`AKoo7/openwrt:econet-xpon-gpon`, head
`d7569c5e26551084e7643b0e83ecda9c31f49f11`. The live API reports open,
unmerged and not draft; the PR body's older draft wording is stale.
This is a source review, not an import or a hardware test.

Implementation update: the LuCI commit has since been imported separately
and adapted for Q1000K. Its packages build and the new backend passed a live
RAM-only check. The OMCI prototype cross-compiles for AArch64 but remains
unimported. See the [implementation checkpoint](XGSPON-STATUS.q1000k.md)
for those results and the subsequent alternative-kernel review. The rest
of this document records the original upstream review and reuse decisions.

The PR targets EN7528 with an EN7571 optical frontend and reports GPON
service on a DASAN H660GM-A. Q1000K has AN7581SIT and two EN7573AN devices.
The LuCI views and portions of the userspace design are reusable, but the
driver, optical initialization and data-path interfaces need a different
implementation for Q1000K.

| Component / source commit | Decision for Q1000K |
| --- | --- |
| `luci-app-econet-xpon`, `e27eee81fddad217e111ce67bc7a8102b00b24b4` | Import as a separate source commit, then adapt dependencies, RPC data sources, identity defaults and XGS-PON presentation. Keep unselected until the adapted backend works. |
| `econet-xpon-config`, `d7cff0d1cbf8568031a265764c375ff8fc8ee695` | Reuse the UCI identity/service concept with attribution; replace EN7528 module loading and validation for Q1000K. |
| `econet-omcid`, `bd95d9e8b929b02ddb775b0ba974a004585376ca` | Evaluate as a small native prototype. It needs transport, MIB, service-mapping and lifecycle work; it is not yet a complete Q1000K OMCI implementation. |
| EN7528 driver, board and Ethernet changes, `7ef655b83d76989f9ea5ff05a550e5dd645ee879` | Use as architectural reference. Do not import the unrelated board, flash layout or MIPS Ethernet integration. |
| elfutils host fix, `d7569c5e26551084e7643b0e83ecda9c31f49f11` | Exclude from XGS-PON scope. |

All imports belong to `q1000k-xgspon`, based on `q1000k-dev`. Preserve
original authorship, license files and trailers, and keep Q1000K changes
in follow-up commits. A partial import must list its retained scope and
source revision. Recheck the source head before implementation because
the PR has rewritten its commit history.

The LuCI package consists of three JavaScript views (status, identity,
and raw diagnostics), a shell rpcd backend, menu/ACL files and packaging.
Its package architecture is `all`, but the dependency chain is
`luci-app-econet-xpon -> econet-xpon-config -> kmod-econet-xpon`, with the
configuration package restricted to `TARGET_econet`. An unchanged import
therefore does not provide a usable Airoha package selection.

Required adaptations, verified against the source:

- Replace the hardcoded `ponwan0` interface, `/sys/module/econet_xpon`
  parameters and `/proc/econet_xpon_*` parsing with the Q1000K backend.
  The current backend also identifies equipment as `Airoha EN7528`.
- Derive operational state from actual driver state. The current
  `gpon_state` is parsed from `Mode:` in `ponInfo`; it must not be assumed
  to represent an O1/O5 activation state without verifying the ABI.
- Detect the real OMCI service separately from the `xpon_daemon` kernel
  task. The latter currently drives the UI's OMCI-agent flag. A running
  process alone also does not prove successful provisioning.
- Represent unavailable and stale data explicitly. The backend defaults
  absent LOS to zero, and the view renders zero LOS as signal OK. Failed
  polling leaves old status on screen; missing individual DDM fields
  also become zero. Correct these before use during hardware bring-up.
- Validate the EN7573 diagnostic representation, signed temperature and
  scaling before reusing the SFF-8472 conversions. Expose only supported
  measurements, with meaningful units.
- Use this unit's preserved FSAN and WAN MAC as defaults. The reference
  UI suggests an empty serial uses a driver default even though the
  loader refuses to load without a serial. Replace this contradictory
  behavior and the random-WAN-MAC wording with Q1000K factory handling.
- Adapt the GPON password field and 10-character validation to the actual
  XGS-PON registration/authentication interface. Validate on the backend
  too; LuCI-only checks do not constrain direct UCI changes.
- Replace the loader's autoload-file generation and full command logging.
  It currently places supplied identity/password arguments in
  `/etc/modules.d/91-econet-xpon` and logs the entire line. Q1000K's loader
  must enforce calibration/firmware ordering and avoid logging secrets.
- The "MIB data" view is currently four raw procfs dumps. Label these as
  diagnostics unless a real managed-entity view is implemented. Remove
  stale statements about no userspace OMCI and unsupported UI placeholders.
- Declare backend command dependencies and use the established LuCI
  package integration. Check ACL scope, JSON output and retained settings.

The `econet-omcid` source is small and uses libc/Linux socket APIs, making
an AArch64 compilation experiment reasonable. Compilation alone would
not verify its protocol or hardware integration. The inspected source:

- Receives raw OMCI PDUs through an `omci` AF_PACKET netdevice and assumes
  the driver/hardware supply the relevant framing and MIC behavior. The
  selected AN7581 transport must be checked against these assumptions.
- Handles baseline device ID `0x0a`; extended `0x0b` messages are skipped.
  Determine the intended service's requirements and implement missing
  message handling before declaring compatibility.
- Seeds H660GM-A/DZS-specific managed entities, software-image versions,
  four Ethernet UNIs and a VEIP/HGU profile. Q1000K's two copper ports,
  OEM SFU behavior and FIT/UBI layout need an appropriate MIB model.
- Programs `/proc/econet_xpon_mkgem`, and includes implicit GEM creation
  when the OLT did not explicitly create the expected entry. Replace
  assumptions with verified service mapping and error propagation.
- Replies with success for some unimplemented actions, including an
  ignored reboot and the generic fallback. Implement supported behavior
  and report unsupported operations accurately.
- Saves MIB state to `/tmp/mib.dump`, restores it without a complete
  datapath replay, and has no packaged init service. Define restart and
  reset semantics, use safe file handling, and add procd supervision.
- Logs a warning but continues on invalid/missing serial. The Q1000K
  service should reject incomplete identity before enabling registration.

The discussion also points to [Sirherobrine23's Airoha kernel work](https://github.com/Sirherobrine23/airoha_kernel)
as an alternative OMCI/driver design. That tree has not been audited in
this review. Compare its actual AN7581/XGS-PON coverage during the
dependency inventory instead of assuming GPON support establishes it.

Validation for the eventual LuCI import: package selection/build on the
Airoha target; shell/JavaScript/JSON checks; fixture-driven tests for absent
drivers, unavailable sensors, LOS, registration without service, healthy
OMCI, failed polls and invalid identity; then browser and hardware checks.
The full PR and OEM reference were inspected only in temporary files;
neither its packages nor firmware have been installed on a device.
