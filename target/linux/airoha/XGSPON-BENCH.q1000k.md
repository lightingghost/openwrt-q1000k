# Q1000K RAM bench

This is a separate `quantum_q1000k-xgspon-bench` image target on
`q1000k-xgspon`. It produces an initramfs FIT, no sysupgrade or bootloader
artifact. The normal Q1000K UBI target remains unchanged. RAM boot,
read-only preflight, controller startup and the complete disconnected-fiber
PHY/MAC/OMCI startup/shutdown test have passed. The current device runs
`30ea573aa3`; two cycles validate initial startup and warm reattachment.
Optical service, loaded-pipeline drain and long-duration acceptance remain
pending. See the latest hardware record below.

The bench DT disables the NAND controller and NAND chip, removes partition
definitions and the persistent rootdisk reference, and uses console-only
boot arguments. It enables native GDM2 (`ponraw`) and the manually loaded
vendor PHY/MAC resources; the competing native PON PCS stays disabled.
Native GDM2 uses a 10G internal fixed link for CPU DMA availability, not
optical carrier. The controller caches `quantum,tx-inhibit` at probe and
rejects all consumer TX-enable requests with power-off containment.

The explicit builder `--profile bench` selects this target and
`192.168.255.1/24`, including preinit/failsafe. LAN DHCP, DHCPv6 and RA servers
are disabled, both copper ports remain LAN, and the normal PON service is
disabled. Connect a dedicated host at e.g. `192.168.255.2/24`, with no gateway
on that link. **192.168.1.1 belongs to the user's working router and must
not be used for Q1000K SSH.** No default WAN or OLT identity is inferred.

The user changed the requested default from 192.168.0.1 to **192.168.255.1**
after the stack-test checkpoint. New images use the new subnet; the older
images and runtime logs below retain their original 192.168.0.1 address.

## Staged device tests

The user has RAM-booted the bench, provided SSH at 192.168.0.1, confirmed
disconnected fiber and explicitly authorized the controller-only test. This
permits RAM staging, controller module/GPIO/I2C operations and cleanup.
The user subsequently approved the prepared PHY/MAC/OMCI startup and
shutdown/drain test as well. That approval includes fixing and retrying this
RAM bench test, but does not cover optical activation or flashing. Keep the
fiber physically disconnected. Firmware flashing remains forbidden. Do not use
HTTP recovery upload, sysupgrade, MTD writes, UBI formatting or `saveenv`.
The RAM transfer/boot commands depend on the bootloader actually running;
inspect that bootloader and its memory map before providing commands.
In particular, the local chainloader has `loadaddr=0x81800000`, which overlaps
this image's decompressed kernel range starting at `0x80200000`. Do not use
that default to stage the FIT. Its audited installed-boot implementation
stages images at `0x84000000`; confirm the running bootloader and available
RAM before choosing a transfer address. HTTP recovery uploads write firmware
and are not a RAM-boot transport.

1. On the existing Q1000K kernel at an explicitly confirmed address, collect
   a read-only baseline: identity, kernel/boot arguments, mounts, MTD map,
   network names, module list and relevant logs. This unit's 513-byte XGS
   calibration can be prepared from its documented original NAND backup:
   validate the DSD identity fields with the production factory reader and
   extract its `0x12000` record from DSD at backup offset `0x400000`.
   This checks provenance and format, not an unknown internal checksum.
   Compare with a read-only factory export when available. Do not
   read the working router at 192.168.1.1. Do not replace calibration with
   synthetic data or a record from another unit.
2. User RAM-boots the checked bench FIT. Collect serial boot output. Verify
   the running image revision, RAM root, absent MTD/UBI, address 192.168.255.1,
   disabled DHCP and unloaded PON modules. `q1000k-pon-bench status` performs
   only reads and rejects a persistent root, visible NAND or wrong subnet.
3. After explicit runtime approval, stage the unit's calibration under
   `/tmp` and the locally extracted, hash-verified OEM PM/DM pair under
   `/lib/firmware/airoha/q1000k/`. All are in the RAM filesystem. The generic
   image contains neither subscriber secrets nor calibration/OEM blobs.
4. Run `q1000k-pon-bench controller /tmp/CALIBRATION --fiber-disconnected`.
   It loads only the controller, confirms TX inhibit, detects both paths,
   stages calibration, performs checked firmware load/readback, observes
   MCU/TX/LOS five times, then powers off and unloads its module.
5. Only after reviewing controller results and obtaining stack-test approval, run
   `q1000k-pon-bench stack /tmp/CALIBRATION --fiber-disconnected`. It repeats
   controller initialization, opens `ponraw`, loads the vendor BSP/PHY/MAC
   and OMCI modules, and checks status five times. It uses an explicit
   synthetic bench identity under the TX inhibit; this is not AT&T
   provisioning. Cleanup unloads in reverse order, exercising physical
   drain, and closes the native transport. Failed unloads retain dependent
   modules for diagnosis. A PHY TX request is an error with containment,
   never a fake successful enable.
6. Inspect errors, remaining modules, network health and controller-off
   evidence. Repeat only after explaining any failures. Later optical
   registration, OLT OMCI and traffic/QoS tests need a separately reviewed
   image and explicit approval; this bench cannot validate them.

The helper is a bounded smoke test, not a background daemon. No boot hook
calls it. Status sampling can miss faults between samples; it does not
replace disconnected fiber or prove optical safety/registration.

## Local validation

The production controller fixture tests immutable TX rejection, zero
enable writes, context and lease rejection, and power-off containment.
The bench lifecycle fixture substitutes private files and inert loaders;
it checks RAM/storage/address guards, explicit fiber acknowledgement,
typed inhibit/LOS status, partial startup, reverse cleanup and preservation
of dependencies when an unload fails. Network defaults are exercised with
the real UCI parser in private directories. The dedicated builder tests
reject accidental selection of the UBI profile and changes to the bench IP.

## Built bench image — 2026-09-14

Source `109361469940476fc06bf68303b47514718bcb5e` builds successfully using
the separate builder's bench profile (Kconfig gate fix `aaa8656`). The source
development configuration was restored after the cached build; no device
was accessed. Controller r5 adds the TX inhibit; vendor r61/core r13 are
unchanged. All 87 PON/WAN/bench host tests and eight builder tests pass.

`openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb` is
7,602,176 bytes; SHA-256:
`98717f5c3ca6721ce1381709a4e860bf974081866f95feb04f68db1575915d4d`.
The local artifact directory is
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-1093614699`.

Offline inspection verifies both FIT payload hashes, the enabled bench
resources, disabled NAND/PCS, absent partitions/rootdisk, immutable controller
property, exact source revision and all 1,154 embedded initramfs entries.
It checks the 192.168.0.1 preinit/normal-LAN configuration, exact bench defaults,
required modules/userspace files, absent OEM blobs and absent PON module
autoload, including boot-module symlinks. A corrupt kernel is rejected; a DT
with valid recomputed hashes but the TX-inhibit property removed is rejected
too. This is build/content validation, not hardware acceptance.

## First RAM boot and read-only preflight — 2026-09-14

The user booted the bench and supplied SSH at 192.168.0.1. Read-only checks
confirm the expected model, Linux 6.18.44 and embedded source revision
`109361469940476fc06bf68303b47514718bcb5e`. The boot arguments are
`console=ttyS0,115200 earlycon root=/dev/ram0 rdinit=/init`; `/` and `/tmp`
are tmpfs. `/proc/mtd` contains only its header, `/sys/class/mtd` is empty,
and `/sys/class/ubi` contains only the framework's `version` attribute.
There are no MTD devices, attached UBI devices or flash-backed mounts.

`br-lan` has 192.168.0.1/24 with no default route. The 1G `lan1` port has
carrier at 1000 Mbps; the `lan2` RTL8261N driver binds, but that port has no
carrier in this sample. DHCP, DHCPv6, RA and NDP service settings are disabled;
the UDP socket tables show DNS listeners but no DHCP server ports 67/547.
The normal PON service remains disabled. `ponraw` is registered with the
native airoha_eth driver, administratively down with flags 0x1002 and no
address. No PON/controller/OMCI modules are loaded, and the deferred-device
list is empty. The boot log has no observed kernel warning splat or oops.

The live DT contains the bench marker and controller TX-inhibit property.
This establishes the intended policy, not the actual transmitter state:
the controller has not been loaded, powered, detected or sampled in this
session. The helper, common library and jshn shell library hashes match the
locally built files. The audited `q1000k-pon-bench status` command returns
`RAM bench preflight passed; no hardware changes made.`

All device commands were reads. No calibration/firmware was copied to the
device, no PON modules were loaded, no interface was changed, and no flash
operation occurred. At this baseline, the controller test still needed permission to
stage RAM files and change GPIO/I2C state, plus confirmation that the fiber
remains disconnected. The subsequent PHY/MAC/OMCI and shutdown/drain tests
remain pending. Local raw logs and a summary are saved under the bench
artifact directory's `read-only-boot/` directory; raw logs are kept private.

## First authorized controller test — 2026-09-14

With the fiber confirmed disconnected, the unit's validated 513-byte XGS
calibration and SHA-checked OEM PM/DM files were staged into the RAM filesystem.
The helper and controller module matched the inspected image. The controller
test returned `Controller did not bind` before detection or initialization:

```
pin gpio9 already requested by 0-0051; cannot claim for ...:521
q1000k-pon-control 0-0051: error -EINVAL: GPON enable GPIO
```

The GPIO offset/pin mapping is correct. The controller's default pinctrl
state first selects the GPIO mux and preloads both active-low power enables
inactive. AN7581 then rejects its GPIO descriptor request because the mux
function lacks the GPIO flag/classifier required by strict pinmux ownership.
GPIO9 corresponds to pinctrl pin 22 and the GPIO chip's offset 9 (global 521).

Patch `9999k-pinctrl-airoha-identify-an7581-gpio-function.patch` flags the
AN7581 GPIO function and exposes `pinmux_generic_function_is_gpio`. Strict
peripheral ownership and the board's output preload/mux sequencing are
preserved. A host regression compiles the production pinmux core, reproduces
the original ownership failure, permits the flagged GPIO mux and continues
to reject occupied GPIOs and conflicting peripheral mux owners.

The helper unloaded the controller after failure. Read-only diagnostics
confirm no PON modules remain, the bench preflight still passes and LAN SSH
is intact. No controller detection, firmware initialization or MCU/TX/LOS
sampling was reached. No PHY/MAC/OMCI module was loaded and no flash operation
occurred. Physical optical TX state remains unmeasured.

The fix changes the built-in pinctrl driver, so retry requires a newly built
bench FIT and another user RAM boot. The controller-test authorization carries
forward; do not expand it into stack activation. Test logs are retained in
the original bench artifact's `controller-test/` directory, with raw logs
private. This failed probe is not controller hardware acceptance.

## Replacement image for controller retry — 2026-09-14

Source `0e4acc9d70880ed4c3520c956822945ffaf165ea` builds successfully with the
AN7581 GPIO classification fix. Both changed pinctrl files cross-compile;
the AArch64 object contains the generic GPIO classifier reference. The kernel
patch passes checkpatch with no errors or warnings. All 88 PON/WAN/bench host
tests pass, and the pinmux regression also passes against the prepared kernel
sources. The existing builder/profile is unchanged. The full build retains
unrelated vendor and module metadata warnings.

The new `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`
is 7,602,176 bytes; SHA-256:
`0636ec23539a77f95856151f2b98e13bf2b5e53c7d6f52b33c333b81dcf03275`.
The artifact, manifest, resolved configuration, test/build logs, inspection
and checksums are under
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-0e4acc9d70`.

Offline inspection passes for both FIT hashes, the exact source revision and
all 1,154 embedded initramfs entries. NAND/PCS remain disabled, partitions and
rootdisk absent, controller TX inhibited, LAN/failsafe at 192.168.0.1, and
DHCP/RA plus automatic PON startup disabled. The ordinary development config,
protected branches, normal builder and user-owned README were preserved.

At the build checkpoint this replacement had not been booted. The next action
was another user RAM boot through second-stage `http-uboot-q1000k`, then read-only preflight and
the already authorized controller retry with fiber disconnected. No new
device test or flash operation was performed while building the fix.

## Controller retry passed — 2026-09-14

The user RAM-booted the replacement and supplied SSH at 192.168.0.1. The
running revision is `0e4acc9d70880ed4c3520c956822945ffaf165ea`, with Linux
6.18.44, RAM root, absent MTD/UBI devices and the expected helper/module hashes.
Preflight passes; DHCP/RA and PON service remain disabled and `ponraw` is down.
The prior controller-only permission and disconnected-fiber confirmation
apply to this retry. Unit calibration and verified OEM PM/DM files were
restaged only in RAM.

