# Q1000K app checks

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
counts, PSE snapshot validation, Ethernet counters, latency target persistence,
service restart failure, timestamp freshness, IPv4/IPv6 targets, zero RTT,
loss and jitter windows. Ping, routes, service control and UCI are mocked.

The Node tests execute the production LuCI views against RPC fixtures and a
minimal DOM. They check late CPU policy registration, current-frequency fallback,
control errors and kernel readback, temperatures, VLAN/PPPoE controls, target settings,
absent Wi-Fi, Ethernet error/drop deltas and resets, missing links, PSE
occupancy/high/full/missing states, and successful/unanswered/missing latency.

After preparing the target kernel, the PSE test compiles the actual driver
reader from that tree with a read-only register fixture. It checks register
addresses, masks, output fields and exactly one read per register. No write
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
