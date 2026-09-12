# Airoha FlowSense for Q1000K

LuCI **Status → FlowSense** monitors PPE bound/unbound entries, CPU
load, Ethernet port traffic and upstream latency. Select
`CONFIG_PACKAGE_luci-app-airoha-flowsense=y` with the LuCI feed installed.
The Q1000K community build profile selects this package. The package declares
its `ip-full`, `ip-bridge`, `iputils-ping`, `tc-tiny`, `jshn`, `jsonfilter`
bridge-netfilter and bridge-hw-offload dependencies.

The Q1000K currently exposes `lan1` (1 GbE) and `lan2` (10 GbE), with PON
disabled. Only existing Ethernet interfaces appear. Wi-Fi gauges are hidden
when no wireless PHY is present. No MT7996 firmware is installed by this app;
firmware detection follows the AN7581 driver default or device-tree override.

Hardware flow offload can be configured here for routed and LAN bridge IP
traffic, matching firewall4's `flow_offloading` and `flow_offloading_hw`
settings. It remains disabled by default and preserves existing settings on
upgrade. The bridge-hw-offload service installs a bridge-family flowtable
for br-lan members when both flags are enabled. The updated kernel and
bridge-hw-offload package are required; installing only the app on an older
kernel cannot add this support. Inspect PPE bound entries under real traffic.
See `target/linux/airoha/BRIDGE-OFFLOAD.q1000k.md` for setup and diagnostics.
Offloaded traffic bypasses CAKE/SQM queueing, as indicated by the app.

Temperature and VLAN/PPPoE bridge-filtering controls are available in the
companion **SoC Status** page. High latency alone is not reported as proof
that offload has failed.

## Q1000K health indicators

**Integrity** uses the physical Ethernet interfaces in both bridge and router
mode. After a baseline poll it reports new RX/TX/CRC errors and drops,
with cumulative counters for context. CRC and RX errors may overlap, so
they are not added twice. Disconnected ports and unavailable statistics are
reported explicitly. A counter reset starts a new baseline.

**Latency** measures ICMP round-trip time to the current IPv4 default gateway,
falling back to an IPv6 gateway (with a scope ID for link-local addresses).
Use the target field to select a reachable IPv4/IPv6 address or hostname;
leave it empty for automatic gateway selection. Applying the target saves it
and enables/restarts monitoring, including on existing installations.
The foreground `npu-jitter` procd service sends one probe per loop with a
one-second reply timeout, followed by a two-second delay. It reports the
latest RTT, successful replies, loss over the last ten attempts, and the mean
absolute deviation of the replies in that window. Changing the automatically
selected gateway resets the window. No gateway, no reply, and missing/stale
sampler results are distinguished from a measured zero-millisecond RTT.
An ICMP-unresponsive target does not prove loss of forwarded traffic.

The UCI equivalent is:

```sh
uci set npu-monitor.jitter.target='192.168.1.254'
uci commit npu-monitor
/etc/init.d/npu-jitter enable
/etc/init.d/npu-jitter restart
```

Existing anonymous jitter sections are supported too. Configuration survives
sysupgrade; results are written atomically to `/tmp/npu-jitter.json` with a
monotonic timestamp. Results older than 15 seconds are not shown as live data.

**HW Buffer** retains the original community diagnostics and also displays
buffer occupancy:

- **PSE Δ**: observed increase in the ten internal PSE port drop counters
  (ports 0–9) since the previous poll.
- **CDM Δ**: observed increase in CDM1 + CDM2 RX hardware-forwarding drop
  counters since the previous poll. These are distinct from GDM RX drops.
- **PPE % BND**: `bound / (bound + unbound) × 100`, with both counts displayed.
  This is the bound share of observed entries, not binds per second, bandwidth
  offload percentage, or an alarm when idle. Empty tables show 0% (0/0);
  missing tables show N/A.
- **Occupancy**: shared PSE pages used divided by total minus reserved pages,
  plus used/capacity/free page counts. A zero occupancy snapshot does not mean
  that no packets were dropped between samples.

The card retains the community **DROPPING** warning when CDM Δ is positive
or PSE Δ exceeds 200 per poll. Smaller PSE deltas are displayed but do not
alone establish congestion: PSE includes internal discard paths. **HIGH**
means shared usage reached the configured threshold; **FULL** means no free
pages. Full buffers take priority over the drop warning. **OK** means neither
threshold nor drop warning was observed in a valid sampled interval.

The first sample is **SAMPLING**. Each counter has its own baseline so that
one counter decreasing cannot be hidden by another increasing. A decrease
(reset or wrap) marks that metric **reset**, discards its interval and starts
a new baseline. Missing/invalid data shows N/A; an older occupancy-only
kernel shows **PARTIAL** with an upgrade message. Polling is every five
seconds. It can miss short occupancy peaks and resets between samples;
these diagnostic deltas are not a lossless packet-loss accounting service.

Patches `9991-net-airoha-expose-pse-buffer-status.patch` and
`9993-net-airoha-expose-pse-cdm-drop-counters.patch` provide the root-readable
`/sys/kernel/debug/ppe/pse` snapshot, restricted to AN7581. Version 2 adds
`pse_drop0` through `pse_drop9`, `cdm1_hwf_drop` and `cdm2_hwf_drop`; the app
also accepts version 1 for occupancy. The driver reads each register once
per snapshot. CDM reads share the corresponding GDM statistics lock when
that port exists. No additional counter reset or register write is introduced.
The GDM packet counters accumulated/reset by the Ethernet driver remain
outside this interface.

The original community sampler and
[merbanan's AN7581 register map](https://github.com/merbanan/air_tools/blob/main/fe_reg.sh)
identify PSE shared status at FE offset `0x104`, PSE port drop counters at
`0x120 + 4 × port`, and CDM1/2 RXHWF_DROP at `0x5a4`/`0x15a4`.
Configured buffer-limit masks come from the existing driver. Neither the
backend nor the UI accesses `/dev/mem`.

The drop metrics require the updated kernel as well as app release 1.1.9-r9.
PPE percentage and occupancy remain available when installing the app over
the previous occupancy-capable kernel. `CONFIG_KERNEL_DEBUG_FS=y` is required
and selected by the Q1000K build. Kernel/register, backend and UI fixtures
check the integration; drop-counter behavior under real load still needs
physical Q1000K validation. To inspect the source values after upgrading:

```sh
cat /sys/kernel/debug/ppe/pse
ubus call luci.airoha_flowsense getFrameEngine
ubus call luci.airoha_flowsense getPpeEntries
```

## Origin

Created by [Gilly (@Gilly1970)](https://github.com/Gilly1970), based on the
Airoha status work by [Ryan Chen (@rchen14b)](https://github.com/rchen14b).
Imported from [c180c48b4e](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/c180c48b4e79ca9344a902bc3d9dcc4d4d57cbbd)
and [2e85bf63bc](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/2e85bf63bcfd5795cd6bccb5ee28e00d4e5bfc45),
with Q1000K adaptations in a separate commit. Apache-2.0; see `LICENSE`.