The controller now binds successfully. Both selected EN7573 paths report
family ID `0x1388` (5000); detection returns to off mode. XGS initialization
completes firmware/calibration readback and starts the MD32 with TX disabled.
The immediate initialized status and all five subsequent one-second samples
report `md32_enabled`, `firmware_verified`, `calibration_supplied`,
`tx_disabled`, `tx_inhibited` and `los` true, with `last_error=0`.
Initialized status reads sample MCU/TX registers and the LOS GPIO. The
`checked_uptime` field is the prior selection/ID check time, not each sample
time. This is a bounded controller smoke test, not an independent optical
measurement or sustained firmware/analog-health test.

The helper exits zero after power-off and unload. Read-only postchecks confirm
no PON modules, released I2C memory reservation at 0x50, unbound controller
at 0x51, removed bench lock and `ponraw` still down (flags 0x1002). LAN SSH
remains available at 192.168.0.1 and lan1 has carrier at 1000 Mbps. Preflight
still passes. The post-test kernel log exactly matches the pre-test baseline;
there are no new probe errors or other kernel messages.

The artifact's `controller-test/` directory contains a report, structured
status and private raw logs. No PHY/MAC/OMCI module was loaded and no firmware
was flashed. Calibration and OEM firmware remain in the RAM filesystem.

The next prepared action is the existing `stack` helper on this same image,
using `/tmp/q1000k-controller-test-0e4acc9d70/xgspon-calibration.bin`. It repeats
controller initialization, opens `ponraw`, loads BSP/PHY/MAC/OMCI with synthetic
bench identity, observes five samples, then unloads in reverse and closes
`ponraw`. This changes PHY/MAC/DMA state beyond the controller-only permission
and requires separate approval. No new image is needed for that test. Loaded
drain, optical registration, traffic/QoS and long-duration health remain
separate acceptance gates.

## First authorized stack test — 2026-09-14

On the same `0e4acc9d70` RAM boot, preflight and all 13 helper/library/module/CLI
hashes matched the inspected image. Staged calibration and OEM firmware
hashes matched. Controller initialization passed again with TX disabled and
LOS asserted. `ponraw` opened its native internal 10G link and the dependency
modules loaded, but `xpon_10g` failed before the observation loop.

The prepared OpenWrt ubox `main_modprobe()` accepts module names but never
forwards trailing command-line parameters. Consequently the launcher's
identity/lower arguments did not reach `xpon_10g`; its identity initializer
rejects missing parameters before MAC/PHY startup. Both the bench and optional
supervisor now use `insmod` for parameterized modules after loading their
explicit dependencies. The supervisor continues to suppress loader output
that could contain subscriber credentials. The fixture loaders now model
ubox's parameter behavior and check the complete argument sequence.

Cleanup unloaded the MAC/PHY/OMCI dependencies until SCU, then stopped as
designed: `airoha_ecnt_scu` reported zero references but `[permanent]` because
it had an init function and no exit function. Patch 051 adds an AN7581-only
exit that clears cached aliases to syscon-owned regmaps. This adapter owns
no IRQ, mapping, clock, reset or asynchronous work; its consumers pin the
module through exported symbols. Exit does not write hardware or destroy
the shared regmaps. The other SoCs' legacy lifecycle is unchanged.

After verifying all MAC/PHY/OMCI consumers were absent and the remaining
controller/hook had zero references, the authorized cleanup powered off and
unloaded the controller and hook, then closed `ponraw`. Controller off status
reported no error; `ponraw` is down (0x1002), LAN remains at 192.168.0.1 with
1G carrier, and preflight passes. Only the old SCU module remains; it was not
forced out. A user RAM reboot is required to replace it. No firmware was
flashed, and no optical registration or traffic was attempted.

The new SCU lifecycle regression covers successful initialization, all lookup
and read failures, unload, and reload while preserving syscon-owned data.
Offline image inspection now rejects any PON module with an init function
but no exit function. The old SCU binary is rejected; the other eight PON
modules pass, including the hook library with neither init nor exit.
Vendor release 62, bench release 2 and supervisor release 3 contain the fixes.
The raw attempt and containment logs are retained under this image's
`stack-test/` artifact directory. This attempt did not exercise full stack
startup or its physical MAC/PHY shutdown/drain path.

## Replacement image for stack retry — 2026-09-14

Source `cddd1983fa2ae8031da3802c43356102d18fdb96` completes the full cached
bench build with vendor r62, bench r2 and supervisor r3. All 89 host tests
pass. The SCU lifecycle fixture also passes against the prepared vendor
sources, and the compiled SCU module contains both init/exit callbacks and
the kernel's exit metadata. Patch 051 passes checkpatch without warnings.
The earlier permanent SCU binary is rejected by the new inspection guard.

The replacement `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`
is 7,602,176 bytes; SHA-256:
`266c76cb76abd655384dcd9384b02f31fa178ef26238d952933774b746350196`.
Its artifact directory is
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-cddd1983fa`.
It contains the image, manifest, build and test logs, resolved config,
inspection results and checksums.

Inspection verifies both FIT hashes, the embedded source revision, all 1,154
initramfs entries and unloadability of all nine PON modules. NAND/PCS stay
disabled, partitions/rootdisk are absent, TX is inhibited, LAN/failsafe use
192.168.0.1, and DHCP/RA plus automatic PON startup are disabled. The normal
development config, protected branches, normal builder and user-owned README
were preserved. Unrelated vendor build warnings remain.

This replacement has not been booted. The retained old SCU cannot be replaced
by a normal module unload, so the next action is a user RAM boot of this FIT,
followed by read-only preflight and the already-authorized stack retry. No
forced unload, reboot or firmware flash was performed during this work.

## Default IP changed to 192.168.255.1 — 2026-09-14

The user requested 192.168.255.1 as the default OpenWrt address. Source
`de0b571776d067c027138d333b33bcaeaebf6a24` and experimental builder
`42302b4c57a1f3aa6c68bd394b1e3fc3efc37a16` set the bench's normal LAN and
preinit/failsafe to 192.168.255.1/24, broadcast 192.168.255.255. Bench release
3 updates the LAN defaults and preflight guard. Existing tests and image
inspection use the same address; the guard rejects the previous subnet.

The seven bench tests, eight builder tests, full cached image build and
offline FIT/initramfs inspection pass. The image contains the expected new
address in preinit and normal LAN configuration and all nine unloadable PON
modules. NAND/TX/autostart policy and the prior stack fixes are preserved.
Normal development configuration, protected source branches, the normal
builder and the user-owned README were preserved. No device access or
runtime network configuration change occurred for this update.

The image is 7,602,176 bytes; SHA-256:
`a8521b0815fd43c9fda5d31916272f3d3cef10badeb9e38566b3afd551e4752b`.
The image, manifest, test/build logs, inspection and checksums are under
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-de0b571776`.
This image has not been booted. After user RAM boot, the new SSH target is
192.168.255.1; use a host address such as 192.168.255.2/24 while DHCP remains
disabled. The authorized stack retry remains the next hardware step.

## Stack retry at 192.168.255.1 — 2026-09-14

The user RAM-booted `de0b571776` and supplied SSH at 192.168.255.1. Read-only
preflight confirms RAM root, no MTD/UBI devices, immutable TX inhibit and all
13 helper/library/module/CLI hashes matching the inspected image. Verified
unit calibration and OEM PM/DM inputs were restaged only in RAM.

The authorized stack retry passes controller detection and firmware/calibration
readback again: both paths report 0x1388, MD32 is enabled, TX disabled/inhibited,
LOS asserted and last_error zero. Module parameters now reach the MAC. Startup
passes native attachment/protocol initialization but then returns EBUSY before
the observation loop. Automatic cleanup now unloads all nine PON modules,
including SCU. Controller/I2C reservations and the bench lock are released;
ponraw is down (0x1002), LAN retains 1G carrier and preflight still passes.
No flash, reboot or forced unload was performed.

WAN netdevice creation still requested new frame limits before physical drain.
The native guard correctly rejects that change. Vendor patch 052 removes this
premature Q1000K operation: the existing cold transaction already installs
60/2000-byte limits after drain, reset and epoch replacement. Other targets
retain their legacy setup. MAC initialization now names its failing stage
without printing identity. Vendor release 63 contains the changes.

All 90 host tests pass, including a regression where early frame configuration
returns EBUSY, legacy WAN rollback, the later drained frame-limit installation
and native rejection of undrained changes. Patch 052 passes checkpatch. The
artifact `bench-de0b571776/stack-test/` retains the attempt and cleanup logs.
Runtime validation of the fix remains next. Unlike the prior permanent-SCU
failure, complete cleanup allows a matching replacement module in RAM without
another boot. Full PHY/MAC/OMCI startup and physical drain remain unverified.

## Hook-framework panic and fix — 2026-09-14

The MAC module from `bc4fced17a` builds for the booted 6.18.44 kernel. Its five
vendor dependencies match the image byte-for-byte. With every PON module
unloaded, the verified replacement was staged only in RAM, retaining the
original module there. The existing authorized stack test was retried.

Controller initialization passes again, with TX disabled/inhibited and LOS
asserted. The serial trace confirms startup passes the former WAN failure
and registers both xPON and OMCI devices. It then panics in
`ecnt_register_hook+0xbc/0x138`, before PHY cold-start. The faulting address is
0x38 with x1 zero; matching disassembly shows `ldr w2,[x1,#56]`, the priority
read through an uninitialized hook-list entry. The framework contains an
`ecnt_hook_init()` function but had no module entry point calling it.

The kernel announces its own automatic reboot after the fatal exception.
No agent reboot, flash or forced unload occurred. Cleanup did not complete;
post-reboot state is unknown. The original SSH client eventually exits 255.
The trace and partial SSH capture are retained under
`bench-de0b571776/stack-test/retry-bc4fced17a/`. Do not retry the old hook
framework. No PHY cold-start or physical drain was reached by this attempt.

Vendor r64 connects the framework initialization to module load and supplies
an exit callback. Consumers pin the framework through exported symbols and
unregister/drain their callbacks before releasing it; the framework itself
owns only static storage. The lifecycle fixture now invokes the production
module init/exit instead of calling the core initialization helper manually.
It checks every list head, registration, concurrent RCU removal, node reuse
and reload. All 90 host tests pass. FIT inspection now requires both init
and exit callbacks for every PON module, rejecting the earlier hook binary.
A replacement RAM bench image is being prepared for a user boot and the
already-authorized stack retry. Hardware validation remains pending.

The complete cached image build at `4cefe6cd42` passes offline inspection;
all nine embedded PON modules now define init/exit callbacks. The compiled
hook module also has the kernel's `.init`/`.exit` metadata. Its updated
lifecycle fixture and checkpatch pass. The earlier image is rejected by the
stricter hook lifecycle check. The existing unrelated feed/Kconfig diagnostics
remain in the build log; resolved package selection and final build pass.

The final bench additionally sets `panic=0` in its DT boot arguments, and FIT
inspection checks this. Linux's explicit panic reboot is disabled so a fatal
test does not request a normal boot after three seconds. This does not alter
other reset sources, including a hardware watchdog. Read-only preflight on
the next user RAM boot must check `/proc/sys/kernel/panic` is zero. No running
device configuration was changed for this adjustment.

## Replacement RAM image ready — 2026-09-14

