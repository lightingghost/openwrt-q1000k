# Q1000K XGS-PON implementation checkpoint

2026-09-14, branch `q1000k-xgspon`, based on `q1000k-dev` at
`c526db0e25fa159ca79b60125741afd4e08440c9`.

**Local integration is progressing; working optical service has not been
 demonstrated.** Vendor r52 and OMCI core r8 implement the native packet path,
physical drain and namespace replacement, checked cold startup/reset, burst
profiles and ranging, authenticated PLOAM/OMCI, baseline unicast provisioning,
SP/WRR scheduling and class 171 tag transformations. The optional supervisor,
`q1000k-omci` userspace command and read-only LuCI status/MIB are implemented.
Data-key/encrypted-service support, combined VLAN filter pipelines and the
remaining advanced service paths are still incomplete.

**Device access is read-only; never flash firmware.** No device access or
activation occurred during this continuation. The controller bring-up tests
below predate that restriction: both EN7573 paths detected, OEM MD32 firmware
and calibration read back correctly, and TX-disable remained asserted. Those
historical RAM tests do not verify the current MAC/PHY/OMCI integration.

The current local evidence is 70 passing PON host tests, the complete OMCI
core tests in UML, real Linux skb VLAN tests, and matching AN7581 core/vendor
package builds. Earlier checkpoints contain the native drain, protocol,
cryptography and PHY concurrency results. Hardware cold boot, OLT registration,
optical traffic and recovery remain acceptance gates. PON board nodes remain
disabled and the experimental packages remain optional/BROKEN.

See [integration details](XGSPON-INTEGRATION.q1000k.md),
[VLAN contract](XGSPON-VLAN.q1000k.md) and [AT&T research](XGSPON-ATT.q1000k.md).

## Imported references

