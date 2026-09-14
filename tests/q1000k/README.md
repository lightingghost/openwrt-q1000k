# Q1000K app checks

## XGS-PON development

```sh
python3 tests/q1000k/test_xgspon.py
node tests/q1000k/test_xgspon_views.cjs
```

These require a C compiler, Python 3, Node.js, BusyBox and the OpenWrt host
`jshn` tool/library. The tests compile the production factory reader and
exercise the production shell backend and CLI. They cover exact factory/DSD
fields, 513-byte calibration preservation, malformed/truncated/duplicate
records, read-only inputs, symlink rejection, identity overrides, unknown
optical states, firmware integrity, private RAM staging and cleanup on a
failed read. Extraction tests check complete publication and rejection of
wrong firmware; synthetic firmware hashes replace the OEM hashes only inside
fixtures. No OEM firmware or device-specific calibration is part of the tests.

The LuCI fixtures exercise unavailable/LOS states, failed refreshes, schema
validation and identity validation. They do not establish optical service or
replace browser testing. See the [implementation report](../../target/linux/airoha/XGSPON-STATUS.q1000k.md)
for package builds and the live read-only Q1000K checks.

After preparing the v2 vendor PON package, run the resource tests with:

```sh
python3 tests/q1000k/test_pon_resources.py
python3 tests/q1000k/test_pon_hooks.py
python3 tests/q1000k/test_pon_hook_lifecycle.py
python3 tests/q1000k/test_pon_identity.py
python3 tests/q1000k/test_pon_unsupported.py
python3 tests/q1000k/test_pon_crypto.py
python3 tests/q1000k/test_pon_lifecycle.py
python3 tests/q1000k/test_pon_packet.py
python3 tests/q1000k/test_pon_omci_tx.py
python3 tests/q1000k/test_pon_adapter.py
python3 tests/q1000k/test_pon_tcont.py
python3 tests/q1000k/test_pon_gem.py
```

These compile the production MAC MMIO accessors and prepared SCU regmap
helpers against host fixtures. They cover all register-window offsets,
misalignment, absent/removed providers, IRQ bounds, read/write failures,
full-write strobe semantics and masked updates that preserve unrelated bits.
The hook fixtures exercise actual dispatch and representative QDMA/FE wrappers
for absent/inactive providers, invalid indices, unhandled requests, provider
errors, success, copied outputs and TX ownership. They use UBSan and do not
claim concurrent-unregister or DMA execution coverage.
The separate hook lifecycle fixture runs the production registration/removal
code with pthread readers and atomic list links. It checks 100 blocked-reader
removal/reuse races, 100 concurrent duplicate registrations, partial batch
rollback, ID exhaustion, invalid indices and inactive providers. Its grace
period fixture rejects waiting from within an RCU read section. This is a
host concurrency model, not execution of the Linux RCU implementation.
For a separate test with real Linux RCU, run
`tests/q1000k/run_pon_hook_uml.sh`. It builds an isolated UML guest with
`PROVE_RCU`, lockdep and atomic-sleep diagnostics, then runs 100 hook
registration/removal cycles concurrently with a dispatching kernel thread.
Success requires a zero test exit and no kernel/RCU/locking diagnostics. Only
the extracted registry is loaded in the guest; its host filesystem is read
only, and no network or optical device is configured. Like the crypto runner
below, it accepts `Q1000K_UML_BASE` to copy an existing UML build cache.
The identity fixtures exercise the production parameter/cache implementation
and MAC callers for exact input lengths, malformed MAC/FSAN, missing values,
byte order, immutable copies and unchanged outputs on failure.
Unsupported-control fixtures verify the actual storm/FEC entry points and
storm ioctl dispatch return an error without MMIO or user-output writes.
The crypto fixture requires OpenSSL 3 development headers/libcrypto. It runs
the production AES/CMAC helpers using OpenSSL AES, verifies NIST examples and
33,411 fragmented vectors against independent OpenSSL CMAC, and injects errors
at each crypto operation. It also verifies allocation failure/retry, duplicate
initialization and timer shutdown before transform release.
ASan/UBSan remain enabled; LeakSanitizer is disabled
because it cannot run under the workspace ptrace sandbox. This does not execute
the kernel crypto provider or prove the optical authentication/key lifecycle.
The lifecycle fixtures run extracted startup, teardown, cdev, WAN and worker
functions. They inject failures at all 17 startup stages and each of the four
WAN interface creations, retry after failure, verify cdev allocation/publication
ownership, and check crypto setup precedes interrupt enable. A pthread harness
executes 201 worker start/stop cycles with idle, full and active queues. These
tests use UBSan and verify software ownership/order; they do not prove hardware
rollback, native QDMA attachment, provider removal or real kernel RCU behavior.
Set `Q1000K_PON_BSP` to test another prepared BSP source directory. Kernel
compilation checks the real APIs; these fixtures do not activate hardware or
establish reset/clock sequencing.

