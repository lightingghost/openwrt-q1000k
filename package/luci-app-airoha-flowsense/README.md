# Airoha FlowSense for Q1000K

LuCI **Status → FlowSense** monitors PPE bound/unbound entries, CPU
load, Ethernet port traffic and upstream latency. Select
`CONFIG_PACKAGE_luci-app-airoha-flowsense=y` with the LuCI feed installed.
The Q1000K community build profile selects this package. The package declares
its `ip-full`, `ip-bridge`, `iputils-ping`, `tc-tiny`, `jshn`, `jsonfilter`
and bridge-netfilter dependencies.

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

**HW Buffer** displays AN7581 PSE shared-buffer occupancy and free pages.
Patch `9991-net-airoha-expose-pse-buffer-status.patch` adds the root-readable
`/sys/kernel/debug/ppe/pse` interface. The driver reads the PSE status register
once per snapshot and reports configured total/reserved pages and the hardware
high threshold. The card shows shared usage as a percentage of total minus
reserved pages; HIGH means the shared-use threshold was reached and FULL
means no free pages. These are snapshots, not a peak recorder or a packet-drop
counter. Polling can miss brief bursts.

The PSE status offset `0x104` and used/free decoding match the community
AN7581 sampler; the offset is also named PSE_SHARE_BUF_STA in
[merbanan's register map](https://github.com/merbanan/air_tools/blob/main/fe_reg.sh).
The configured limits use the masks already used by the kernel driver.
Access is restricted to the AN7581 driver. It does not access `/dev/mem`,
write hardware registers, or sample/reset GDM/CDM MIB counters.
The original app's GDM TX_DROP and the driver's TX_ETH_DROP offsets refer to
different definitions; they are not substituted into this occupancy metric.

HW Buffer requires the new kernel as well as the updated app; installing
only the app on an older image reports that requirement. The read-only
interface requires `CONFIG_KERNEL_DEBUG_FS=y` (enabled by the Q1000K build).
A complete image build and automated register/backend/UI fixtures validate
the integration; physical readings still need verification on a Q1000K.

## Origin

Created by [Gilly (@Gilly1970)](https://github.com/Gilly1970), based on the
Airoha status work by [Ryan Chen (@rchen14b)](https://github.com/rchen14b).
Imported from [c180c48b4e](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/c180c48b4e79ca9344a902bc3d9dcc4d4d57cbbd)
and [2e85bf63bc](https://github.com/OpenWRT-fanboy/OpenW1700k/commit/2e85bf63bcfd5795cd6bccb5ee28e00d4e5bfc45),
with Q1000K adaptations in a separate commit. Apache-2.0; see `LICENSE`.