Source `f885e80846740f0a8d600e5c2bb2c6ebf575cc18` builds the complete final
RAM bench with vendor r64 and `panic=0`. The image is 7,602,176 bytes; SHA-256:
`c2c0e6e6f6b2e469c745eebada3cf792fbde1d41c7b09c545c9f228122d7125e`.
Artifacts, manifest, 90-test log, build logs, inspection, runtime binary hashes
and checksums are in
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-f885e80846`.

Inspection passes both FIT hashes, the exact source revision and all 1,154
embedded initramfs entries. Every PON module defines init/exit callbacks.
NAND stays disabled, rootdisk/partitions absent, TX inhibited, normal and
failsafe LAN at 192.168.255.1/24, and DHCP/RA plus PON autostart disabled.
The DT includes `panic=0`. The normal development configs, protected source
branches, normal builder and user-owned README are preserved.

This image has not been booted. The next step requires a user RAM boot through
the second-stage http-uboot-q1000k serial console at the established safe FIT
address 0x84000000, with fiber disconnected. Then perform read-only preflight
at 192.168.255.1 (including panic timeout zero), stage the verified private
inputs in RAM and retry the already-authorized stack test. No firmware flash
or additional device operation occurred while building either fix. Full
PHY/MAC/OMCI startup, physical drain and optical service remain unvalidated.

## r64 RAM retry and reusable scripts

The user booted `f885e80846740f0a8d600e5c2bb2c6ebf575cc18` at
192.168.255.1. The host serial capture is `/tmp/serial_output.log`.
The saved runner verified all 13 runtime hashes against the artifact manifest,
RAM root, no NAND/UBI devices, immutable TX inhibit and idle PON modules.
Its read-only capture is `bench-f885e80846/preflight-saved-script/` under
`build-artifacts/q1000k-xgspon/`; the stack capture is
`bench-f885e80846/stack-r64-01/`. Each has a `checkpoint.json` with absolute
paths, SSH output and the appended serial-log segment.

`/proc/cmdline` was `console=ttyS0,115200 earlycon root=/dev/ram0 rdinit=/init`:
U-Boot replaced the DT arguments. `/etc/sysctl.d/10-default.conf` also sets
`kernel.panic=3`. Before testing, the runner changed only the runtime sysctl to
zero and verified readback. Bench r4 packages a later sysctl override; the
helper checks the live value before mutations and offline image inspection
checks the effective shipped sysctl order. The earlier DT-only claim was
insufficient to establish the runtime setting.

Controller initialization passes: both IDs 0x1388, verified firmware/calibration,
MCU running, LOS true, TX disabled and inhibited. Hook registration and OMCI
registration now pass without a panic. At uptime 656 seconds, cold-start fails
with `-5` and the physical shutdown reports `-5`. All nine modules unload,
ponraw returns down, the lock and owned RAM inputs are removed, and LAN/SSH
remain healthy. Physical drain and full PHY/OMCI startup are still unverified.
No optical activation, firmware flash, force-unload or agent reboot occurred.

See [reusable script instructions](../../../scripts/q1000k/README.md).

## r65 physical failure localized

Checkpoint `ed4e42c64b0a199eb3c8db7687c9a2ae6bda8281` builds through
`scripts/q1000k/bench-build.py`: 97 host tests and FIT/initramfs inspection pass.
Normal configurations are restored and protected refs remain unchanged. The
7,602,176-byte image hash is
`a913b643617c74c2b786ee2eae47157ee9172486b0dc740be6ea5081cdc87403`.
It includes the effective userspace panic override as well as the DT setting.

The saved runner temporarily installed the verified MAC/provider diagnostic
modules on the same f885e80846 RAM kernel. Its source/kernel/dependency checks
passed. `bench-ed4e42c64b/stack-r65-01/` records CPU pause completion (stage 1),
then failed MPI RX stop: control `0x10000`, readback zero, expected completion
`0x40000000`. Containment's all-stop request also reads back zero. No FE channel
has been retired (`retired=0`). This failure precedes FE retirement and PHY
startup. All modules unload and original module hashes are restored; private
inputs are removed and LAN/SSH remain healthy. The reset/clock state needs
inspection before changing the sequence. Vendor r66 adds read-only probe
reporting for the SCU reset, WAN selector, local MAC reset and stop word.

A fixed-list read-only register capture was attempted; the image has no
`/dev/mem`, so it stopped at that guard. It did not read or write MMIO.

## r66 reset/WAN evidence and r67 cold handoff

The r66 image at `ef676230c858a4c3ac388466049e0e93ca4a1dcf` builds with
97 passing host tests and passing FIT/initramfs inspection; image SHA-256 is
`6baa98417ee1e16214642efcddff78c039d2904dd3fcfa7b896030e40df1fa8b`.
The module retry in `bench-ef676230c8/stack-r66-01/` reports
`scu-reset=0 wan=0x12 local-reset=0x0 stops=0x0`. The first MAC stop still fails
at stage 1, before FE retirement. Cleanup and original-module restoration
both pass, with LAN/SSH healthy.

`0x12` is PON-lane USXGMII. The local http-uboot Ethernet initializer writes
this value even for the separate copper path. Its source and proposed
follow-up are recorded in [the bootloader note](XGSPON-HTTP-UBOOT.q1000k.md).
No bootloader files were changed.

Vendor r67 adds a guarded cold handoff after native attachment/CPU pause:
from the known unconfigured USXGMII mode, acquire the verified optical
controller, confirm TX off and select XGS-PON through the masked SCU helper.
Existing XGS-PON mode is unchanged; unknown modes, an active/configured PHY,
invalid context, controller failure and failed mode writes are rejected.
The physical stop/drain checks still follow and are not bypassed.
The host suite passes 97 tests; the real Linux UML PHY suite also passes
50 lifecycle cycles with RCU/context checks and concurrent poll/IRQ teardown
(`/tmp/q1000k-pon-phy-uml.EhJprl/`). That is simulated hardware, not optical
acceptance. The native kernel is unchanged, so the saved runner can verify and
stage the matching PHY/MAC/provider modules together for a RAM-only retry.

The r67 image (`cff7bf6b24db8392c542b92061751509ceeac824`) builds and passes
inspection, hash `df3b8c3e5630a99b0f807102871356b279e1b7e523ca57aff34e99826091117a`.
The matching-module retry in `bench-cff7bf6b24/stack-r67-01/` confirms writable
stop controls: MPI RX request reads back `0x10000` but completion times out.
Containment reads `0x0101c101`: both MBI completions, neither MPI completion.
No FE retirement has begun. All modules unload, original hashes are restored,
private inputs removed, and LAN/SSH stay healthy. The WAN mode remains XGS-PON
in RAM; no persistent settings or firmware were written.

The new `resources` runner action uses only the hook, SCU and MAC resource
providers. Their probe maps/reads state without changing clocks or resets;
no controller/PHY/MAC startup or private inputs are involved. Its capture in
`bench-cff7bf6b24/resources-r67-01/` reads `wan=0xa local-reset=0x1` with the
same stop word. This rules out a held local MAC reset as the immediate cause.
Module cleanup/restoration pass. The host test checks reverse-order cleanup
on normal exit and failed intermediate loads.

## r68 cold PHY preparation

The imported vendor `gpon_init()` configures the PHY before waiting for MPI
stops. The native adaptation had placed all PHY preparation after that wait,
which cannot complete without the cold PHY's clocks. Vendor r68 separates
initial preparation from active-port retirement. After CPU/DMA pause and WAN
selection, an unconfigured PHY first gets a checked MPI RX stop request and
an acknowledged MBI RX stop. PHY configuration then verifies controller TX
off, initializes the PHY, and leaves IRQ/polling inactive. The existing full
MPI/FE/FIFO/RX drain must still pass before MAC reset or service-table/ID
replacement. An already configured PHY follows the existing retirement path.

The request-only API is restricted to holding MPI RX; it cannot release a
stop or claim completion. Tests distinguish request readback from stop ACK,
inject failures at each preparation boundary, and preserve all 39 retirement
failure cases and containment checks. Hardware validation is pending.

The r68 build at `cebd6ffe4b3fae514213c9708d1676e55e82162e` passes all 98 host
tests, image inspection and the real Linux UML PHY lifecycle test. Image hash:
`8d7fde82b173f19c427b36e02102ea82614754ffd3e7bf98e9602a5f94bb32ef`.
The matching-module run in `bench-cebd6ffe4b/stack-r68-01/` completes PHY
configuration with `Final phy_tx_enable mode=0`. MPI ingress stop now
acknowledges, and FE channels 0–15 retire (`retired=0xffff`). Channel 16 fails
with `-EIO`, leaving stage 2 (ingress stopped). All modules unload, original
RAM files are restored, private inputs are removed, and LAN/SSH remain healthy.
This is partial physical retirement, not completed drain or optical service.

## Native RX enable width correction

The existing native FE initializer writes TX `0xffffffff` and RX `0xffff`.
The retirement adaptation incorrectly requires `BIT(channel)` to read back
from both registers for all 32 TX channels. The failure at channel 16 is
consistent with this unsupported RX bit. Patch `9999l` masks temporary RX
enables to 16 bits, preserving all 32 TX retirements and exact readback checks.
It adds failing-register/expected/actual diagnostics for the next bench run.
RX activation also uses the hardware's 16-bit receive word after all table,
generation, DMA and ownership checks; the upstream T-CONT bitmap remains the
separate TX eligibility check. Upper TX channels are never aliased to a lower
RX bit. Fixtures now emulate the RX width and cover TX channel 31 activation.

This is native kernel code. It cannot be tested by replacing the three vendor
modules on f885e80846. Build and inspect a complete replacement bench, then
have the user RAM-load it from the second-stage http-uboot. The old bench is
left idle; no flash, reboot, force-unload or optical activation is authorized.

## Replacement image ready: d928eb20b6

The saved builder completed source `d928eb20b6673329d76da024ac6a122798921a8b`
with vendor r68 and native RX-width patch `9999l`. All 98 host tests pass.
The actual patched native sources match those tested in Linux UML: 100
transport cycles pass, including channel 31, RCU/IRQ control, late DMA,
pause-timeout retry and RX generation checks. No kernel diagnostics were
reported by that guest. The real PHY UML suite also passed at r68.

Artifact directory (workspace-relative):
`build-artifacts/q1000k-xgspon/bench-d928eb20b6/`.
Image: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`.
Size: 7,602,176 bytes. SHA-256:
`577e20c0ae5866e05c64e713f41f7589953c161da1d0c4c3b25bb174eb04b141`.
`checkpoint.json` records absolute paths, source/builder revisions, normal
config hashes, unchanged protected refs and successful restoration.
`inspection.json`, `host-tests.log` and `uml-transport/` retain validation.

Inspection confirms NAND disabled, immutable TX inhibit, management/failsafe
192.168.255.1, no PON autostart, and packaged panic timeout zero. The live
panic timeout must still pass readback. The user must RAM-load this FIT using
the second-stage http-uboot and keep fiber disconnected. After SSH returns,
run saved `bench-run.py status` using this artifact, then the already-authorized
stack test with the verified private inputs. Do not use `--modules-from` to
put this change on f885e80846: the native kernel changed. This image has not
been booted or flashed, and complete physical drain remains unverified.

## d928eb20b6 hardware result and drained QoS correction

The user RAM-booted this image. `bench-d928eb20b6/preflight-01/` confirms the
exact revision and 13 runtime hashes, RAM root, absent NAND/UBI, immutable TX
inhibit, idle PON stack and live panic timeout zero. The authorized run in
`bench-d928eb20b6/stack-r68-01/` uses the saved runner and verified private
inputs with fiber disconnected. Serial capture starts at byte 44942 of the
unmodified host `/tmp/serial_output.log`.

Controller startup and cold PHY preparation pass. With RX-width patch
`9999l`, all 32 FE channels retire and the checked MAC stops, alignment FIFO,
native RX DMA drain, PHY quiesce and MAC reset complete. At uptime about
138 seconds the transaction fails at stage 8 (tables changing), error `-108`
(`-ESHUTDOWN`), containment zero. This is the QoS snapshot at the start of the
clear callback: native FE retirement has set every channel's retiring bit,
which the original scheduler guard rejects. Namespace replacement has not
completed. All nine PON modules unload, private inputs are removed, ponraw
returns down and LAN/SSH remain healthy. No flash or optical TX activation
occurred. This is idle physical-drain evidence, not loaded or optical service.

Patch `9999m` allows reads of retired schedulers only after complete native
drain, while preserving all write and epoch guards. The focused host fixture
covers all channels, each missing drain precondition, fault propagation and
unchanged outputs on error. The real Linux UML transport test now snapshots
all 32 schedulers between RX drain and epoch reset, rejects premature reads
and writes, then verifies replay after reset. It passes 100 lifecycle cycles
without kernel diagnostics in `/tmp/q1000k-pon-transport-uml.6dAU5Z/`.
The kernel must be rebuilt and RAM-booted by the user before retrying the
stack. Matching vendor-module substitution cannot change this native code.

## Replacement image ready: 30ea573aa3

The saved builder completed `30ea573aa347f7c766406e9baee294091b0180ff` with vendor r68 and native
patch `9999m`. All 98 host tests pass against the prepared build sources.
FIT payload hashes and the extracted root filesystem pass inspection.
The native UML suite passes 100 lifecycle cycles; its generated test source
matches the prepared driver exactly. Logs and the source digest are saved in
`uml-transport/` beside the image.

