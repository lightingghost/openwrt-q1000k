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
```

The Python tests run the production shell backends using BusyBox ash and
host builds of OpenWrt jsonfilter/jshn. Runtime paths are redirected to a
temporary fixture tree. They cover sensor values and absence, firmware
selection, validated/persisted bridge settings, CPU policy validation, PPE
counts and latency sampling with/without a gateway. Ping is mocked.

The Node tests execute the production LuCI views against RPC fixtures and a
minimal DOM. They check initial rendering and two poll cycles, temperature
updates, VLAN/PPPoE controls, absent Wi-Fi, the two Q1000K Ethernet ports and
unavailable frame-engine counters. They do not replace browser or hardware QA.
