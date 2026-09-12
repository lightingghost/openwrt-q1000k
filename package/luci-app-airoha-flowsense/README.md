# Airoha FlowSense for Q1000K

LuCI **Status → Airoha FlowSense** monitors PPE bound/unbound entries, CPU
load, Ethernet port traffic and upstream latency. Select
`CONFIG_PACKAGE_luci-app-airoha-flowsense=y` with the LuCI feed installed.
The Q1000K community build profile selects this package. The package declares
its `ip-full`, `ip-bridge`, `tc-tiny`, `jsonfilter` and bridge-netfilter dependencies.

The Q1000K currently exposes `lan1` (1 GbE) and `lan2` (10 GbE), with PON
disabled. Only existing Ethernet interfaces appear. Wi-Fi gauges are hidden
when no wireless PHY is present. No MT7996 firmware is installed by this app;
firmware detection follows the AN7581 driver default or device-tree override.

Hardware flow offload can be configured here for routed traffic, matching
firewall4's `flow_offloading` and `flow_offloading_hw` settings. It remains
disabled by default. This tree does not include the experimental standalone
bridge-offload kernel series. A LAN bridge is therefore not proof of an
accelerated forwarding path; inspect PPE bound entries under real traffic.
Offloaded traffic bypasses CAKE/SQM queueing, as indicated by the app.

Temperature and VLAN/PPPoE bridge-filtering controls are available in the
companion **SoC Status** page. Raw frame-engine counters are unavailable:
this adaptation avoids direct register reads and writes. High latency alone
is not reported as proof that offload has failed.

The `npu-jitter` service samples CPU load and pings the current IPv4 default
gateway every two seconds. With no route it reports latency unavailable and
continues sampling CPU load. No external target is selected by default.
To select a host explicitly:

```sh
uci set npu-monitor.jitter.target='192.168.1.254'
uci commit npu-monitor
/etc/init.d/npu-jitter restart
```

Use an empty target for automatic gateway discovery. The config survives
sysupgrade. The daemon validates target characters and writes its result
atomically to `/tmp/npu-jitter.json`.

## Origin

Created by [Gilly (@Gilly1970)](https://github.com/Gilly1970), based on the
Airoha status work by [Ryan Chen (@rchen14b)](https://github.com/rchen14b).
Imported from [c180c48b4e](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/c180c48b4e79ca9344a902bc3d9dcc4d4d57cbbd)
and [2e85bf63bc](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/2e85bf63bcfd5795cd6bccb5ee28e00d4e5bfc45),
with Q1000K adaptations in a separate commit. Apache-2.0; see `LICENSE`.