Artifact directory (workspace-relative):
`build-artifacts/q1000k-xgspon/bench-30ea573aa3/`.
Image: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`.
Size: 7,602,176 bytes. SHA-256:
`aeb6353084dea4d3b0781f21b62e23a5670e6364bcd6fa8fb0424ff94d081880`.
`checkpoint.json` records absolute paths, source/builder revisions, image hash,
unchanged protected refs and successful normal-config restoration.

Inspection confirms NAND disabled, immutable TX inhibit, management/failsafe
192.168.255.1, disabled PON autostart and packaged panic timeout zero. No new
device writes followed the failed stack test and its successful cleanup.
The image has not been booted or flashed. RAM-load it through second-stage
http-uboot with fiber disconnected, then use the saved runner's read-only
`status` preflight against `bench-30ea573aa3`. Once the revision and runtime
guards pass, the existing controller/stack authorization covers the next
bounded test with verified private inputs. Do not substitute vendor modules
on d928eb20b6: this fix changes the native kernel. Do not use an HTTP recovery
firmware upload, persistent environment write or flash operation.

## Complete disconnected-fiber stack pass: 30ea573aa3

The user RAM-booted this FIT. Saved `bench-run.py status` verifies the exact
revision and all 13 runtime file hashes, RAM root, absent NAND/UBI, immutable
TX inhibit, live panic timeout zero and idle modules at 192.168.255.1.
The existing runtime authorization covers the two bounded stack tests; fiber
remains disconnected. Neither test changes firmware or persistent settings.

Captures are under `build-artifacts/q1000k-xgspon/bench-30ea573aa3/`:

| Capture | Serial start byte | Hardware path | Result |
| --- | --- | --- | --- |
| `preflight-01/` | 68700 | Read-only revision/storage/network/module checks | Pass |
| `stack-r68-01/` | 68700 | First startup from WAN 0x12, then shutdown | Pass, 25.138 seconds total |
| `stack-r68-02/` | 71170 | Reattachment from WAN 0x0a, then shutdown | Pass, 24.888 seconds total |

Each stack run verifies controller IDs 0x1388, private firmware/calibration,
MCU enabled, TX inhibited/disabled and LOS. The status sysfs read samples MCU,
TX and LOS live; `checked_uptime` remains the earlier controller selection
stamp and is not a fresh-sample timestamp. Startup completes all 32 native
FE retirements, MAC stop acknowledgments, alignment FIFO and native RX DMA
drain, PHY quiesce, MAC reset, full scheduler snapshot, table clear, epoch
replacement and cold namespace setup. Only channel zero's scheduler is
replayed for this unprovisioned namespace; this does not validate traffic
scheduling on every subscriber channel.

The MAC reaches its running receive-only state. All five observations in
both runs report `protocol_error=0`, state O1, an enabled but non-operational
OMCI agent, no ONU or OMCC assignment (`65535`), 334 initial MIB objects,
`service_error=0`, and zero optical packet/error counters. These are local
initial MIB objects, not evidence of OLT provisioning. The controller remains
TX-disabled with LOS throughout the samples. Both cleanup paths perform the
checked physical shutdown, unload all nine modules, remove private inputs,
and leave ponraw down with management LAN/SSH healthy. Neither serial interval
contains a panic, warning or PON failure diagnostic.

The second run specifically checks reuse after a *successful* activation and
shutdown, which previous failed-start retries did not establish. No further
repetition is needed for that bounded check. `stack-results.json` preserves
the aggregate validation, generated by the reusable read-only command:

```sh
python3 scripts/q1000k/bench-report.py ../build-artifacts/q1000k-xgspon/bench-30ea573aa3/stack-r68-01 ../build-artifacts/q1000k-xgspon/bench-30ea573aa3/stack-r68-02
```

The reporter accepts both complete captures and rejects the known failed
`bench-d928eb20b6/stack-r68-01` capture. No driver or image change was needed
in this continuation. The existing FIT remains the validated RAM bench.

The disconnected-fiber startup/shutdown gate is complete. It does not cover
long-duration health, an occupied optical pipeline, OLT registration/ranging,
actual OMCI provisioning, encrypted subscriber traffic or DHCP. Those require
a separately prepared and explicitly authorized optical test, the subscriber's
own provisioning identity and a suitable RAM image. Do not remove TX inhibit
from this bench, connect fiber for this helper, or treat these results as
flash/production readiness. Firmware flashing remains forbidden.

## Receive-only preparation and fiber-state LEDs (2026-09-14)

The user authorized preparation of the next receive-only optical test, and
reported that the fiber-state LED was inactive. OEM DTS `pon_lnk_green` and
`pon_lnk_red` identify GPIO22 and GPIO30 (both active high). They already exist
as `green:wan-1` and `red:wan`; a read-only check of the running `30ea573aa3`
image confirms brightness zero and no selected trigger on both. The MAC DT
now references them. The core releases LED references on all unwind/unregister
paths, cancels software blinking before steady brightness, and blanks the
indicator on release. A cancellable backend observer reports fresh PHY LOS
once per second, independently of the fixed 10G CPU carrier. Optical signal
with discovery/registration blinks green; operational is steady green; LOS or
unavailable PHY is red. GPIO24 is the distinct OEM activity LED and is not
substituted for the fiber-state indicator. Visual confirmation remains pending.

Vendor r70/controller r6/bench r5 add explicit `rx_bench=1` startup. The MAC
checks the RAM bench DT, the PHY checks immutable controller TX inhibit and
live TX off, and an already configured PHY cannot switch mode. The normal
protocol worker never starts; its IRQ and all MAC sources stay masked and
external readiness remains false. PHY IRQ/poll paths bypass legacy callbacks
and acknowledge only the selected RX events. Fresh read-only snapshots include
controller/PHY LOS, sync, frame/LOF/FEC counters, IRQ/poll counts and boot-time
sample timestamps. Unexpected TX enable fails and contains the PHY.

The helper and host runner have a dedicated `receive` command with an explicit
fiber state. A 30-second dark run must pass before connecting fiber for a new
run. A connected run additionally requires five consecutive final intervals
with LOS clear, sync and changing frame counts. Both retain the checked cold
pipeline/drain and complete reverse cleanup. They use only synthetic bench
identity; no registration or subscriber traffic is attempted. The existing
normal stack test remains disconnected-fiber only. This change requires a new
user RAM boot, because it changes the controller/core and DT; the old-image
module substitution guard deliberately refuses that combination.

Host fixtures cover mode/inhibit guards, read failure without partial sample
publication, RX IRQ W1C ownership, callback suppression, TX refusal, MAC masks,
registration-start suppression, connected/dark helper decisions and cleanup.
Linux UML passes 50 normal and 50 RX-only lifecycle cycles with real mutexes,
workqueues and concurrent IRQ teardown; artifacts are in
`/tmp/q1000k-pon-phy-uml.uNDXMz`. An earlier UML attempt exposed a fixture error:
it tried to inject a pending interrupt by writing its new W1C register model.
Direct modeled hardware injection fixes that test; no device was involved.
The full OMCI core UML suite also passes at
`/tmp/q1000k-omci-core-uml.y9bPZ1`. Build/image inspection and new hardware
acceptance are still pending here.


The initial combined image at `c08aec4bd1` builds and passes 107 host tests
and FIT/rootfs inspection. It is superseded for device use: final review found
that module parameter attributes can be read during initialization, when
native attachment alone does not guarantee the optical WAN selector/clocks.
Vendor r71 moves the verified active-PHY sample before the MAC mask read.
Its fault-injection test proves that unconfigured or failed PHY status causes
no MAC register access. This does not change the PHY/core implementations
covered by the saved UML evidence. A final image is built from the follow-up
checkpoint before requesting a user RAM boot.


## Final receive-only RAM image ready (2026-09-14)

Source checkpoint `fbd26fb10a74f6e503a1397458950e2357c12c58` builds with vendor
r71, core r14, controller r6 and bench helper r5. All 107 host tests pass against
the actual prepared build sources. FIT/kernel/DT hashes and embedded rootfs
inspection pass, including the named fiber LED references to GPIO22 and GPIO30,
TX inhibit, disabled NAND with no partitions, 192.168.255.1, disabled DHCP/PON
services and the late zero panic timeout. The runtime timeout is still read
back by the runner before tests. Normal `.config` and `.config.old` hashes and
all protected branch refs are unchanged; the user's untracked README remains
untouched. No device writes or flash operations were performed for this update.

Artifact directory:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-fbd26fb10a/`

- FIT: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`
- Size: 7,602,176 bytes
- SHA256: `e443bf73e5afa7864e623f5b7036b41b985dbff7226585495888cdefa2006930`
- `checkpoint.json`, `inspection.json`, `host-tests.log`, `runtime-sha256sums`
  and `sha256sums` record the complete build and inspected runtime payloads.
- `uml-phy/`, `uml-core/` and `uml-source-verification.json` retain the passing
  Linux UML evidence and confirm the production source matches. The saved
  `scripts/q1000k/bench-record-uml.py` verifies/reuses these captures, including
  reconstructing the two intentionally instrumented OMCI files. Its invocation
  against this artifact succeeds and preserves existing evidence byte-for-byte.

Next device sequence: user RAM boot from second-stage http-uboot with fiber
still disconnected; read-only preflight; normal stack test to check the LED
observer and teardown; 30-second `receive --fiber-disconnected` test; only then
user-confirmed connection and `receive --fiber-connected`. The fiber indicator
is expected red while the disconnected stack runs and off after release.
GPIO class readings cannot establish physical illumination; user visual
confirmation remains required. The bench intentionally leaves the stack
unloaded at boot, so it has no live optical status to display until testing.
The synthetic identity remains unassigned and no optical TX is requested.

## Identity-capable receive bench

The identity-capable image includes vendor r72, core r15 (Generic Netlink v16), OMCI CLI
r2, diagnostics r6, supervisor r4, bench helper r6 and LuCI r5. All identity
controls from the 8311 PON page are exposed in LuCI and `q1000k-omci config`.
The [CLI contract](../../../package/network/utils/q1000k-omci-tools/README.md)
explains each field, defaults, secret handling and runtime readback.

Both launchers validate staged values before loading any module. The normal
supervisor forwards the subscriber serial/registration. The bounded bench
retains `TEST00000001`, its synthetic WAN MAC and zero registration ID, while
applying the configured OMCI presentation (equipment, vendor, hardware,
software, banks and logical credentials). This permits local inspection
without transmitting subscriber identity. Empty vendor derives from the
synthetic serial in this bench; it derives from the real serial in the normal
supervisor. Staged configuration itself remains visible in LuCI/CLI even
when the stack is unloaded.

The connected-fiber acceptance sequence remains:

1. User RAM boots the newly built FIT through second-stage http-uboot. Keep
   fiber disconnected initially. Management is `192.168.255.1`, DHCP disabled.
2. Read-only preflight verifies the image revision/runtime hashes, NAND/UBI
   absence, TX inhibit, idle modules and unenslaved/down `ponraw`.
3. Run the disconnected stack test and confirm the physical fiber LED is red
   during LOS, then off after teardown. Readbacks alone cannot prove the LED
   is physically illuminated.
4. Run `receive --fiber-disconnected`, which must retain LOS, O1/unassigned,
   no registration, zero MAC interrupt mask and TX disabled throughout.
5. After the user connects the fiber, run `receive --fiber-connected`. For
   30 seconds collect fresh controller/PHY LOS, receive synchronization,
   frame/FEC/LOF counters, protocol state and LED class readings. Pass requires
   five final consecutive samples with both LOS indications clear, sync
   acquired and a progressing frame counter, while all transmit guards hold.
   The fiber LED should indicate received light/discovery, not O5 service.
6. Reverse teardown must leave every owned module released, controller off,
   and `ponraw` down. Retained modules, stale samples or uncertain cleanup
   fail the run; do not force-unload or automatically reboot.

Use the saved `scripts/q1000k/bench-run.py` for the exact verified artifact:
`stack --fiber-disconnected`, then `receive --fiber-disconnected`, then
`receive --fiber-connected`, each with `--artifact`, a separate `--output`,
and the private `--inputs` archive. The wrapper verifies fourteen immutable
runtime files, including the staged configuration CLI helper, before testing.
Configuration can be inspected using `q1000k-omci config list` (secrets
redacted) and `config validate`; it is not auto-applied to the running stack.

This is a receive-only connected-fiber test. O5 registration, PLOAM/OMCI TX,
AT&T authentication, encrypted traffic and DHCP require a later explicitly
authorized test with a different transmit policy. This image cannot enable
optical TX through CLI or LuCI settings. No flashing is part of the sequence.

### Verified identity image — 2026-09-14

Image source checkpoint: `d84282fc8618aeebaeeef233c33a690c09dc1c6d`.
Artifact directory:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-d84282fc86/`.