After preparing the target kernel with native PON patch 9997:

```sh
python3 tests/q1000k/test_pon_transport.py
python3 tests/q1000k/test_pon_dma.py
tests/q1000k/run_pon_transport_uml.sh
```

The transport fixture covers real attachment, metadata encode/decode, callback
ownership, generations and BUSY behavior. The DMA fixtures compile the actual
native TX/RX functions, TX completion and cleanup against DMA/page/ring fixtures. They
exercise descriptor fields, all mapping failures for zero through three
fragments, doorbell publication, cleanup, out-of-order/duplicate completion,
mixed Ethernet/PON ownership, raw frame lengths, ordinary Ethernet
behavior, damaged fragment chains, allocation failure and an all-invalid ring.
`Q1000K_PON_ETH` can select a stable prepared Ethernet source directory. Do not
run against a kernel tree while a build is cleaning/reapplying its patches.

The UML transport test uses real Linux netdevice registration/stop/unregister,
RTNL, RCU and skb/queue APIs; its hardware-owner storage and DMA submission are
fixtures. It runs 100 attach/release/stop cycles with concurrent RX/TX/wake
activity and unregisters the lower netdevice while a consumer still owns its
handle. Synthetic DMA completions are delayed to exercise real quiesce waits,
bounded timeout/retry and release before final completion. It requires no
kernel/RCU/locking diagnostics and a zero test exit.
It uses the same read-only hostfs and UML kernel guards as the other runners;
only a synthetic netdevice exists, with no attached NIC or optical hardware.
`Q1000K_UML_BASE` reuses a UML build cache. When overriding `Q1000K_PON_ETH`,
`Q1000K_PON_HEADER` may select the matching public header. These tests do not
prove physical DMA drain, padding/CRC, MIC authentication or optical operation.

The packet fixture runs the actual prepared vendor RX callback with the new
Q1000K framing helpers. It covers populated lengths, fragmented input, raw OMCI
versus Ethernet parsing, loopback, missing/down interfaces, ownership on failure
and all 65,536 extended length values with each MIC flag. VLAN/flow hooks and
GEM selection are fixtures; no crypto/MMIO or optical operation is invoked.

```sh
tests/q1000k/run_pon_packet_uml.sh
```

This separate UML test executes the production framing helpers through real
Linux skbs, page-fragment linearization, receive backlog and an AF_PACKET
socket on a synthetic netdevice. It checks exact raw bytes for baseline and
extended OMCI plus Ethernet, including fragmented input. It accepts the same
`Q1000K_UML_BASE` cache option, mounts hostfs read-only and attaches no physical
NIC. It does not validate MIC/authentication, OMCI provisioning or hardware.

The OMCI TX fixture runs the production framing helper and prepared vendor
MIC caller. It checks all extended length values, complete versus truncated
frames, optional trailers, buffer expansion, and allocation/CMAC failures.

```sh
tests/q1000k/run_pon_omci_tx_uml.sh
```

