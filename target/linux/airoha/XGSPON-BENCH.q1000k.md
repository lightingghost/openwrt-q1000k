# Q1000K RAM bench

This is a separate `quantum_q1000k-xgspon-bench` image target on
`q1000k-xgspon`. It produces an initramfs FIT, no sysupgrade or bootloader
artifact. The normal Q1000K UBI target remains unchanged. RAM boot,
read-only preflight and the controller-only test have passed; PHY/MAC/OMCI
and optical service acceptance remain pending.

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
