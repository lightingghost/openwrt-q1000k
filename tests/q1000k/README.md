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
python3 tests/q1000k/test_pon_identity.py
python3 tests/q1000k/test_pon_unsupported.py
python3 tests/q1000k/test_pon_crypto.py
```

These compile the production MAC MMIO accessors and prepared SCU regmap
helpers against host fixtures. They cover all register-window offsets,
misalignment, absent/removed providers, IRQ bounds, read/write failures,
full-write strobe semantics and masked updates that preserve unrelated bits.
The hook fixtures exercise actual dispatch and representative QDMA/FE wrappers
for absent/inactive providers, invalid indices, unhandled requests, provider
errors, success, copied outputs and TX ownership. They use UBSan and do not
claim concurrent-unregister or DMA execution coverage.
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
Set `Q1000K_PON_BSP` to test another prepared BSP source directory. Kernel
compilation checks the real APIs; these fixtures do not activate hardware or
establish reset/clock sequencing.

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