This UML runner executes those same routines with real Linux skbs, clones,
page fragments, trimming and tail expansion. Its 80 cases combine baseline
and extended frames, linear/fragmented and cloned input, presence/absence of
a trailer, success, and missing-provider/allocation/CMAC failures. DMA and
hardware CMAC are fixtures; the test validates ownership, framing and error
propagation, not a cryptographic result, key selection or optical operation.
It accepts `Q1000K_UML_BASE` and uses the same read-only hostfs/UML guards.

The vendor adapter TX fixture runs the actual prepared `pwan_net_start_xmit()`.
It checks one free/consumer per packet, native queue rejection, padding and
early failures; accepted or dropped packets never return Linux BUSY or errno
from the ndo after their contents have been modified.

```sh
tests/q1000k/run_pon_adapter_uml.sh
```

This UML test runs the production adapter with real workqueues, RTNL/RCU,
netdevices and skbs against a synthetic native provider. It covers metadata
translation, a full 128-packet FIFO, wakeup/BUSY races, retry without a new wake,
expiry, upper unregister while queued, early callbacks, allocation/attach
failures, lower detach, and release following quiesce timeout. Concurrent TX/RX
exercise 50 attachment lifecycles; allocation/destruction counts must balance
with no kernel diagnostics. Native DMA is a fixture and has no physical NIC.
The existing transport/DMA fixtures separately exercise the native owner's
implementation. Use `Q1000K_UML_BASE` for the same optional build cache.

The transport host fixture now checks exclusive QDMA1 reservation, all 32
channels and all 256 queue-close masks, preserved neighbouring bytes, failed
MMIO readback, unchanged output arguments and admission epochs across closure
and reopening. The UML transport test races native queue controls against TX
and detach. The UML adapter test verifies closed-queue rejection and that a
BUSY retry retains its original epoch across reopening. Register and DMA
behavior are fixtures; these tests cannot establish physical FIFO retirement.
`Q1000K_PON_HEADER` can select the matching native public header for either
UML generator when testing a separately prepared driver tree.

The native tests also cover per-channel descriptor reclamation: all 32 encoded
channel IDs, every fragment mapping failure, mixed-channel/out-of-order and
duplicate completions, permanent closure, pending polls and errors after
disconnect/readback failure. The native UML guest races channel retirement
against TX and reopening, and executes a channel operation from real interrupt
context. The adapter guest checks that its RCU wrapper preserves native errors.

Native FE tests cover all 32 GDM2 TX-enable bits and preservation of other
channels. Attachment rejects loopback and verifies all-off before publication;
enable requires closed queues and reclaimed native mappings. Fault injection
covers ignored enable/disable writes, mismatched readback and QDMA-close
failure. A failed disable cannot claim FE stopped; later disable attempts
all-off without clearing the fault. The native UML guest exercises FE control
in hard-IRQ context and alongside TX, queue closure and detach; the adapter
guest verifies exact error forwarding under RCU. These are modeled FE
registers, not evidence of physical FIFO or optical retirement.

All PON UML runners explicitly reject kernel diagnostics; a negated `grep`
alone does not trigger `set -e` and is insufficient as a failure check.

The T-CONT host tests execute the production table helper and prepared vendor
transaction/caller functions. They check reserved command bits, channel/ID
bounds, unchanged outputs, stale invalid entries, duplicate preservation,
concurrent allocation, permanent quarantine, all 33 command timeout points
and write verification. Setup fixtures inject failures at each MAC/native/FE
stage and during rollback, including reentrant control requests. Removal must
preserve bindings/counts and report incomplete retirement; reset guards must
stop before clearing identity or touching the remaining reset sequence.

For the table helper with real Linux spinlocks, IRQ state and kthreads:

```sh
tests/q1000k/run_pon_tcont_uml.sh
```

