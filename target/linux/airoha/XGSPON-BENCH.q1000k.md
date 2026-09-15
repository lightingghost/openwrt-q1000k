# Q1000K RAM bench

This is a separate `quantum_q1000k-xgspon-bench` image target on
`q1000k-xgspon`. It produces an initramfs FIT, no sysupgrade or bootloader
artifact. The normal Q1000K UBI target remains unchanged. RAM boot and
read-only preflight have passed; optical hardware acceptance remains pending.

The bench DT disables the NAND controller and NAND chip, removes partition
definitions and the persistent rootdisk reference, and uses console-only
boot arguments. It enables native GDM2 (`ponraw`) and the manually loaded
vendor PHY/MAC resources; the competing native PON PCS stays disabled.
Native GDM2 uses a 10G internal fixed link for CPU DMA availability, not
optical carrier. The controller caches `quantum,tx-inhibit` at probe and
rejects all consumer TX-enable requests with power-off containment.

The explicit builder `--profile bench` selects this target and
`192.168.0.1/24`, including preinit/failsafe. LAN DHCP, DHCPv6 and RA servers
are disabled, both copper ports remain LAN, and the normal PON service is
disabled. Connect a dedicated host at e.g. `192.168.0.2/24`, with no gateway
on that link. **192.168.1.1 belongs to the user's working router and must
not be used for Q1000K SSH.** No default WAN or OLT identity is inferred.

## Staged device tests

The user has RAM-booted the bench, provided SSH at 192.168.0.1, confirmed
disconnected fiber and explicitly authorized the controller-only test. This
permits RAM staging, controller module/GPIO/I2C operations and cleanup.
PHY/MAC/OMCI activation still requires separate authorization. Keep the
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
   the running image revision, RAM root, absent MTD/UBI, address 192.168.0.1,
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