- FIT: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`
- Size: 7,602,176 bytes.
- SHA256: `56c9131d879b242f1d20d5ba20f414225638dc92836982e25c1ecfc057d406b3`.
- Native build, 110 PON host tests, 13 diagnostics tests and FIT/DT/rootfs
  inspection pass. `identity-inspection.json` additionally verifies the
  embedded CLI helper, supervisor and common library against source, all LuCI
  fields, the identity file's 0600 mode and the MAC's new module parameters.
- `uml-core/` records the passing real-kernel cold identity, independent
  software/bank fields, logical credential GET/reset/redaction and single-PPTP
  model checks. `uml-phy/` retains the unchanged, previously tested PHY;
  `uml-source-verification.json` confirms exact production-source equivalence
  for both UML runs. There is no claim of a new physical-device test here.
- `runtime-sha256sums` records the fourteen required runtime files;
  `checkpoint.json` records restored normal configs and unchanged protected
  branches. The unrelated user README remains unchanged.

A read-only SSH check observed the device still running source `30ea573aa3`
(kernel 6.18.44). Only kernel/build/board identity was read. The new FIT has
not been booted or flashed. Physical LED illumination, dark receive and
connected receive acceptance require the user's next RAM boot. No TX,
registration or DHCP test was performed.

### Identity image: disconnected hardware acceptance — 2026-09-14

The user RAM-booted `d84282fc8618aeebaeeef233c33a690c09dc1c6d` and confirmed
fiber disconnected. The saved runner verified all fourteen runtime hashes,
RAM root, disabled NAND/no MTD or UBI, immutable TX inhibit, management
`192.168.255.1`, disabled DHCP/PON services and runtime panic timeout zero.
Both the five-sample normal stack run and thirty-sample receive-only dark run
passed, including reverse module teardown, `ponraw` down and removal of only
the owned, hash-verified private RAM inputs. Management remained available.

Captures under `/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/`:

- `bench-d84282fc86-stack-20260914-01/`: five OMCI samples, six initialized
  controller samples; both EN7573 controllers detected, verified firmware and
  calibration, TX disabled/inhibited, LOS true, O1/unassigned, 322 MIB objects,
  no service or protocol error. Elapsed including setup/cleanup: 25.195 s.
- `bench-d84282fc86-receive-dark-20260914-01/`: thirty fresh RX samples from
  boot milliseconds 355663 to 386884, polling calls 0 to 20; both LOS sources
  true, no sync, frame/FEC/LOF counters zero, registration disabled, MAC IRQ
  mask zero and TX disabled. Elapsed including setup/cleanup: 52.171 s.
- Each contains baseline/attempt/postflight/cleanup, checkpoint and new serial
  output. No kernel failure diagnostics occurred. The receive directory's
  `report.json` independently validates both captures with the saved
  `scripts/q1000k/bench-report.py`; its four host tests cover incomplete
  evidence, bad guards, stale counters, connected final stability, counter
  wrap, kernel errors and failed cleanup. It performs no device operations.

Every LED sample was green=0/red=1. The user visually confirmed **red during
the test, off afterward**. Read-only idle brightness was zero for both LEDs.
This verifies physical LOS indication and teardown; green indication on
received light and operational service remain separate acceptance checks.

Read-only `q1000k-omci config list` and `config validate` succeeded on the
actual bench: all seventeen keys are exposed, subscriber identity is unset,
`sync_circuit_pack=1`, `mib_profile=native-pptp`, `fix_vlans=0`. No staged
configuration was changed. The optional final file-mode check in that SSH
command found `stat` absent; the preceding CLI and LED reads succeeded and
image inspection already records the identity file's 0600 mode. This did not
affect either hardware test.

No flash, optical TX, registration, authentication or subscriber traffic was
attempted. Connected receive acceptance follows only after the user's explicit
fiber-connected confirmation.

### First connected receive capture — 2026-09-14

After the user confirmed fiber connected, the same immutable image completed
all thirty observations but **failed downstream acceptance**: controller LOS
and PHY LOS stayed true, sync false, frames/FEC/LOF zero throughout. Polling
advanced 0 to 20, timestamps advanced 1675983 to 1707196 ms, O1/ONU-ID and
OMCC-ID 65535 remained unchanged, and all TX/registration/IRQ guards held.
All LED samples were red=1/green=0. No kernel failure diagnostics occurred;
postflight and owned-input cleanup passed, management remained available,
all nine modules were released and `ponraw` returned down. There is no O5 or
subscriber-service acceptance. Capture:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-d84282fc86-receive-connected-20260914-01/`.

The first host launch was rejected before execution because automatic approval
review timed out. Its permitted identical retry started the captured test;
there was no duplicate hardware run.

Local investigation compares QKX001-06.00.44.00's `en7572.ko` and
`etc/init.d/xponconfig` with the pinned public loader. The OEM binary accesses
MD32 control/address/data at 0x50; the public reference and current loader
access MD32 control/address at 0x51, data at 0x50. Whether these addresses alias
on this part has not been established. MCU-enable readback alone does not
prove firmware execution. Do not change routing or APD analog values based
only on LOS; first add serialized, read-only receiver/control observations.

### Historical paused checkpoint: receiver diagnostics prepared, not built

The user requested a pause after the first connected-fiber capture. Controller
r7 and bench helper r7 source now add a serialized, read-only `receiver_status`
snapshot before PHY startup and at every observation: both MD32 enable address
views, APD/OCP controls, firmware status word and LOS/system controls. No
loader routing, analog setting, TX policy or registration behavior changes.
The portable controller tests pass, including all 15,388 loader I2C failure
points and new receiver read/error/partial-publication checks. `git diff
--check` passes. The updated shell bench fixture, full target build and image
inspection have **not** run; no r7 image exists and no r7 code was installed
on the device. Resume with these local checks before preparing a new RAM FIT.

The device was left after successful reverse cleanup of the failed connected
receive observation: all PON modules unloaded, controller powered off,
`ponraw` down, private staged inputs removed and management available. The
last user-confirmed fiber state is connected. Red LOS indication and off
after teardown were visually confirmed for the disconnected test; connected
LED observation and confirmation of the exact incoming-fiber path remain
unanswered. No additional test is scheduled or running. Nothing was flashed.

### Connected retest preparation — 2026-09-15

The user reported that the fiber was not firmly seated during the previous
connected test and requested preparation for a retry. That observation means
the earlier no-light result cannot establish a receiver-initialization fault.
Repeat the receive-only check on the unchanged `d84282fc86` image before
changing loader behavior or deploying the pending r7 diagnostic additions.
The FIT hash still matches
`56c9131d879b242f1d20d5ba20f414225638dc92836982e25c1ecfc057d406b3`.

The temporary private input archive had disappeared. The saved
`scripts/q1000k/bench-inputs.py` recreated it at
`/tmp/q1000k-controller-test-1093614699-inputs.tar`, with mode 0600 and every
input checked against the runner's known size/hash manifest. No private data
was read from or copied to the device. `/tmp/serial_output.log` was initially
absent and then became available with a new serial-capture session. An initial
read-only SSH connection to `192.168.255.1` was refused; that is not evidence
that the booted image is ready. The retest requires a successful exact-image
preflight and confirmation that the incoming fiber is firmly seated.

The serial log subsequently showed a fresh boot. Read-only preflight capture
`bench-d84282fc86-retest-ready-20260915-02/` passed: the running source is
exactly `d84282fc8618aeebaeeef233c33a690c09dc1c6d`, all fourteen runtime files
match, storage/TX/service guards pass and the PON stack is idle. The earlier
`...-01/` capture retains the connection failure during startup. Preparation
is complete; no controller/module operation or connected test ran during
preparation. Confirm the incoming fiber is firmly seated before invoking
`receive --fiber-connected` with a fresh capture directory. The private input
archive and saved runner are ready; no new image or module substitution is
needed to repeat the previous observation.

### Connected retest: light detected, synchronization absent — 2026-09-15

After the user confirmed the bench up at `192.168.255.1` and fiber connected,
the unchanged `d84282fc86` image completed another thirty-sample receive-only
test. Capture:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-d84282fc86-receive-connected-20260915-01/`.

Both controller and PHY LOS were **false in every sample**, unlike the first
connected attempt. The user's loose-connector report explains that earlier
LOS result; current receiver initialization detects light without a firmware
or loader change. However, `synced=false`, `sync_status=0`, frame count zero
and FEC/LOF counters zero persisted throughout. RX polling advanced 0 to 20;
RX IRQ count remained zero. The helper correctly failed its final downstream
stability requirement. Optical reception of valid frames is not yet verified.

All samples retained TX disabled/inhibited, registration disabled, zero MAC
interrupt mask, O1 with ONU/OMCC IDs 65535, 322 MIB objects, no protocol or
service error, and zero OMCI traffic. Red LED brightness stayed zero; green
sampled both blink phases (14 on, 16 off). Physical connected-fiber LED
confirmation is still pending. Serial output contains no kernel failure
diagnostic. Total elapsed setup/observation/cleanup was 52.557 seconds.

Postflight and owned-input cleanup passed: every PON module released,
controller off, `ponraw` down, private staged files removed and management
available. No flash, reboot, optical TX, registration or subscriber traffic
was performed. The next software investigation should focus on the AN7581
RX synchronization path and its receive-only startup/poll handling with LOS
clear, rather than treating the controller address discrepancy as the cause
of missing light. No unproven routing or analog change was tested.

### Connected LED observation confirmed — 2026-09-15

The user missed the first visual observation and explicitly requested another
run. `bench-d84282fc86-receive-connected-20260915-02/` records the same bounded
receive-only test on the unchanged image. All thirty samples again detected
light at both LOS sources but no synchronization or frames. Registration,
MAC interrupts and optical TX remained disabled; the downstream acceptance
check failed, while reverse cleanup and private-input removal passed. The
serial interval contains no kernel failure diagnostic.

The user first reported the indicator off, then corrected the observation:
**it started blinking green**. This confirms physical green acquisition
indication. The initial off observation may have occurred during controller
setup; it does not justify changing the GPIO mapping. No LED/GPIO control
was altered. The read-only `led-idle.log` subsequently confirms both link
brightness values zero, all nine PON modules absent and `ponraw` down. Together
with the earlier disconnected red indication, both link colors now have user
visual confirmation. Blinking green indicates received light/discovery only;
it does not establish downstream frame sync, O5 registration or service.

### Receiver acquisition diagnostic checkpoint — 2026-09-15

The imported AN7581 `en7581_xgpon_phy_event_poll()` has a no-LOS/no-ready
recovery path: `pma_no_los_no_ready_reset()` performs `PLUG_OUT` followed by
the PMA reset path, and every tenth unsuccessful cycle can reset the optical
SCU. RX bench deliberately bypasses that polling handler because it also
dispatches registration events. Missing reacquisition is therefore a possible
explanation for the light/no-sync observations, not a demonstrated root cause.
The initial board profile and PMA rate settings match the OEM boot log; the
connected captures do not yet distinguish controller output from CDR/PCS state.
Do not enable the complete vendor poll handler to investigate this.

Vendor r73 now adds a `receiver` object to the existing guarded RX sample.
The observations use the owned PHY provider and callback mutex, require the
immutable controller TX inhibit and live TX-off state, and publish only when
every read succeeds. No reset, debug probe selection, analog write, counter
clear or registration callback is added. The following raw words are sampled:

| Field | AN7581 physical address | Reference register |
| --- | --- | --- |
| `rx_control` | `0x1faf0a04` | XG_PON_RX_SYNC_CTRL |
| `pcs_reset` | `0x1faf0a0c` | XG_PHY_RST_N |
| `pma_reset` | `0x1fa8b460` | SW_RST_SET |
| `clock_control` | `0x1fa8b450` | PON_CK_SET |
| `cdr_control` | `0x1fa8b818` | rg_force_da_pxp_cdr_lpf_lck2data |
| `rx_frequency` | `0x1fa8b530` | RO_RX_FREQDET |
| `pll_status` | `0x1fa8b420` | ADD_LCPLL_RO_1 |
| `tdc_control` | `0x1fa8b010` | SS_LCPLL_TDC_PW_0 |
| `rx_analog0` / `1` / `2` | `0x1fa8b424` / `428` / `42c` | ADD_RO_RX2ANA_1 / 2 / 3 |
| `rx_sequence_force` | `0x1fa8b114` | RX_CTRL_SEQUENCE_FORCE_CTRL_1 |
| `rx_sequence_disable` | `0x1fa8b10c` | RX_CTRL_SEQUENCE_DISB_CTRL_1 |

The reference `freq_check()` interprets `rx_frequency` bit 0 as FBCK lock,
and `cdr_control` bit 8 as forced CDR selection with bit 0 selecting data
versus reference lock. `rx_control` bit 16 enables the PCS receiver. Keep the
raw values alongside these interpretations: they are not an independently
measured frequency or proof of frame reception. Controller r7's separately
serialized seven-word snapshot observes its two MCU address views, APD/OCP,
firmware, LOS and system controls before PHY startup and at every sample.
The address discrepancy remains an observation target, not a loader change.

Focused host validation passes: read-only field publication, every RX MMIO
read failure, all-ones receiver words, partial-sample rejection, fail-closed
TX/callback handling, JSON field encoding, all 15,388 controller loader I2C
failure points, and all fifteen shell bench tests including missing controller
diagnostics. The PHY UML fixture now checks receiver values across fifty
receive-only start/IRQ/poll/stop cycles. A full local build, image inspection
and the UML execution are still pending at this source checkpoint.

Next hardware action after those checks is a user RAM boot of the new FIT
through second-stage http-uboot. Repeat the already-authorized connected
receive-only observation at `192.168.255.1` with the exact new artifact and
saved private inputs. The earlier disconnected startup/cleanup and both LED
colors have passed; these additions only read status. Keep TX, registration,
DHCP and persistent storage disabled. Use the new snapshots to select a
specific correction before introducing any receive recovery. This checkpoint
has not accessed or modified the device and does not claim sync is fixed.

### Receiver diagnostic RAM image ready — 2026-09-15

Image source: `e26854f308a5204169173e90e309f18d7fca7dea`.
Artifact directory:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-e26854f308/`.
FIT: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`,
7,602,176 bytes; SHA256
`06596da17cbf23c31e44cbd6bfdc255baf5cfe081415e0d7f9008a06b823d435`.

The saved builder completed the full build, **116 host PON tests**, and FIT,
embedded kernel/initramfs and DT inspection. The package manifest confirms
vendor r73, controller r7, bench helper r7, unchanged OMCI core r15 and CLI r2.
Inspection confirms NAND disabled, immutable TX inhibit, management address
`192.168.255.1`, panic timeout zero, no PON autoload, unloadable module
lifecycles and the identity CLI/LuCI fields. The fourteen runtime file hashes
and runtime copies are saved beside the FIT. Normal `.config` and
`.config.old`, all three protected branch refs, the user's untracked board
README and the normal builder are preserved. The cached feeds emit the same
unrelated unselected-package dependency warnings as the earlier image.

The new PHY UML run at `/tmp/q1000k-pon-phy-uml.H5AUg5/` passed fifty normal
and fifty RX-only cycles with real kernel locking, RCU/context guards and
concurrent poll/IRQ teardown. Its generated source matches this checkpoint;
`bench-record-uml.py --phy-only` saved `uml-phy/` and
`uml-phy-source-verification.json` in the artifact. No new OMCI core run is
claimed: the core and its fixtures are unchanged from the tested `d84282fc86`
image, whose independent evidence remains in that artifact directory.

This image is ready for the next **RAM-only connected receive test**, not a
flashable/service-validated PON release. The currently booted image is still
`d84282fc86`; do not bypass exact-image guards or substitute the new controller
module into it. After the user boots the new FIT through second-stage
http-uboot and reports SSH ready, use the existing runner with a new capture:

```sh
python3 scripts/q1000k/bench-run.py receive \
  --artifact ../build-artifacts/q1000k-xgspon/bench-e26854f308 \
  --output ../build-artifacts/q1000k-xgspon/bench-e26854f308-receive-connected-01 \
  --inputs /tmp/q1000k-controller-test-1093614699-inputs.tar \
  --fiber-connected --serial-log /tmp/serial_output.log