This separate UML guest uses two emulated registers and no physical hardware.
It checks timeout fault latching, verification and quarantine, and races 64
allocation callers. The guest mounts the host filesystem read-only, enables
lockdep/atomic-sleep diagnostics and accepts `Q1000K_UML_BASE` as a build cache.
Never execute its generated init script on the host. These tests establish
software behavior only; they do not validate physical command completion,
FIFO emptiness, channel retirement or optical service.

The GEM tests execute the production command helper, raw ABI callers, binding
registry, XMCS entry points and TX/RX mapping consumers. They cover the complete
ID field, all 256 software slots, type-bit polarity, reserved bits, compare
failures, all three timeout stages, readback faults, output preservation,
concurrent publication, staged T-CONT/ANI setup, malformed channels and permanent
retirement. Hook fixtures change packet metadata or retire a binding between
lookup and transmission to verify snapshot ownership and rejection. Reset
fixtures also require pending/multicast GEM retirement before identity reset.

For the GEM commands and binding registry with real Linux locks and IRQs:

```sh
tests/q1000k/run_pon_gem_uml.sh
```

This guest runs concurrent creators/readers, all 256 GEM slots and control from
real hard-IRQ context. MAC registers and native drain are modeled; Linux
provides spinlocks, atomics, kthreads, IRQ work and lockdep. It uses read-only
hostfs, supports `Q1000K_UML_BASE`, and rejects kernel diagnostics. Never execute
its generated init script on the host. No physical device is contacted.

To execute the same cryptographic helpers with the real Linux crypto API:

```sh
tests/q1000k/run_pon_crypto_uml.sh
```

This builds an independent User Mode Linux kernel and a test-only module,
then runs AES/CMAC known-answer tests in that guest. It requires native kernel
build tools, Python 3 and static `/usr/bin/busybox`; UML needs permission to
ptrace its own child processes. The guest mounts the host filesystem read-only
and contains no optical driver or network-device connection. Both the guest
init script and module reject a normal host/router kernel. Do not invoke the
generated init script directly. Logs stay in the printed `/tmp` directory.
Set `Q1000K_UML_BASE` to reuse a matching prepared UML build; the script clones
its build directory and leaves the original configuration/source unchanged.

From the OpenWrt repository, after building its host tools:

```sh
mkdir -p /tmp/q1000k-app-test-bin
cc -D_GNU_SOURCE -DJSONC -I staging_dir/host/include -I staging_dir/host/include/json-c \
  build_dir/target-aarch64_cortex-a53_musl/jsonfilter-*/{main,ast,lexer,parser,matcher}.c \
  staging_dir/host/lib/libubox.a staging_dir/host/lib/libjson-c.a -lm \
  -o /tmp/q1000k-app-test-bin/jsonfilter
PATH=/tmp/q1000k-app-test-bin:$PATH python3 tests/q1000k/test_apps.py
node tests/q1000k/test_views.cjs
python3 tests/q1000k/test_pse.py
python3 tests/q1000k/test_cpufreq.py
```

The Python tests run the production shell backends using BusyBox ash and
host builds of OpenWrt jsonfilter/jshn. Runtime paths are redirected to a
temporary fixture tree. They cover sensor values and absence, firmware
selection, validated/persisted bridge settings, CPU policy discovery, fallback frequency tables, validated governor/max-frequency controls, PPE
counts and missing-table handling, version 1/2 PSE snapshot validation,
32-bit PSE/CDM counter values, Ethernet counters, latency target persistence,
service restart failure, timestamp freshness, IPv4/IPv6 targets, zero RTT,
loss and jitter windows. Ping, routes, service control and UCI are mocked.

The Node tests execute the production LuCI views against RPC fixtures and a
minimal DOM. They check late CPU policy registration, current-frequency fallback,
control errors and kernel readback, temperatures, VLAN/PPPoE controls, target settings,
absent Wi-Fi, Ethernet error/drop deltas and resets, missing links, PSE
occupancy/high/full/missing states, PSE/CDM drop thresholds and per-counter
resets, first/missing samples, PPE bound percentages and empty/unavailable
tables, and successful/unanswered/missing latency.

