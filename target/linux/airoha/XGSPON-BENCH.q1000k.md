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