| Import | Local commit | Original source |
| --- | --- | --- |
| Complete `package/kernel/airoha-pon` source import | `406e859e25` | [coolsnowwolf/lede f7fd86e](https://github.com/coolsnowwolf/lede/commit/f7fd86eaa58c29fed97da04ab219c74a835a9358) |
| Complete `luci-app-econet-xpon` source import | `87650e5472` | [AKoo7/openwrt e27eee8](https://github.com/AKoo7/openwrt/commit/e27eee81fddad217e111ce67bc7a8102b00b24b4), from [OpenWrt PR #24577](https://github.com/openwrt/openwrt/pull/24577) |

The import commits retain original authors and source trailers. Follow-up
changes move LuCI to `package/luci-app-econet-xpon`, use the existing LuCI
build system and replace its EN7528 backend with `q1000k-xgspon`. The new
backend follows the PR's UCI identity concept but uses the Q1000K factory
layout and a new status schema. The EN7528 driver/configuration package,
OMCI daemon and unrelated PR changes were not imported.

## Dependency matrix

| Component | Implemented and locally verified | Remaining acceptance or work |
| --- | --- | --- |
| Board/controller | AN7581SIT, two EN7573AN; exclusive resources, checked controller lease/TX state, factory identity and calibration, SHA-checked OEM firmware | Production DT/pinctrl cold boot, analog tuning/alarm behavior, long-running firmware health |
| Factory data | Named UBI factory reader and offline DSD fallback; no automatic raw MTD fallback | Establish logical NAND/BBT view before supporting direct legacy DSD reads |
| Native packet transport | Native FE/QDMA ownership, TX/RX retirement, bounded retries, epochs, verified queue closure | Hardware descriptor, timing and loaded traffic validation |
| PHY/MAC lifecycle | Owned IRQ/protocol executor, physical clear/install/reactivation, cold receive-only startup, reset, full burst profiles and checked ranging | Hardware synchronization, burst timing, full analog/PHY audit and interoperability |
| GEM/T-CONT | Full namespace replacement, quarantined failures, PLOAM allocation reconciliation, separate ONU/OMCC publication | Encrypted GEM activation, multicast channel ownership and hardware validation |
| FE/QoS | Checked native frame/queue controls, SP and WRR managed entities, drained scheduler replacement | Advanced shaping/backpressure and hardware throughput tests |
| Authentication | 36-byte registration derivation, software OMCI/PLOAM MICs, key/epoch barriers, recovery containment | Unicast data-key exchange, encrypted service and secure mutual-authentication rekey |
| OMCI services | Generic core, topology, unicast GEM/bridge/mapper provisioning, class 171 tag processing on UNI-facing pon0 | Combined class 84/171 pipeline, advanced VLAN modes, multicast and actual OLT identity/MIB compatibility |
| Userspace/LuCI | q1000k-omci status/MIB/get/set; read-only LuCI; optional explicit supervisor with owned teardown | Browser QA on an installed image, final netifd/firewall integration and hardware lifecycle acceptance |
| AT&T WAN | DHCP identified; optical VLAN must come from provisioning, with local tag handling defined by OMCI | Actual gateway/line identity, OLT provisioning, successful DHCP and IPv4/IPv6 tests |
| Builds | Matching experimental modules/packages; normal builder remains on q1000k-dev | Experimental image boot/recovery acceptance; never flash under the present restriction |

The following entries are historical checkpoints. Their outstanding-work lists
describe their original dates; use the matrix above for the current state.

The 2026-09-13 continuation adds exclusive native QDMA1 queue configuration,
verified queue-close writes and packet admission epochs. The adapter rejects
closed queues and preserves the epoch across retries, preventing a queued frame
from becoming valid when its channel is reopened. Four remaining AN7581 QDMA
wrappers now propagate unsupported/dispatcher errors. The native/adapter UML
concurrency tests and full kernel build pass; see the
[queue admission checkpoint](XGSPON-INTEGRATION.q1000k.md#native-queue-admission-checkpoint-2026-09-13).
Patch 018 now connects T-CONT setup/removal to native queue closure, propagates
control errors and prevents uncertain channel reuse. Indirect MAC commands
are serialized and verified; timeout faults remain latched. Removal retains
bindings and returns an error until physical retirement exists. All 24 host
PON tests, a separate Linux UML T-CONT concurrency test and the r15 vendor
package build pass. FE retirement, GEM mapping synchronization and complete
ONU/OMCC transactions remain pending; see the
[T-CONT checkpoint](XGSPON-INTEGRATION.q1000k.md#t-cont-command-and-setup-checkpoint-2026-09-13).
Kernel patch 9999b and vendor patch 019 add native TX reclamation per channel
and prevent reopening within the attachment. Native/adapter UML tests pass,
including control from a real interrupt; the native locking warning they
exposed is fixed, and all PON runners now reject kernel diagnostics explicitly.
The final Linux 6.18.44 AN7581 kernel and r16 vendor APK build successfully;
all six vendor modules pass modpost against the eight native API exports.
All 24 host PON tests also pass against the final prepared kernel/package tree.
See the [channel drain checkpoint](XGSPON-INTEGRATION.q1000k.md#per-channel-tx-drain-checkpoint-2026-09-13).
Vendor patch 020 now publishes data GEM bindings only after verified hardware
commands and snapshots complete GEM/ANI/T-CONT mappings for TX/RX. It fixes
unchecked/truncated indices and preserves retiring mappings without reuse.
Raw/debug writes and legacy replay are blocked; encrypted activation and live
replacement remain unsupported. All 27 host PON tests and the real-kernel GEM
concurrency/interrupt test pass; all six vendor modules pass modpost and the
r17 package builds. See the
[GEM checkpoint](XGSPON-INTEGRATION.q1000k.md#gem-command-and-binding-checkpoint-2026-09-13).
Kernel patch 9999c and vendor patch 021 add verified native GDM2 FE TX-channel
control to T-CONT setup/rollback. Enabling requires closed queues and reclaimed
native mappings; disable permanently closes admission. Readback faults block
new transmission and cannot be cleared by a later successful write. All 27
host tests, the native/adapter UML tests, the AN7581 kernel and r18 package
build pass. Physical RX/FIFO retirement and the remaining FE/QoS/PHY/OMCI
integration are still required; see the
[FE TX-channel checkpoint](XGSPON-INTEGRATION.q1000k.md#native-fe-tx-channel-checkpoint-2026-09-13).
No SSH access or device changes were made for this continuation.

The reference loader was inspected at
[airoha_xpon_en757x 950199a](https://github.com/Sirherobrine23/airoha_xpon_en757x/tree/950199a8de6b75e76906a7c1b39b7a9a3e2913f9/v2/lddla).
It reads the controller family ID at I2C address `0x51`, register `0x0408`,
expecting `0x1388`. Register addresses are two bytes, most significant byte
first; the two data bytes form a little-endian word. This is a family check,
not proof of which EN7573AN is selected. **Control and address registers
use `0x51`; only PM/DM data ports `0x3008`/`0x3014` use `0x50`.** This
corrects the initial checkpoint's overly broad assignment to `0x50`.
The loader consumes a 512-byte BOB payload. The OEM's full 513-byte record
is preserved by our reader; the first 512 bytes go to DM offset `0x600`.
The working copy retains the unit's original bytes, including vendor text.
The reference loader's firmware hashes differ from the local OEM pair, so
its bundled firmware must not be substituted just because filenames match.

The alternative
[Airoha kernel xPON source at 2e2cf91](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c)
was also inspected. Its implementation covers GPON/EPON; its resource-size
comment explicitly leaves XGSPON for later. It is an architectural reference,
not an already working AN7581 XGS-PON substitute.

## Live hardware observations

SSH checks used the user-provided `root@192.168.1.1`. The running image reports
Linux `6.18.44`, OpenWrt revision `r36173+21-7e7d8a4d19`. The factory volume
is currently `/dev/ubi0_2`, with 65536 data bytes, on MTD 2 named `ubi`.
The reader discovers these values; neither number is hardcoded. Its FSAN
at `0x9000`, binary WAN MAC at `0x5000` and XGS-PON record at `0xb000`
match the original DSD data. Identifiers and calibration are omitted here.

The live RPC check ran the built AArch64 reader, production shell backend,
CLI and rpcd dispatcher under a temporary `/tmp` directory. Only their
program paths were redirected to that directory. Real UCI, jshn, factory,
sysfs and firmware paths were used. The result reported valid factory data,
both PON modules absent, both firmware files unverified, and all optical
states unavailable. The temporary files were removed afterward.

The board has one 64-line GPIO controller. The old kernel leaves offsets
8, 9, 10, 11, 45 and 46 unrequested. OEM global numbers 461/462, 488/489
and 490/491 correspond to local 45/46, 8/9 and 10/11 respectively.
Independent power tests with the fiber disconnected confirmed:

| Selected path | GPIO 45/46 | GPIO 8 | GPIO 9 | Family ID |
| --- | --- | --- | --- | --- |
| GPON | low/low | high | low | `0x1388` |
| GPON, own enable inactive | low/low | low | high | ENXIO |
| XGS-PON | high/high | low | high | `0x1388` |
| XGS-PON, own enable inactive | high/high | high | low | ENXIO |
| Either, both disabled | either paired setting | high | high | ENXIO |

GPIO 8 is therefore active-low XGS-PON enable, and GPIO 9 is active-low
GPON enable. Force-GPIO bits are SCU `0x228[8:11]` and `0x22c[13:14]`.
The pinctrl patch exposes these GPIO functions and writes output latches
before enabling output, avoiding a transient opposite-level assertion.

I2C adapter `1fbf8000.i2c0` exists at `/sys/bus/i2c/devices/i2c-0`, with
`/dev/i2c-0`; absence of `/sys/class/i2c-adapter` alone does not mean I2C
is unavailable. The driver reports 100 kHz. The initial unpowered probe
returned ENXIO; the confirmed GPIO sequence resolves it. Changing the
I2C-master selection bit `0x214[13]` alone did not help and is unnecessary
once the GPIOs are configured. The production implementation leaves it alone.

The standalone driver completed the exact OEM PM/DM upload and every-word
readback of all 16 KiB PM and 4 KiB DM, including this unit's 512-byte
calibration payload. MCU enable and TX-disable read back asserted after
startup. XGS LOS was high with the fiber disconnected. These are controller
register samples, not a firmware heartbeat or proof of a working optical
service. The existing image lacks the new DT node, so the tests used a
temporary lookup-table/client harness, reserving GPIOs and restoring the
owned register bits on exit. No full-image cold-boot test has been done.

## Kernel audit

`kmod-airoha-xpon-en757x` is gated by `BROKEN`, remains unselected and has
no autoload entry. `KBUILD_MODPOST_WARN=1` was removed from the MAC build
command. The package must not produce a nominally successful build
with unresolved symbols. No compatibility no-ops were added to satisfy
missing runtime interfaces.

Patches 002/003 fix the hook declaration and AN7581 IOMUX table bound.
Patches 004/005 select the AN7581 BSP dependency graph and fix public
declarations, arm64 IRQ flags, PHY probe error lifetime, callback/argument
and return types, jiffies width and mapped PHY register access. AN7583 retains
its extra board/OLT/combo-PHY objects. The four AN7581 BSP modules and
`phy_10g.ko` compile and pass modpost against Linux 6.18.44/GCC 14.4.0.

Patch 006 restores runtime state and PHY-to-MAC event dispatch from the
original `gpon_proc.c`, which the imported compatibility patch had replaced
with success stubs. It exposes cached software state through a read-only
`/proc/xgpon/status` seq_file and propagates procfs creation failures. Obsolete
writable debug commands remain unavailable. It also disables OEM WAN-to-LAN
mirroring, replaces `random32()` with `get_random_u32()` and fixes const/label
compilation issues.

Patch 007 adds the bounded Q1000K MAC register/IRQ provider and a disabled
board DT node with named register windows and interrupts. Probe does not
change clocks, resets or DMA. SCU access uses existing syscon regmaps; masked
updates are atomic with other regmap users, and full writes retain write
semantics for strobes. PHY/MAC initialization rejects absent resource providers.
This does not establish reset/clock sequencing or error propagation through
legacy void/value-only register APIs. See the integration audit for limits.

Patch 008 initializes every QDMA/FE request with `-EOPNOTSUPP`, propagates
negative dispatcher errors, and publishes copied getter fields only on
success. Hook dispatch checks indices before indexing and inspects lists
under RCU; inactive providers count as absent. The MAC checks for QDMA WAN
and FE providers before initialization. Host fixtures cover missing/inactive
providers, unhandled requests, error propagation, copied outputs and packet
ownership at the wrapper boundary. The checks do not pin a provider or
implement the shared Ethernet adapter; caller error handling and teardown
still need work.

Patch 009 connects immutable, validated `wan_mac` and `pon_serial` module
parameters to the MAC identity APIs, WAN netdev creation and GPON serial
configuration. Missing/invalid identity fails before resource or hardware
startup; no default MAC/serial or raw NAND read is used on Q1000K. Getter
errors and netdev registration failures propagate. The board mode is fixed
to XGS-PON/SFU, consistent with the cached OEM `onu_type=71` record; other
`mode` overrides are rejected. This removes six more linker dependencies.
The future launcher must supply the backend's selected factory/override
values. It is not yet connected to a running service. Registration ID/MSK
and authentication behavior remain to be integrated and validated.

Patch 011 corrects the imported crypto conversion to use the Linux 6.18
`ecb(aes)` lskcipher transform and declares its ECB/AES dependency. Shared
transforms are serialized across key setup and encryption. AES/CMAC failures
propagate, outputs are published only after success, and temporary key-derived
blocks are cleared. CMAC handles empty and split vectors without advancing
past their bounds; ECB rejects lengths other than one AES block. Host tests
pass the NIST AES-128/CMAC examples, 33,411 fragmented-input cases against
OpenSSL's independent CMAC implementation, and injected errors at every crypto
operation. ASan/UBSan are enabled; leak detection is disabled for the ptrace
sandbox. Additional fixtures verify both allocation failures, retry, duplicate
initialization and timer shutdown before transform release. The extracted
production crypto helpers also pass AES/CMAC known-answer tests using the real
Linux 6.18.44 crypto API in a disposable UML guest. This does not validate
on-device key transitions or authenticated OMCC.

Patch 012 adds staged startup and software teardown: errors propagate from
QDMA setup, worker creation and every WAN interface; failed startup releases
only completed stages. Callback ingress and netdev opens remain closed until
all state is ready. Protocol interrupts are enabled last and masked before
hook removal. Teardown closes WAN interfaces, drains RCU callbacks, timers and
tasklets, stops the worker and clears global aliases. It fixes the dynamic
cdev release path and the worker's idle/full-queue stop deadlock. Five host
fixtures pass 17 startup failure points with retry, all four WAN interface
failures, cdev failures, crypto-before-interrupt ordering and 201 real pthread
worker cycles. These fixtures do not establish hardware rollback or the
missing native QDMA provider lifetime/detach contract.

Patch 013 fixes unregister-by-ID waiting for an RCU grace period inside its
own RCU read section. Registration/removal now serialize through a process
mutex, held through the grace period. Duplicate registration and ID exhaustion
are checked under that mutex. Removal tolerates never-registered or already
removed nodes and clears their links only after readers finish, allowing
reuse. Enable and diagnostic queries validate indices and traverse under RCU.
A pthread/UBSan fixture passes 100 blocked-reader removal/reuse races and 100
simultaneous duplicate-registration races, plus partial-batch rollback and
invalid-index checks. This models the lifetime contract; it does not execute
Linux RCU or replace a native Ethernet attachment/provider pin. A separate
disposable Linux 6.18.44 UML guest also passes 100 actual registration/removal
cycles while a kernel thread dispatches callbacks (162,513 in the recorded
run). `PROVE_RCU`, lockdep and atomic-sleep diagnostics are enabled; no RCU,
locking, BUG/Oops or panic report occurs. Only the registry routines are
included in this UML test module, with no optical driver or network device.

PHY/MAC builds retain warnings for unused vendor diagnostic code and missing
prototypes without treating those categories as errors. Implicit declarations,
type/format errors and unresolved symbols remain fatal. The imported MAC's
stack-frame warning exception also remains; runtime/stack auditing is pending.
All package patches apply to freshly prepared source. The complete vendor
package now builds against Linux 6.18.44 with no suppressed or unresolved
symbols. The artifact is
`bin/targets/airoha/an7581/packages/kmod-airoha-xpon-en757x-6.18.44-r10.apk`.
It remains gated by `BROKEN`, unselected and without autoload. No vendor
module or package has been installed or executed on the device.

Patch 010 removes the remaining nine linker dependencies by retiring OEM
interfaces that do not implement the Q1000K XGS-PON path:

| Former dependency | Resolution |
| --- | --- |
| `cmd_register`, `cmd_unregister`, `subcmd` | Omit the OEM kernel command interpreter and its registration calls. |
| `is_hwnat_dont_clean`, `wan_speed_test_hook` | Omit the OEM NAT debug command and packet speed-test interception; RX continues through the normal path. |
| `qdma_wan_fwd_timer`, `storm_ctrl_shrehold_wan` | Storm-control get/set return `-EOPNOTSUPP`, preserving ioctl errors and caller outputs. |
| `get_frame_engine_data`, `set_frame_engine_data` | Omit EPON setup that writes DSA registers; reject EPON FEC that accesses QDMA1. No raw FE compatibility mapper is supplied. |

Combined with the resource and identity changes, the previous 19-symbol
integration inventory is resolved. This is a link-complete module, not an
implemented packet path. Missing dynamic QDMA WAN/FE providers still cause
MAC startup to fail before hardware initialization. Host tests confirm the
unsupported controls do not access registers or publish successful output.

The new `airoha_ecnt_xpon` provider resolves the four PON resource symbols.
Its DT node remains disabled, and the provider has not been loaded.
The current Ethernet driver already owns both QDMA blocks and the FE; the
DSA switch owns its register region. The dynamic QDMA/FE hooks, imported
success-only flow helpers and `skb->cb` metadata need real integration even
after every linker error is resolved. See the [integration audit](XGSPON-INTEGRATION.q1000k.md).

## Build and use the diagnostics

Use an existing Q1000K build checkout with its feeds and toolchain prepared:

```sh
test "$(git branch --show-current)" = q1000k-xgspon
git merge-base --is-ancestor c526db0e25fa159ca79b60125741afd4e08440c9 HEAD
make -j8 package/network/utils/q1000k-xgspon/compile CONFIG_PACKAGE_q1000k-xgspon=m V=s
make -j8 package/luci-app-econet-xpon/compile CONFIG_PACKAGE_luci-app-econet-xpon=m CONFIG_PACKAGE_q1000k-xgspon=m V=s
make -j8 package/kernel/q1000k-pon-control/compile CONFIG_PACKAGE_kmod-q1000k-pon-control=m V=s
```

Run these top-level OpenWrt builds sequentially; concurrent invocations
race while regenerating shared package metadata. All pass using the local
GCC 14.4.0 musl toolchain. Outputs include
`bin/packages/aarch64_cortex-a53/base/q1000k-xgspon-2.apk`,
`bin/packages/aarch64_cortex-a53/base/luci-app-econet-xpon-1-r3.apk` and
`bin/targets/airoha/an7581/packages/kmod-q1000k-pon-control-6.18.44-r2.apk`.
LuCI depends on `luci-base` and `q1000k-xgspon`; it has no dependency on the
EN7528 kernel package or the broken vendor AN7581 package.

For a diagnostics image, add [q1000k-xgspon.config](q1000k-xgspon.config) to
an existing `quantum_q1000k-ubi` configuration on this branch and run
`make defconfig`. Pin the checkout revision for any shared test image. This
fragment selects the standalone controller, with no autoload, but leaves
the broken vendor PON stack unselected. The normal `q1000k-build`
repository/settings were not changed. No full XGS-PON image was produced.

After installing the diagnostics packages, the UI is Network → XGS-PON.
The CLI provides:

```sh
q1000k-xgspon status
q1000k-xgspon validate
q1000k-xgspon prepare
# On a matching kernel/DT with the optional module and verified firmware:
modprobe q1000k-pon-control
q1000k-xgspon detect
q1000k-xgspon initialize
q1000k-xgspon off
```

`status` returns schema version 1 with a `controller` object. In controller
release 2, status sampling never powers off, selects or writes controller
registers. A failed sample reports unknown MCU/TX fields and its errno; an
unexpected bit value is reported as sampled with `-EIO`. Shutdown requires
an explicit operation. Status polling is not a protection mechanism. LOS is null
until initialized and sampled by the driver. Registration, OMCI, service
readiness and optical measurements remain null. `validate` checks the selected
factory/override serial and MAC. Blank `/etc/config/q1000k-xgspon` overrides
use this unit's factory data. `prepare` requires valid factory calibration,
identity and the exact OEM firmware pair, then stages the 513-byte record
in a new mode-0700 RAM directory and prints that path. It does not activate
optics. Remove that temporary directory when finished. `detect` probes both
paths and powers off. `initialize` loads and verifies XGS-PON MD32 with
TX-disable asserted; it cleans its temporary calibration file on exit.
`off` disables both controllers. Use these bring-up commands with the fiber
disconnected until the remaining PHY/analog integration is validated.
See the [driver API and test instructions](../../../package/kernel/q1000k-pon-control/README.md).
`start`, `restart`
and `reload` explicitly fail until optical integration exists; no init
service is installed yet.

To supply local firmware from the user's extracted OEM image:

```sh
python3 scripts/q1000k-extract-pon-firmware.py \
  --squashfs ../QKX001-06.00.44.00.bin_extract/3566568-40229864.squashfs_v4_le \
  --output-dir /tmp/q1000k-pon-overlay
```

Choose a new output directory. The extractor invokes `unsquashfs -cat`,
runs no OEM executable, verifies both blobs before publication and emits
an OpenWrt overlay under `lib/firmware/airoha/q1000k/`. It does not copy the
overlay into the checkout or a device automatically. The validated baseline
is QKX001-06.00.44.00:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `A60993.elf.pm` | 15232 | `5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1` |
| `A60993.elf.dm` | 56 | `21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4` |

## Native packet transport checkpoint

Kernel patch 9997 adds a Q1000K-only GDM2 consumer API to the existing Ethernet
owner. The complete Linux 6.18.44 target build and board DT compilation pass;
all three public API symbols are exported. All 17 host PON fixtures pass. It carries explicit GEM/T-CONT/OMCI metadata through the native DMA path
and delivers raw RX before Ethernet parsing or offload. BUSY preserves the
caller's skb; DMA-map failure and cleanup have one packet owner. Ring and BQL
wakeups notify the consumer, and PON submissions always publish the doorbell.
Malformed fragmented RX is discarded as a whole, with bounded poll work.

Attachment and removal use RTNL/RCU, including a lower-stop notification and
stale software-assembly rejection. Local TX/RX fixtures pass, as do 100 cycles
with real Linux netdevices and RTNL/RCU in UML (1,538,479 RX/wake callbacks,
51 lower-detach notifications, no kernel diagnostics). GDM2 has an explicit
PON role property but remains disabled; its PHY connection and the vendor
adapter are still absent. Callback release does not drain hardware DMA or
turn off optics. See the [native transport contract](XGSPON-INTEGRATION.q1000k.md#native-ethernet-consumer-transport)
for ownership, limitations and the remaining integration work.

## TX drain checkpoint (2026-09-13)

Patch 9998 adds a bounded quiesce API and per-descriptor references. It closes
callbacks/TX, waits for native TX reclamation outside RTNL, reports timeouts,
and prevents reattachment while an old mapping remains. Release remains safe
with delayed completions because native descriptors retain internal storage
without invoking consumer callbacks. Mapping failures, normal completion and
cleanup all release the same ownership; duplicate notifications are ignored.

All 17 host tests pass, including native completion with mixed Ethernet/PON
traffic and out-of-order fragments. The UML test passes real wait/RTNL/RCU
execution with delayed completions, timeout/retry, 100 lifecycle cycles and
unregister/release before final completion. The full Linux 6.18.44 target
build passes and exports the new quiesce API. No device access was performed.
This does not establish optical/FE FIFO emptiness or DMA-stop recovery on
hardware. The [adapter findings](XGSPON-INTEGRATION.q1000k.md#vendor-packet-adapter-findings)
record the remaining vendor RX length, management framing, TX ownership and
NAPI mismatches. PON stays disabled.

## RX framing checkpoint (2026-09-13)

Vendor patch 014 adapts the Q1000K callback to populated native skbs. It validates
metadata and lengths before parser access, linearizes page fragments, bounds
OMCI reads, removes duplicate skb growth and preserves the raw management
header. Descriptor metadata determines management versus Ethernet traffic;
Ethernet-looking bytes in an OMCI payload no longer select the protocol.
Unknown checksums remain unverified, and missing/down upper interfaces drop
safely. Other vendor targets retain their existing receive contract.

All 18 host tests pass, including the actual vendor callback and all extended
length values. A real Linux UML packet-socket test preserves complete bytes
for three OMCI cases and one Ethernet frame, including fragmented input. The
six-module vendor package builds as release 11 without unresolved symbols or
autoload. These are framing/bounds checks; the native adapter, MIC/authentication,
PHY sequencing and optical/OMCI service are still pending. Device access remained
read-only; this continuation made no device connection.

## OMCI transmit framing checkpoint (2026-09-13)

Vendor patch 015 rejects truncated/overflowing OMCI TX frames, preserves skb
length/tail consistency when replacing a trailer, linearizes fragments and
reserves writable space even for clones. MIC-generation errors now fail the
send instead of returning success. Caller ownership is retained on failure.

All 19 host tests pass. The actual vendor MIC caller also passes 80 UML cases
using real skb trimming, expansion, clones and page fragments, with injected
provider/allocation/CMAC failures. Hardware DMA/CMAC are fixtures, so this does
not prove authentication or optical operation. The complete optional package
builds as release 12. Native TX/FE integration, PHY and OMCI service remain
pending. No device connection or firmware flash was performed.

## Native packet adapter checkpoint (2026-09-13)

Vendor patch 016 attaches to an explicitly named native PON lower before MAC
initialization. It replaces Q1000K legacy packet callbacks, shared DMA/IRQ
enable operations and vendor NAPI with the native consumer API. Lower detach
closes runtime readiness. Control/QoS and FE providers remain required.

TX uses a 128-packet FIFO, a one-second expiry and deferred BUSY retry with a
timer fallback. Metadata is copied separately from the skb control area, and
queued packets hold the submitting netdevice until consumed or dropped.
Unsupported offload/meter/account requests fail explicitly. Stop drains readers
and work before native quiesce/release and queue cleanup; timeout is reported.
Kernel patch 9999 retains the OMCI descriptor no-drop hint.

The full kernel and release-13 vendor package build. All 20 host PON tests pass.
A real Linux UML test covers backpressure, racing/missing wakeups, expiry,
upper unregister, lower detach, allocation failures and reclamation timeout.
Fifty producer/RX lifecycle cycles process over 800,000 packets with balanced
allocation/destruction counts and no kernel diagnostics. DMA is a fixture;
these tests do not establish optical traffic or hardware drain.

No SSH or device changes were needed. PON remains disabled pending native
control/FE, PHY, physical drain and OMCI service integration and hardware tests.

## Validation and remaining acceptance gates

- Eleven Python tests pass for the production C reader, shell backend, CLI and
  extraction logic. They cover invalid/truncated/duplicate records, unchanged
  inputs, firmware corruption, absent state, overrides, private staging and
  failed-stage cleanup, controller status types/schema and ambiguous discovery.
- Node tests pass for the production LuCI views, including missing data,
  LOS true/false, failed polling, unknown schema and identity validation.
- Both userspace packages and the standalone controller module build. The
  pinctrl patch applies and its objects compile; the board DTS compiles.
  The vendor BSP/PHY/MAC and complete package now build with no unresolved
  symbols. Host MMIO/regmap fixtures
  pass for bounds, absent providers, failures and masked shared SCU access.
- The production loader's host test checks layout, address spaces, endian
  behavior, readback mismatch and immediate failure at 15,388 I2C transfer
  points. No MCU enable occurs before successful full-memory verification.
  Additional tests sample all MCU/TX bit combinations and both read-failure
  points with no write/delay callbacks; failed samples clear stale values.
- Live factory/backend checks, both controller IDs, firmware/calibration
  loading and readback pass. Two consecutive CLI initialization/off cycles
  passed with the final module. Short calibration and corrupted firmware
  were rejected; detection during initialization returned `EBUSY` without
  interrupting it. Final cleanup restored the owned GPIO/mux bits and empty
  firmware-loader path, removed both temporary modules, and left LAN1 at
  1 Gbit/s. Optical service remains unimplemented.
- No browser QA, image flash, OLT registration, OMCI provisioning, optical
  traffic, reconnect/reboot reliability or accelerated PON traffic test has
  been claimed or completed.

Continue the [implementation plan](XGSPON.q1000k.md) with production DT
boot validation and remaining PHY/analog initialization, then resource/QDMA
integration and OMCI. Keep `pon_pcs` and `gdm2` disabled until their owner and
initialization order are implemented. Keep optical packages optional until
the bench, registration, service and recovery gates pass.