```

Use the last confirmed connected fiber state unless the user changes it.
The runner stages verified inputs in RAM, collects thirty samples with the
new diagnostics, and performs bounded reverse cleanup. A failed downstream
stability check must remain a failed test even if light and LED behavior are
correct. No device access, flash, reboot, transmit activation or registration
was performed while preparing this image.

### Connected receiver diagnostics captured — 2026-09-15

The user RAM-booted the new image and reported the bench ready. The guarded
connected receive run verified source `e26854f308`, all fourteen runtime
hashes and the RAM/storage/TX guards before staging inputs. Capture:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-e26854f308-receive-connected-01/`.
It contains thirty RX and thirty-one controller snapshots and completed in
52.903 seconds. Both LOS signals were false throughout, but synchronization
stayed in HUNT (`0`), with no frames, FEC/LOF counts or RX IRQs. Polls advanced
0 to 20. The downstream acceptance check remains **failed**.

New observations, interpreted against the imported header and OEM disassembly:

- PCS receiver enabled: `rx_control=0x00030202`; PCS resets released:
  `pcs_reset=3`; PMA reset releases: `pma_reset=0x7f`.
- CDR control `0x01010101` selects forced lock-to-data. RX frequency status
  `0xa49a0313` / `0xa49b0313` has the vendor's FBCK-lock bit set. This is a
  reported status, not independent clock validation: the calibration code
  also contains forced-lock controls.
- LCPLL status `0x07000101` has the OEM `freq_check()` TX clock lock2 bit
  (16) clear; TDC control is `0x101`. Do not infer from the RX status that
  every optical clock is locked, or enable optical TX to investigate this.
- Both controller MCU address views read `1`; APD control is `0x93a`; OCP
  changes from `5` before PHY startup to `0x43000005` during observation.
  Firmware status is `0xff277573`, LOS control `0x0306000c`, system status
  `0`. The address discrepancy alone is not evidence for a loader rewrite.

All samples retain TX disabled/inhibited, registration disabled, MAC IRQ mask
zero, O1/ONU/OMCC unassigned, 322 MIB objects and zero OMCI/service/protocol
errors. The serial interval has no kernel failure. Postflight and input
cleanup passed: PON modules unloaded, controller off, `ponraw` down, private
RAM inputs removed and management available. No flash or reboot occurred.

`scripts/q1000k/bench-receiver-report.py` saved the aggregate observations in
`receiver-report.json` without converting the failed test into a pass.
`scripts/q1000k/oem-pon-reference.py` saved local OEM source hashes and
disassembly under `build-artifacts/q1000k-xgspon/oem-pon-reference-20260915/`.
The OEM binaries were only read/disassembled, never executed. Its polling
handler includes a no-LOS/no-ready PMA reacquisition path. Preparing an opt-in,
single bounded attempt of that path is the next experiment; no such retry
was run on this device during this capture.

### Opt-in single receive reacquisition prepared — 2026-09-15

The observed thirty-sample light/no-sync state is the trigger condition used
by the reference polling handler's PMA recovery. Vendor r74 / helper r8 now
offer a bounded experiment with `receive --fiber-connected --reacquire-once`.
The unchanged default only observes. After ten consecutive no-sync polls with
both LOS sources clear, the callback consumes its one-attempt lifetime budget,
performs the reference `PLUG_OUT` followed by the existing checked PMA reset
(`PLUG_IN`), then samples the guards again. Neither public status reads nor
IRQ callbacks advance the retry streak. LOS or sync breaks the streak;
stop/start resets the streak but never replenishes the attempt budget.

The private PMA helper requires XGS mode, completed initial calibration,
`first_plugin_flag=false`, TX intent disabled and healthy provider/controller
checks before and after each phase. It cannot enter `FIRST_PLUG_IN` full
calibration. The existing PMA reset suppresses optical TX during the reset and
restores only the previously disabled intent. There is no call to the full
vendor polling handler, registration event dispatch, optical SCU reset or
shared copper SerDes code. Errors contain callbacks and keep TX off; shutdown
waits for an in-flight callback before releasing its resources.

The helper and host runner reject the new flag except for connected receive
tests. JSON records requested mode and zero/one attempts; the thirty-sample
acceptance test still requires final synchronization and advancing frames.
Host tests cover the single-attempt limit, no status/IRQ-triggered retries,
LOS/sync/stopping interruptions, pre/post TX violations, failed PMA phases,
explicit argument forwarding and repeated-attempt rejection. The new UML
run `/tmp/q1000k-pon-phy-uml.Aw4Fup/` passes fifty normal plus fifty RX-only
cycles and both exit/quiesce races during reacquisition. The actual analog
hardware behavior remains untested; this experiment is not a claimed fix.

The running device is left idle on `e26854f308` after successful cleanup.
These source changes have not been installed on it. A new matching RAM FIT
must be built/inspected and booted by the user before this optional experiment.

### Single-reacquisition RAM image ready — 2026-09-15

Source: `90952676e40bb74905aee2b549114b8dd3335804`.
Artifact: `/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-90952676e4/`.
FIT: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`,
7,602,176 bytes; SHA256
`08b91d9ff1e65b1973a48096805cf2ef8cba9c68f0b4e79d14bd4a7d690d342b`.

Full build, **120 host PON tests**, and FIT/kernel/rootfs/DT inspection passed.
Inspection also verifies the new MAC opt-in parameter, PHY reacquisition
routine and helper option, with the helper default disabled. The manifest
contains vendor r74, controller r7 and helper r8. NAND remains disabled;
management remains `192.168.255.1`; immutable optical TX inhibit, no PON
autoload, panic zero and the identity CLI/LuCI fields remain verified.
The saved PHY UML run and generated-source equivalence are recorded under
`uml-phy/` and `uml-phy-source-verification.json`. No new OMCI core run is
claimed. Normal configs, protected refs and the user's board README were
preserved. The same unrelated cached-feed warnings remain in the build log.

The connected test on `e26854f308` remains failed for downstream sync, with
successful cleanup. No additional device mutation was performed while this
new image was built. Ask the user to RAM-boot the new FIT via second-stage
http-uboot and report SSH ready. Fiber may remain connected; do not flash.
Then run the prepared, opt-in experiment against the exact new artifact:

```sh
python3 scripts/q1000k/bench-run.py receive \
  --artifact ../build-artifacts/q1000k-xgspon/bench-90952676e4 \
  --output ../build-artifacts/q1000k-xgspon/bench-90952676e4-receive-reacquire-01 \
  --inputs /tmp/q1000k-controller-test-1093614699-inputs.tar \
  --fiber-connected --reacquire-once --serial-log /tmp/serial_output.log
```

Retain before/after snapshots with `bench-receiver-report.py`. Treat the
experiment as unproven until the hardware observations are collected. A
reacquisition attempt alone cannot pass the downstream acceptance check,
and the test cannot establish O5 registration, OMCI service or WAN traffic.

### Consolidated receiver bench prepared — 2026-09-15

The user requested fewer image builds and more useful tests per RAM boot.
The pending r74 image has not been tested on hardware. Vendor r75 / helper r9
consolidate the remaining read-only receiver hypotheses and add runtime
observation windows before the next image is selected.

Every RX sample now carries `receiver_version=2` with 24 PHY words. These
eleven additional words are ordinary configuration registers used by the
reference PMA initialization/reset paths; reads do not select probes, clear
interrupts or replay strobes:

| Field | Physical address | Reference register |
| --- | --- | --- |
| `rx_sequence_force0` | `0x1fa8b110` | RX_CTRL_SEQUENCE_FORCE_CTRL_0 |
| `rx_sequence_disable0` | `0x1fa8b108` | RX_CTRL_SEQUENCE_DISB_CTRL_0 |
| `rx_lock_force` | `0x1fa8b330` | RX_FORCE_MODE_9 |
| `rx_lock_disable` | `0x1fa8b33c` | RX_DISB_MODE_8 |
| `rx_oscal_control` | `0x1fa8b840` | rg_force_da_pxp_rx_oscal_en |
| `rx_reset0` / `rx_reset1` | `0x1fa8b204` / `208` | RX_RESET_0 / 1 |
| `pll_power` | `0x1fa8b000` | SS_LCPLL_PWCTL_SETTING_0 |
| `pll_filter` | `0x1fa8b034` | SS_LCPLL_TDC_FLT_3 |
| `pll_pcw1` / `pll_pcw2` | `0x1fa8b048` / `04c` | SS_LCPLL_TDC_PCW_1 / 2 |

In particular, the paired force/disable words let us interpret the earlier
FBCK-lock status; the sequence and OS-calibration controls allow comparison
with the OEM initialization without guessing a corrective write. The later
matrix analysis below corrects the earlier suspected OSCal tail difference.
PLL power/filter/frequency configuration and RX reset controls add
context for the clear PLL lock2 bit. None is a measured frequency or a proven
cause of failed synchronization. Snapshot publication remains atomic under
the callback mutex and fails closed on any failed read.

The same image offers 30, 90 and 180 receive samples. The saved host matrix
combines baseline, optional one-time PMA recovery and longer observation in
two sequential sessions on one boot. It selects longer observation without
recovery if the baseline already passes. Before continuing after a downstream
failure it independently validates all sample counts, TX/controller/OMCI
guards, serial diagnostics and cleanup; uncertain light or another failure
stops it. No extra retry budget, analog writes, SCU reset, TX or registration
path is added. Existing direct `receive` defaults stay observational.

Independent diagnostics are collected in the same window. Physical tests are
serialized because they share the controller/PHY. Image compilation and the
isolated cached PHY UML run can execute concurrently with immutable source;
tests that read prepared vendor/kernel sources wait for compilation to finish.
The full scheduling and saved commands are in `scripts/q1000k/README.md`.

These changes have not accessed the device. New image build/inspection and
updated UML evidence are pending at this source checkpoint. The existing
downstream synchronization failure remains unresolved.

### Consolidated RAM image ready — 2026-09-15

Source checkpoint: `13b833a711d9180eab264f9024e81bdc281f4677`.
Artifact: `/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-13b833a711/`.
FIT: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`,
7,602,176 bytes; SHA256
`773ac9a8d840906360c247d5812353c006628eda3e095e3eabcbce3a480d5e78`.
This image supersedes the untested `90952676e4` image for the next RAM boot.

Full build, **131 host tests**, and FIT/kernel/initramfs/DT inspection passed.
The manifest confirms vendor r75, controller r7 and helper r9. Inspection
checks the version-2 diagnostics, bounded sample options and disabled default
recovery, alongside NAND disabled, immutable TX inhibit, panic zero,
management `192.168.255.1`, no PON autoload and CLI/LuCI identity fields.
Normal configurations, all protected source refs and the user's board README
remain unchanged. Fourteen exact runtime file hashes/copies and the dry-run
`matrix-plan.json` are saved beside the image.

The isolated PHY UML run `/tmp/q1000k-pon-phy-uml.SjbWRm/` ran concurrently
with image compilation using its own cached source/build. It passed fifty
normal and fifty RX cycles, expanded register checks, callback/IRQ teardown
and exit/quiesce races during recovery. `bench-record-uml.py --phy-only`
verified generated-source equivalence and saved the evidence under `uml-phy/`
and `uml-phy-source-verification.json`. The unchanged OMCI core retains its
earlier independent evidence; no new core UML run is claimed.

Offline replay of the complete `e26854f308` failed capture validates the matrix
decision: retain the failed downstream result, recognize successful guarded
cleanup and select the single-recovery follow-up. No device access occurred
while consolidating, building or validating this image.

After the user RAM-boots this FIT through second-stage http-uboot and reports
SSH ready, run the saved connected matrix against the exact artifact:

