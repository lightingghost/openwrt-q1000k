# Q1000K XGS-PON implementation plan

Prepared 2026-09-12 on the user-requested `q1000k-xgspon` branch, created
from `q1000k-dev` at `b287be4f00581e04ddee27f1157a4897455078e5`.
All XGS-PON plans, imports and adaptations belong to this branch.
Hardware baseline supplied by the user: **AN7581SIT SoC and
two EN7573AN devices**. This document plans the implementation; it does not
claim functioning optical service.

Implementation has started. See the [current implementation checkpoint](XGSPON-STATUS.q1000k.md)
for the completed imports, factory/RPC/LuCI foundation, confirmed controller
selection/power mapping and verified XGS-PON MD32 bring-up. The standalone
controller and pinctrl/DT changes are implemented; full-image cold boot,
remaining analog/PHY work, MAC/QDMA and OMCI are still pending. The numbered steps
below remain the full acceptance plan; they are not all complete.

The reference is [coolsnowwolf/lede commit f7fd86e](https://github.com/coolsnowwolf/lede/commit/f7fd86eaa58c29fed97da04ab219c74a835a9358).
Its relevant package is `kmod-airoha-xpon-en757x`, variant `v2`, containing
the BSP modules, `phy_10g.ko` and `xpon_10g.ko`. The package permits unresolved
symbols with `KBUILD_MODPOST_WARN=1`; its porting patch substitutes no-op
GPON flow hooks and stores private packet metadata in `skb->cb`. These
require runtime integration review. The PHY source recognizes EN7572/EN7573,
but the package does not include the OEM's separate `en7572.ko` optical
controller loader or `A60993.elf.pm` / `A60993.elf.dm` firmware. It also does
not package the OEM userspace OMCI stack. A successful package build would
therefore be only an early milestone.

Also reviewed: [OpenWrt PR #24577](https://github.com/openwrt/openwrt/pull/24577),
at head `d7569c5e26551084e7643b0e83ecda9c31f49f11` in
`AKoo7/openwrt:econet-xpon-gpon`. The PR is open and unmerged as of this
review. Its EN7528/EN7571 GPON implementation is a separate hardware path;
the reported H660GM-A testing does not establish AN7581/EN7573 XGS-PON
compatibility. Reuse its LuCI application with a Q1000K backend, and
evaluate its native OMCI daemon as a prototype rather than treating the
whole PR as an AN7581 driver import. Exact import decisions and gaps are
recorded in [the PR #24577 review](XGSPON-PR24577.q1000k.md).

1. **Resolve the hardware and missing-source dependencies first.**

   Inventory the OEM `xponconfig` sequence, optical modules, userspace
   libraries and firmware against the reference package. Trace the role of
   each EN7573AN, its I2C addressing/selection, reset, power, TX-disable,
   burst-enable and LOS wiring. The OEM switches between GPON and XGS-PON
   using GPIOs and separate calibration records; confirm the physical
   topology before assigning a role to either chip. Translate vendor GPIO
   numbers to current pinctrl offsets instead of copying global numbers.

   Locate matching EN7572/EN7573 LDDLA/MD32 loader source and determine how
   to supply the matching firmware. The OEM's `en7572` names are compatible
   with its family-based detection and do not contradict the EN7573AN
   markings. Check the two-byte I2C register addressing and adapter mapping
   used by the reference compatibility layer. Identify all missing QDMA,
   frame-engine, flow-mapping and OMCI interfaces. Inspect OEM binaries for
   ABI evidence where useful; the Linux 5.4 modules cannot be loaded into
   the current Linux 6.18 build.

   Deliverable: a dependency matrix marking each component available,
   requiring a port, or missing. Resolve the optical-loader and OMCI
   implementation paths before committing to a completion estimate.

2. **Import the references with traceable history on `q1000k-xgspon`.**

   Preserve the source commit and authorship where possible, then place
   Q1000K adaptations in separate commits. If only part is imported,
   document the retained files and original commit. Follow the
   [branch policy](../../../AGENTS.md): `main` remains official
   upstream; `q1000k-support` remains reserved for the support PR. Preserve
   existing uncommitted work, including the untracked board README.

   Import the self-contained `luci-app-econet-xpon` commit
   `e27eee81fddad217e111ce67bc7a8102b00b24b4`, preserving AK Sharma's
   authorship and source trailers. Follow it with a separate Q1000K
   adaptation commit. Replace its dependency on the EN7528-specific
   `econet-xpon-config` with the Q1000K service/backend; do not pull in
   `kmod-econet-xpon` to satisfy the original dependency. Keep the package
   unselected until its dependency graph and unavailable-data behavior
   work on the Airoha target. Retain source references when adapting the
   separate configuration and OMCI commits; do not import the PR's board,
   MIPS Ethernet, flash-layout or host-tool changes into this series.

   Keep the package optional during development. Select the AN7581 code
   path and audit the unconditional AN7583 BSP modules before enabling
   automatic loading. Classify every unresolved symbol and compatibility
   stub. Require a build without suppressed unresolved-symbol errors and
   verify the target kernel's crypto and I2C dependencies.

   Acceptance: reproducible package build against the actual development
   kernel, with a complete module dependency graph and no hidden missing
   symbols. This does not yet establish hardware compatibility.

3. **Implement read-only factory-data access and optical initialization.**

   Add a board-specific consumer for the existing `factory` UBI volume,
   found by name. Use the preserved DSD records as a validated fallback for
   older installations that lack the copied fields. Account for the NAND
   view and bad-block handling when reading original DSD; do not assume a
   raw offset alone guarantees the correct bytes.

   | Item | Offset in factory volume | Original NAND location |
   | --- | --- | --- |
   | PON FSAN | `0x9000`, 12 ASCII bytes plus NUL | `fsan` field in DSD starting at `0x00400000` |
   | XGS-PON calibration | `0xb000`, 513 bytes | `0x00412000` |
   | GPON calibration, retained for later support | `0xa000`, 513 bytes | `0x00411000` |

   Validate lengths and content, keep identifiers out of routine logs,
   and determine the loader's required working-file format and padding.
   Never substitute the GPON record for XGS-PON or use another unit's
   calibration. Port the EN7573 controller initialization and load the
   matching MD32 program/data before the SoC PHY and MAC. Represent board
   signals through device-tree GPIO descriptors and implement bounded
   timeouts and error unwinding. Keep TX disabled when required data or
   initialization is missing.

   Acceptance: repeatable detection, firmware loading, calibration and
   diagnostics, including cold boot. Factory access must not require an
   installer rerun or modification of DSD/ART.

4. **Integrate the PON MAC with the existing Ethernet driver.**

   Map register regions, clocks, resets, interrupts and DMA ownership
   between the vendor BSP and OpenWrt's Airoha Ethernet/PCS drivers. The
   current Q1000K DTS disables both `pon_pcs` and `gdm2`; enabling these
   labels alone is insufficient. Determine the actual internal XGS-PON
   connection and link-state model before changing them. Ensure one owner
   for each hardware block and coordinate shared register access.

   Prefer retaining the working Airoha LAN/DSA driver and adapting the PON
   control and packet interfaces around it. Implement the required WAN
   QDMA handoff, management-frame delivery, GEM/XGEM and T-CONT/queue
   mapping, and VLAN behavior. Audit the lifetime of `skb->cb` metadata
   across DMA, the network stack, bridges and offload. Replace required
   no-op hooks with functioning integration or explicit unsupported
   errors. Avoid competing vendor and OpenWrt WAN netdevices.

   Acceptance: one coherent optical data path with working management
   frame TX/RX, valid packet ownership and no regressions on `lan1` or
   `lan2`. Validate initially with flow offload disabled.

5. **Provide OMCI and subscriber configuration.**

   Evaluate PR #24577's `econet-omcid` commit
   `bd95d9e8b929b02ddb775b0ba974a004585376ca` first as a source-buildable
   candidate, alongside the OMCI work linked from the PR discussion.
   It currently handles baseline OMCI only, uses an H660GM-A/DZS MIB,
   and programs EN7528 procfs interfaces. Adapting transport, management
   entities, service mapping and required extended-message handling is
   necessary before claiming support for the intended XGS-PON service.
   Audit success stubs, unsolicited GEM creation, MIB replay and error
   handling; provision only the service configured by the intended OLT.
   Add the missing procd lifecycle and fail startup on invalid identity.

   Choose the implementation based on compatibility with the imported
   kernel API and the actual service. Assess OEM daemon/libraries as a possible
   development reference, including architecture, libc and ioctl
   dependencies; do not assume the binary can run unchanged. Add the
   required PLOAM identity/authentication configuration using this unit's
   factory identity and its service parameters. Determine the required
   OMCI managed entities, OMCC transport, VLAN rules, GEM/XGEM ports and
   T-CONT allocations from the actual service.

   Adapt any OEM image-management behavior to the Q1000K FIT/UBI layout;
   old partition-writing routines cannot be carried over unchanged.
   Keep GPON autodetection as later work: first establish a deterministic
   XGS-PON startup and recovery sequence.

   Acceptance: the intended OLT accepts registration, the ONU reaches its
   operational state, and OMCI completes service provisioning. Optical
   signal detection or registration alone does not establish Internet
   service.

6. **Add OpenWrt configuration and build integration.**

   Add a procd-managed service for factory-data loading, optical startup,
   PON control and OMCI, with observable failures and bounded restarts.
   Expose useful status through ubus: LOS, registration state, OMCI state,
   optical levels where supported, and error counters. Adapt
   `luci-app-econet-xpon` for the web interface rather than creating a new
   page from scratch. Map its RPC methods to that backend, discover the
   actual optical netdevice, expose XGS-PON terminology and validate
   diagnostic units. Separate signal detection, registration, OMCI daemon
   health and service readiness. Missing sensors, failed RPC calls and
   stale samples must display unavailable rather than healthy defaults.
   Replace the misleading raw-procfs "MIB data" view with accurately
   named diagnostics or a real OMCI managed-entity view when supported.

   Use the preserved FSAN and WAN MAC as factory defaults. Apply explicit
   validation in both UI and backend, with authentication fields matching
   the XGS-PON API instead of inheriting the GPON-only 10-character limit.
   Do not copy the reference loader's credential logging or generated
   credential-bearing module-autoload file. Keep loading ordered after
   factory-data and optical initialization.

   Configure the optical interface and any service VLANs in netifd and
   firewall settings. Retain `lan1` and `lan2` as copper LAN interfaces.
   Validate configuration migration for existing installations. Add
   experimental build configuration to this branch when ready for test
   images. Keep `q1000k-build/user/q1000k/settings.ini` selecting
   `q1000k-dev` for normal builds. Provide an explicit experimental
   branch/revision selection and package profile for `q1000k-xgspon`;
   verify the selected revision belongs to that branch. Any builder-side
   work for XGS-PON also belongs on a dedicated experimental branch in
   the separate builder repository. Use packages built for the same
   kernel ABI as the image.

7. **Validate in stages, with a clear pass condition at each stage.**

   | Stage | Required evidence |
   | --- | --- |
   | Build and image | Package/kernel compilation, symbol resolution, DT checks, expected modules/firmware and recovery image |
   | LuCI and backend | Airoha dependency selection; missing-driver, missing-sensor, stale-RPC and LOS states; identity validation; correct units and OMCI process detection; no credential logging |
   | Bench startup | Serial logs, preserved factory data, both LAN ports, EN7573 initialization and stable diagnostics without optical transmission during initial checks |
   | Optical activation | LOS behavior, synchronization, successful ranging/registration and recovery from fiber loss on the intended test service |
   | Service | Successful OMCI provisioning, correct VLAN/GEM mapping, bidirectional traffic, required IPv4/IPv6 behavior and MTU |
   | Reliability | Cold/warm boots, fiber reconnects, daemon restarts, idle/load tests, temperature and memory behavior, configuration-preserving sysupgrade |
   | Acceleration | Repeat the traffic checks with PPE/NPU and bridge offload enabled, checking throughput, packet integrity, counters and recovery |

   Use initramfs/RAM bring-up where supported, retain serial access and
   known-good HTTP recovery. Check calibration/identity preservation by
   comparison before and after upgrade tests. Measure throughput against
   the service and equipment limits rather than assuming nominal 10G
   optical rate is achievable application throughput.

8. **Document support and enable it by default only after validation.**

   Record tested hardware, firmware dependencies, startup order, service
   configuration, recovery procedure and limitations. Keep separate
   commits for the reference import, kernel integration, board/factory
   support, OMCI/userspace and builder configuration. Update the Q1000K
   README's PON status only to the level actually demonstrated on hardware.

Step 1's dependency matrix is now recorded in the implementation checkpoint.
The EN7573 loader, board selection/power wiring and AN7581 datapath remain
the largest gaps; board-level DTS enablement follows their resolution.

Evidence available locally: [PON data inventory](../../../../http-uboot-q1000k/doc/board/airoha/q1000k-pon-data.md),
[current Q1000K DTS](dts/an7581-q1000k.dts),
[OEM DTS](../../../../q1000k_oem.dts), [OEM boot log](../../../../factory_bootlog.log), and the OEM
SquashFS image under `QKX001-06.00.44.00.bin_extract/`. The inspected boot log
shows the firmware/BOB initialization sequence, but reports no detected
PON signal and therefore does not demonstrate successful registration.