After preparing the target kernel, the PSE test compiles the actual driver
reader from that tree with a read-only register fixture. It checks register
addresses, masks, all ten PSE and two CDM drop fields, unsigned 32-bit
values and exactly one read per register. It verifies CDM locking both with
and without the corresponding GDM ports. No write
accessor is supplied. The target kernel build also checks the real kernel APIs.
These tests do not replace browser or physical-hardware QA.

The CPUFreq test compiles the actual prepared PM-domain driver routines with
SMC and ordered MMIO fixtures. It checks invalid firmware responses,
fractional PLL rates and post-dividers, every stock OPP, no-op/invalid requests,
all-CPU synchronization, backup-clock ordering, unrelated-bit preservation,
and failed reads/writes/source changes. It asserts that the active clock path
is never disabled and the PLL is never changed while it is the CPU's source.
It cannot establish physical clock-lock timing, voltage margins or firmware
behavior on hardware.

## Bridge flow offload

```sh
python3 tests/q1000k/test_bridge_offload.py
python3 tests/q1000k/test_bridge_kernel.py
```

The service tests run the production shell script with real flock and temporary
files, replacing only system paths and UCI/nft/firewall/logger commands. They
cover both enable flags, early boot, removed/changed ports, safe device names,
rule validation failure and firewall reload rollback.

The kernel fixtures compile the prepared kernel's actual refragmentation
function and PPE header-preservation block. They check exactly one packet
owner for every error/success path, unchanged routed entries, the TTL bit,
IPv6 source-MAC selector and preservation of unrelated bits/PPPoE IDs.

For actual packet forwarding through the patched kernel on an x86-64 host:

```sh
tests/q1000k/run_bridge_uml.sh
```

This requires native kernel build tools, static `/usr/bin/busybox`, Python 3,
iproute2 (ip and bridge), nftables and iperf3. It copies the prepared source
into a separate /tmp directory, builds User Mode Linux, and boots it as the
current user. UML needs permission to ptrace its own child processes. The
host filesystem is mounted read-only in the guest; /tmp, /run, /proc, /sys
and /dev are guest mounts. Do not run the init script directly: its kernel
release guard restricts it to the disposable test kernel.

The seven runtime tests exercise IPv4/IPv6 UDP and TCP, both flow directions,
TTL/hop-limit and changing DSCP values, TTL=1 forwarding, IPv4 fragmentation,
VLAN access ports, and routed forwarding through a bridge port. They inspect
conntrack and require the slow-path counter to stop increasing for ordinary
UDP flows. VLAN trunks/PPPoE, hardware PPE execution and performance still
require Q1000K hardware testing. Build/run logs remain in the printed /tmp
path. Success requires test exit 0 and no kernel BUG/Oops/panic.

The AN7581 optical PHY provider and Q1000K lifecycle have dedicated fixtures:

```sh
python3 tests/q1000k/test_pon_phy_resources.py
python3 tests/q1000k/test_pon_phy_lifecycle.py
tests/q1000k/run_pon_phy_uml.sh
```

The host tests compile production code with UBSan and inject MMIO, startup
and IRQ failures. The UML test uses real kernel mutexes, RCU, workqueues,
timers and kthreads, with synthetic PHY state, MMIO and IRQ acquisition. It
checks reentry and concurrent callback teardown over 50 start/stop cycles.
No optical hardware is accessed. `Q1000K_UML_BASE` may point to an existing
compatible UML build directory to reuse the local build artifacts.

`test_pon_reset.py` verifies the actual clock reset callbacks and regmap errors.
`test_pon_phy_reset.py` verifies the selected top/PMA reset sequences, bounded
SCU fields and failures before reset, during reset and during restoration.
`test_pon_phy_resources.py` also checks exclusive reset status and exclusion
of concurrent MMIO. `Q1000K_PON_BSP` can select a prepared vendor BSP tree.