```sh
python3 scripts/q1000k/bench-matrix.py \
  --artifact ../build-artifacts/q1000k-xgspon/bench-13b833a711 \
  --output ../build-artifacts/q1000k-xgspon/bench-13b833a711-connected-matrix-01 \
  --inputs /tmp/q1000k-controller-test-1093614699-inputs.tar \
  --fiber-connected --serial-log /tmp/serial_output.log
```

The matrix collects 30 baseline samples and 90 follow-up samples, with all
diagnostics in each window and complete cleanup between stages. It only
selects the opt-in recovery if the baseline has persistent light without
stable frames. Use `--extended-samples 180` for a longer second window on the
same image when justified by the observations. Do not flash, reboot remotely,
enable optical TX/registration or bypass the exact-image/runtime guards.

### Connected matrix captured on one RAM boot — 2026-09-15

The user reported the consolidated bench ready, with the last confirmed fiber
state connected. The saved matrix verified source `13b833a711`, all fourteen
runtime hashes and the RAM/storage/TX guards before either hardware session.
Capture: `/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/bench-13b833a711-connected-matrix-01/`.

Both planned sessions completed on the same boot:

| Observation | Baseline | Single recovery |
| --- | --- | --- |
| RX/controller snapshots | 30 / 31 | 90 / 91 |
| Session time including setup/cleanup | 53.405 s | 118.930 s |
| Controller and PHY LOS | false throughout | false throughout |
| Sync / frames / FEC counts | HUNT / 0 / 0 | HUNT / 0 / 0 |
| PHY polls / IRQ callbacks | 21 / 0 | 64 / 0 |
| Reacquisition attempts | 0 | exactly 1 |
| Downstream acceptance | failed | failed |
| Postflight and private input cleanup | passed | passed |

The one attempt first appears in sample 15, at poll 10, 15.390 seconds after
the first RX observation. Another 82.403 seconds of samples follow it without
sync or frames. This is sufficient evidence that this particular recovery
sequence did not fix the observed state; an unchanged 180-sample repeat was
not run. The saved `comparison.json`, produced by `bench-matrix-report.py`,
records observations around the transition:

| Word | Last before recovery | First after | Final |
| --- | --- | --- | --- |
| RX frequency status | `0xa49b0313` | `0xa49a0303` | `0xa49a0303` |
| PLL status | `0x07000101` | `0x00000100` | `0x00000101` |
| RX analog 0 | `0x7c7e0042` | `0x7c7e0003` | `0x7c7e0003` |
| RX analog 1 | `0x02000603` | `0x10000603` | `0x10000603` |

The PLL lock2 bit remains clear. `rx_lock_force=0x101` and
`rx_lock_disable=0x01000000` confirm that the reference code forces FBCK-lock
status; the separate status bit cannot establish genuine clock acquisition.
The new control words remain unchanged across the attempt: sequence force0
and disable0 zero, OS-calibration control `0x01000100`, RX reset words
`0x01010101`/`0x00010101`, PLL power `0x01000000`, filter `1`, and both PLL
frequency configuration words `0x0fecdd0c`. These observations describe the
state; they are not independent frequency measurements or a root-cause proof.

All 120 RX observations retain TX inhibited/off, registration disabled and
MAC IRQ mask zero. OMCI stays unassigned in O1 with 322 MIB objects and no
packets/service/protocol errors. Serial captures contain no kernel failure.
LED class samples show green blinking and red off during the baseline; red
also appears transiently during the recovery run. No new physical LED
observation was requested or claimed. Both sessions unload the nine modules,
turn the controller off, lower `ponraw`, remove their private RAM inputs and
retain SSH management. No flash, reboot or optical transmission was performed.

The OEM reference was re-extracted with the saved script; its source and
module hashes match the earlier recorded reference. Offline comparison gives
a more specific next investigation:

- OEM `fiber_plug_reset(PLUG_IN)` calls `XPON_DIG_reset`, `XPON_RX_L2D`,
  `XPON_TDC_on`, **`TXPLL_on`**, RX-ready/status and normal R2T selection.
  The OEM call to `TXPLL_on` is at module offset `0x234bc`. The imported
  reconnect path instead holds/releases digital reset around TDC restoration
  and omits that PLL call. This is a verified sequence difference, not yet a
  demonstrated fix. The imported startup already uses `TXPLL_on`; its name
  alone must not be confused with optical transmitter authorization.
- Rechecking `XPON_RX_OSCal` shows its final write is
  `0x1fa8b110[0]=1` in **both** the prepared source and OEM disassembly
  (OEM offset `0x26b4c`). The earlier suspected OSCal-tail discrepancy is
  unsupported and must not justify a register change. Later calibration
  stages modify these controls, so their final sampled zero is insufficient
  evidence of a missing write.

The next code investigation is the clock restoration and digital-reset
ordering around reconnect, including the exact register effects and failure
handling. Do not blindly replay the OEM initialization script or restore its
whole registration poller. There is no new evidence of an http-uboot fault.
The bench remains idle on the tested image; this checkpoint changes only
host-side reporting and notes, so no replacement image was built. The two
new transition-report tests and revalidation of the actual completed matrix
passed; the image retains its previously recorded 131-test validation.

## Optional PLL restoration and TX/RX audit (2026-09-15)

The previous connected matrix saw light on both LOS inputs throughout all
120 samples, but no downstream sync or frames. It does not establish correct
receiver setup. The controller's TX-disable mapping remains 0x51:0x03e0 bit 9,
separate from the PHY RX LOS, sync and frame registers. The two EN7573 selectors
choose GPON versus XGS-PON controllers; they are not separate TX and RX devices.
No evidence in the register mapping establishes a TX/RX swap, and optical TX
operation has not been tested because these benches inhibit transmission.

The user reports their AT&T gateway shows TX range 0..49, current 36, and RX
range -279..-100, current -170. These are consistent with tenths of dBm: TX
+3.6 dBm and RX -17.0 dBm, with displayed ranges 0..+4.9 and -27.9..-10.0 dBm.
The units remain an inference from the reported values, not a retrieved gateway
specification. The gateway's reading is not a Q1000K measurement. Calibrated
Q1000K optical power remains unavailable; do not convert the captured APD or
receiver controls to dBm or publish an unvalidated zero as optical power.

Vendor r76 / helper r10 add opt-in `--restore-pll`, requiring connected RX and
`--reacquire-once`. The same image retains the old recovery and observation
paths. The optional routine applies the thirteen bit updates from public
`TXPLL_on()` after the existing checked PMA recovery; each has provider/controller
checks and masked readback, and the 6/500 us delays match the reference. It
neither calls the full startup calibration nor changes SCU or laser controls.
The poll owner retains the ten-consecutive-light/no-sync threshold, one-attempt
budget and callback shutdown protection. Failure contains callbacks and TX.

OEM `XPON_DIG_reset` at 0x275f8 touches SW_RST_SET bits 11..0, while the imported
hold/release sequence touches bits 6..0. The upper five meanings are not
established. OEM TDC-on also waits 5000 us rather than the public 500 us plus
CDR toggles. These differences remain deliberately unimported. The new PLL
experiment executes after the existing complete recovery, whereas OEM invokes
its PLL routine before final PCS release; this is a bounded comparison, not
an implementation of the whole OEM sequence. No bootloader fault is established.

Receiver schema 3 adds four ordinary control reads (PLL force, measurement,
Kband and outputs) to the existing 24 words. Request/sample mode mismatches
fail helper/report validation. The matrix can opt into PLL restoration for
its one recovery stage; baseline and stable-signal observation remain unchanged.
Focused host tests compare the native write trace and final register contents
with the extracted reference routine, inject every update/readback/health
failure, verify untouched fields and mode/budget guards, and exercise report
rejection. Kernel concurrency and full image validation are recorded separately
when completed. No hardware was accessed during this implementation.

### Validated PLL experiment image

Checkpoint `6fb4b2bae07da7385c986e78b7d4864de2f17634` builds successfully with
vendor r76, controller r7 and helper r10. All 138 PON host tests pass, including
the packaged helper, mode/report consistency and source-derived fault tests.
FIT/rootfs inspection verifies the new module parameter and schema, nine
unloadable modules, disabled NAND, immutable TX inhibit, management address
192.168.255.1 and panic timeout policy. Source-matched PHY UML testing passes
50 normal and 50 RX lifecycle cycles plus concurrent shutdown/quiesce for
both recovery modes, with no kernel failure diagnostics. This is PHY-only UML
evidence; no new OMCI-core UML run is claimed.

Artifact directory in the parent workspace:
`build-artifacts/q1000k-xgspon/bench-6fb4b2bae0/`.
Image: `openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`.
Size: 7,602,176 bytes. SHA256:
`362f51f26ae42aeb9fa03771ec59f90b9ea979464324895443ecc366af27abe4`.
The directory contains build/host logs, inspection JSON, pinned selection,
14 runtime hashes, and `uml-phy-source-verification.json` with saved UML logs.
Both normal configs and all protected branch refs are unchanged. The user's
untracked README is preserved. No device command or firmware flash occurred.

After user RAM boot through second-stage http-uboot, verify this exact image
and run the existing connected matrix with `--restore-pll`; use 30 baseline
samples and the default 90-sample continuation. The previous image does not
contain this experiment. Successful local validation does not predict whether
PLL restoration will establish downstream sync or advancing frames.

## Connected PLL-restoration matrix — 2026-09-15

The user RAM-booted `6fb4b2bae0` at 192.168.255.1 with the fiber connected.
Read-only preflight verified the exact revision, all 14 runtime hashes, RAM
mounts, absent MTD/UBI, immutable TX inhibit, idle modules and LAN carrier.
The saved matrix ran its 30-sample baseline and 90-sample PLL continuation.

Both downstream acceptance tests failed solely for missing sync/frame progress.
Both LOS inputs remained false throughout all 120 samples; sync stayed HUNT
(0), and frames, LOF, FEC and IRQ counters stayed zero. Poll counters advanced
0..21 and 0..64. Exactly one recovery was first observed at sample 15/poll 10,
15.400 seconds after the first continuation sample. Another 82.570 seconds of
observations showed no frames or sync. The extra PLL updates/readbacks did not
report an error. This rules out that isolated restoration as a sufficient fix,
not every possible PLL/reset sequencing issue.

The four new controls were constant before/after recovery:
PLL force 0x01010101, measurement 0, Kband 0x0001000f, outputs 0x00000101.
PLL status changed 0x07000101 -> 0x00000100 -> 0x00000101, the same pattern as
the prior recovery without restoration. RX frequency changed 0xa49a0313 ->
0xa49a0303. Analog observations changed 0x7d7d0042 -> 0x7d7d0003 and
0x03000502 -> 0x10000502. Forced lock flags remain unsuitable as independent
clock-lock proof. TX remained inhibited/off, registration false, MAC IRQ mask
zero, ONU/OMCC unassigned and MIB count 322. No protocol/service or kernel fault
was observed. Green LED class brightness alternated 0/1, red remained 0; no new
physical LED observation is claimed.

Both normal reverse unloads and private-input cleanup passed. The device was
left idle with ponraw down/unenslaved and SSH healthy. No flash, reboot or
transmission occurred. Capture directories in the parent workspace:
`build-artifacts/q1000k-xgspon/bench-6fb4b2bae0-preflight-01/` and
`build-artifacts/q1000k-xgspon/bench-6fb4b2bae0-connected-pll-matrix-01/`.
The latter contains `comparison.json`, independently validated per-stage reports
and private raw/serial logs. The two stages took 53.341 and 119.092 seconds
including setup/cleanup. No unchanged 180-sample rerun was performed.

The user requested controller RX power in the next consolidated bench. OEM
`en7572.ko` `bob_info` reads word 0x0068 using I2C address 0x51 with a two-byte
register address (`read2Bytes`, call at 0x3acc), then byte-swaps its LE helper
result to BE before the optical-power conversion. Its TxPwr uses 0x0066.
The RxPwr format string is `.rodata.str1.8+0xa50`; the logarithmic conversion
is consistent with 0.1 microwatt units, or 100 nW per count. This is a read-only
published sensor word, distinct from `ddmi_rx` and `ddmi_rx_done`, which write
calibration/command registers and must not be invoked for a sensor reading.
Power is currently null in all captured CLI telemetry; no Q1000K dBm value is
yet available. Next implementation must preserve missing/invalid readings and
label a controller-reported value separately from independent optical-meter
validation. Further PHY writes require a supported, isolated candidate rather
than copying undocumented OEM reset bits.

## RX optical power and isolated receiver-gain trial — 2026-09-15

Vendor r77, controller r8, bench helper r11, OMCI tools r3 and LuCI r6 add a
read-only optical RX measurement to the same consolidated test image. The
portable controller reader uses 0x51:0x0068, a two-byte register address and a
big-endian two-byte payload, scaled by 100 to nanowatts. This follows the OEM
`bob_info` evidence above; ordinary control registers remain little endian.
Zero and 0xffff are deliberately reported unavailable (below resolution or
saturated/unusable), not fabricated zero-dBm measurements. The read does not
execute `ddmi_rx`, `ddmi_rx_done` or other calibration commands. The controller
lease and health checks bracket the sensor read. A transport/health fault
withholds the sample and contains the port; unavailable sensor data alone does
not fault it. This is MCU-reported power, not independent optical-meter proof.

The PHY serializes telemetry with callbacks/recovery/stop. OMCI supplies the
existing RX-power telemetry attribute during pre-registration O1 as well as O5;
upstream FEC retains its separate established-session guard. `q1000k-omci -i pon
status` includes `rx_power_nw` and derived `rx_power_dbm` (both null if unavailable).
LuCI displays RX power in dBm and nW, and clears readings on a failed refresh.
TX power remains unavailable. Raw count 199 is a test fixture (19,900 nW,
-17.01 dBm), **not a hardware measurement** or a value copied from the gateway.

Receiver schema 4 adds power validity/value and the actual RX frontend gain
control at 0x1fa8b88c. The earlier `rx_lock_force` observation is at 0x1fa8b330;
it is not the frontend gain control. OEM `phy_10g.ko` `XPON_RX_preset` calls at
0x26420 and 0x26438 set b88c[8]=1, then b88c[1:0]=1. The imported preset omits
those two writes. An explicit `--restore-gain` option applies only this isolated
candidate after the one permitted recovery, independently of `--restore-pll`.
It does not replay the OEM upper digital-reset bits or change the default PHY
path. Each update checks provider/controller health and masked readback.

Gain bits are saved before alteration and restored after callbacks drain on
stop/quiesce, with checked readback and a serial confirmation. A gain capture
with an observed attempt cannot pass report validation without that confirmation.
Only those two fields are restored; unrelated bits are preserved. Failed cleanup
propagates through physical shutdown and stops matrix continuation. Both flags
require RX bench mode, immutable TX inhibit and a single recovery budget. No
normal registration or optical transmission is added.

Next hardware comparison, after the user boots this exact new RAM image:
30-sample baseline and 90-sample continuation with `--restore-gain`. Capture
power, LOS, all 29 PHY words, controller words, frame/FEC counters and LEDs in
both windows. Baseline remains unchanged. The same image also supports the
existing plain/PLL recovery and gain+PLL combination, selected explicitly;
these experiments run sequentially, never concurrently against the PHY. Select
any further mode from the first comparison's evidence, without a rebuild.
No device was accessed while preparing these changes. NAND stays disabled,
management stays 192.168.255.1, optical TX stays inhibited, and flashing remains
prohibited. Local build/concurrency results will be recorded after completion.

### Expanded hypothesis coverage in the same delivered bench

The user requested tests for the other light-present/no-frame explanations
before delivery. The r77 build was gracefully stopped and both normal configs
restored. Vendor r78/helper r12 extend the same image to schema 5: 43 control
words and seven additional PCS counters. No extra experimental write was added.
See [the nine hypotheses and test matrix](XGSPON-RX-HYPOTHESES.q1000k.md).
`bench-hypotheses.py` evaluates saved captures, and `bench-matrix.py` records its
report automatically for schema 5. Existing images/schemas remain reportable.
The expanded UML fixture's expected callback totals were updated for the added
power-concurrency test and two gain teardown cases; the first local UML run
stopped at the old count assertion, not a hardware or lock failure. A fresh
source-matched run is required before delivery.

### Consolidated RX bench validated — 2026-09-15

Image source is `7e9c0edc1d3eff3d36a1ccf5ab2ac28ed4de36d0` on
`q1000k-xgspon`. The final artifact is in the parent workspace at
`build-artifacts/q1000k-xgspon/bench-7e9c0edc1d/`:

`openwrt-airoha-an7581-quantum_q1000k-xgspon-bench-initramfs-bench.itb`

- Size: 7,602,176 bytes.
- SHA-256: `b8151308f75502ad21955d3e426e8d9791183eae8d646056b2076e45824fa97f`.
- Kernel 6.18.44, vendor r78, controller r8, bench helper r12, OMCI tools r3,
  status backend r7, LuCI r6.
- Full PON host suite: 142 tests passed. Status backend: 13 passed. LuCI
  production-view fixtures passed, including valid/missing/malformed power and
  clearing stale readings after failed refresh.
- Source-matched PHY UML: 50 normal plus 50 RX-only cycles, callback/power
  serialization, concurrent polling, RCU/IRQ teardown, and stop/quiesce during
  plain, PLL, gain and gain+PLL recovery. No kernel diagnostics. Hardware MMIO
  recovery is mocked in UML; the production PMA sequence and failure handling
  have separate host fixtures. This does not claim a new OMCI core UML run.
- Exact FIT, embedded initramfs, DT, module lifecycle and packaged CLI/LuCI
  inspection passed. NAND is disabled, TX inhibit is immutable, LAN is
  192.168.255.1, and no private controller firmware/calibration is embedded.

The first final inspection incorrectly compared the source JavaScript byte for
byte with LuCI's minified package. Validation-only commit `0a73e77e79` corrects
that check using the build's `jsmin` tool. `bench-revalidate.py` verified the
saved artifact/test hashes, unchanged production and original test sources,
normal configs and protected refs, reran inspection, and matched all fourteen
exported runtime files against the exact embedded image. The original failed
checkpoint and inspector output remain beside the successful evidence; the FIT
was not rebuilt or modified. Two revalidation guard tests also passed.

PHY UML evidence and generated-source verification are attached under
`uml-phy/` and `uml-phy-source-verification.json`. The run source is
`/tmp/q1000k-pon-phy-uml.UjL3ow/`. Normal builder state, both normal OpenWrt
configs, protected refs and the user's untracked README remain unchanged.

Hardware validation is pending a new user-controlled RAM boot through
second-stage http-uboot. Start with preflight, then the connected 30+90 sample
gain matrix described in `XGSPON-RX-HYPOTHESES.q1000k.md`. No new hardware RX
power measurement, successful sync, O5 or subscriber service is claimed.
No SSH, optical transmission, flash, remote reboot or bootloader change was
performed while preparing this image.

### Connected optical-power and gain matrix — 2026-09-15

The user RAM-booted image `7e9c0edc1d` and confirmed the fiber firmly connected.
Read-only preflight matched all fourteen runtime hashes, management at
192.168.255.1, RAM root, no MTD/UBI devices, immutable TX inhibit, disabled
normal PON service and an idle endpoint. Runtime panic timeout was already 0.

The 30-sample baseline and 90-sample gain continuation completed on this boot.
Neither established downstream synchronization; the original failed acceptance
results are retained. Observation, guard and cleanup validation passed in both.

| Observation | Baseline | Gain continuation |
|---|---|---|
| Valid RX-power samples | 30/30 | 90/90 |
| MCU RX power | 12,800–13,700 nW (-18.93 to -18.63 dBm) | 13,400–13,600 nW (-18.73 to -18.66 dBm) |
| Last MCU RX power | 13,600 nW (-18.66 dBm) | 13,500 nW (-18.70 dBm) |
| Both LOS indications | Clear throughout | Clear throughout |
| Sync state | HUNT (0) throughout | HUNT (0) throughout |
| Frames, LOF, FEC, PHY IRQ calls | Zero throughout | Zero throughout |
| All seven added PCS counters | Zero throughout | Zero throughout |
| Recovery attempts | 0 | 1 |
| Elapsed including setup/cleanup | 53.953 s | 121.174 s |

This is the first Q1000K hardware RX-power measurement; it is not copied from
the gateway. CLI telemetry reports validity bit 64 and the same nW/dBm values.
The controller reading is about 1.7 dB below the gateway's previously inferred
-17 dBm. That comparison is not a calibration or modulation-quality proof.

The gain attempt was first observed in sample 15 at poll 10, 15.734 seconds
after the first snapshot, leaving 84.253 seconds of post-attempt observation.
The gain control changed from 0x102 immediately before recovery to 0x101 and
stayed there. Normal cleanup confirmed `RX gain restored to 0x102`. The
separate initial baseline had gain 0x103; calibration can produce different
starting values across starts, so do not claim identical gain baselines.
The isolated OEM gain choice was insufficient to restore reception.

New stable observations included PCS debug control 0x310, RX input control 0,
SFP status 0/polarity 9, digital status 1, SerDes control 0x000c0000, RX clock
divider 0x01000100, bus width 1, CDR ratio 0x00800000, rate control 2, OSR
control 0x01000000, signal control 0x00010001, equalizer 0x01010100 and frontend
power 0x02000401. RX enable, released PCS reset, inactive counter-clear and
enabled descrambling weaken the simple disabled/reset/held-clear explanation.
The sampled rate/divider/bus/OSR fields match the imported XPON_RX 9/10G branch;
this does not prove recovered clock or correct physical routing. All-zero
codeword/HEC/MAC-boundary counters fail to uncover earlier receive activity.
The misleading FEC_FORCE_OFF macro name is not decoded as disabled FEC:
the imported enable handler deliberately sets that field.

TX stayed inhibited/off, registration disabled, MAC IRQ mask zero, O1 with
unassigned ONU/OMCC, MIB count 322 and no protocol/service/kernel fault. Green
LED class brightness alternated 0/1 and red stayed 0; no new physical LED
observation is claimed. All nine modules unloaded, controller powered off,
ponraw went down/unenslaved, private staged inputs were removed, and SSH stayed
healthy. No flash, reboot, TX enable or persistent change occurred.

Captures in the parent workspace:
`build-artifacts/q1000k-xgspon/bench-7e9c0edc1d-preflight-01/` and
`build-artifacts/q1000k-xgspon/bench-7e9c0edc1d-connected-gain-matrix-01/`.
The latter includes the independently validated `comparison.json` and per-stage
`observations.json`, `receiver-report.json`, `hypotheses.json` and raw evidence.
Next is a user-confirmed disconnected control, followed by reconnection, to
test the sensor/LOS response without a new image or speculative register write.

### Dark/reconnected controls and combined recovery — 2026-09-15

After the user confirmed disconnection, a 30-sample receive-only dark control
passed. Both LOS sources were true in every sample. MCU RX power was 100 nW
(-40 dBm, one sensor count) throughout. Treat this as a near-floor indication,
not an independently calibrated dark optical measurement. SFP status changed
from 0 connected to 1 disconnected; SFP polarity remained 9. LED class readings
were green 0/red 1. Cleanup passed and the stack was unloaded before asking
the user to reconnect.

The user then confirmed firm reconnection. On the same image a second matrix
captured a 30-sample baseline and one 90-sample gain-plus-PLL trial:

| Observation | Reconnected baseline | Gain + PLL |
|---|---|---|
| MCU RX power | 13,600–14,600 nW (-18.66 to -18.36 dBm) | 14,200–14,700 nW (-18.48 to -18.33 dBm) |
| Last RX power | 14,400 nW (-18.42 dBm) | 14,300 nW (-18.45 dBm) |
| Both LOS sources | Clear throughout | Clear throughout |
| Synchronization | HUNT (0) throughout | HUNT (0) throughout |
| Original and seven additional receive counters | Zero throughout | Zero throughout |
| Recovery attempts | 0 | 1 |
| Setup/observation/cleanup elapsed | 53.749 s | 121.294 s |

The combined attempt was first observed in sample 15, at poll 10, 15.731 s
after the first snapshot. An additional 84.292 s still showed no reception.
Gain changed 0x103 -> 0x101 and normal teardown confirmed restoration to 0x103.
Readback checks for both optional routines succeeded; they were insufficient
to restore synchronization. Neither this nor the prior isolated tests rules
out a sequencing/timing dependency in the complete OEM initialization.

Read-only live captures of the LuCI backend showed LOS=true/RX=100 nW when
disconnected and LOS=false/RX=14,400 nW after reconnection, with telemetry bit
64 and TX power null in both. After final unload its OMCI/LOS/registration
fields were null and PHY/MAC-loaded flags false, so the backend did not retain
the last power value. The browser UI was not independently operated in these
tests. Physical fiber changes occurred between unloaded runs: these controls
establish state response across starts, not live insertion IRQ or latency.

All five receive runs (270 samples total) on this one boot passed TX/registration,
protocol/service, postflight and private-input cleanup checks. The dark control
passed its acceptance criteria; all four connected windows retained failed
downstream acceptance. No kernel fault occurred. The last state is idle, all
nine PON modules unloaded, controller powered off, ponraw down/unenslaved,
private inputs removed, SSH healthy and fiber physically connected. No flash,
reboot, optical transmission, bootloader change or new image build occurred.

Additional captures in the parent workspace:
`build-artifacts/q1000k-xgspon/bench-7e9c0edc1d-dark-control-01/` and
`build-artifacts/q1000k-xgspon/bench-7e9c0edc1d-reconnected-gain-pll-matrix-01/`.
Both have independently generated receiver/hypothesis reports; the matrix also
has `comparison.json`. Live backend captures are `luci-status.json` in the dark
capture and reconnected baseline, plus `idle-luci-status.json` at matrix root.
The updated hypothesis document records what this evidence does and does not
resolve; repeating the same recovery modes or longer unchanged observations
is not the next step.
