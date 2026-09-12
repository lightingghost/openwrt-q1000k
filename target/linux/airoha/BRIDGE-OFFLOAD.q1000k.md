# Q1000K bridge IP flow offload

Q1000K LAN1 is the internal switch's 1 GbE user port through GDM1. LAN2 is
an RTL8261N 10 GbE port through GDM4. Same-subnet traffic crosses these
paths through br-lan. The standalone bridge flowtable can accelerate
established IPv4/IPv6 TCP and bidirectional UDP connections, including a
client-to-client iperf test. LAN1 still limits that test to its 1 GbE link.
ARP, multicast/broadcast, non-IP traffic, unsupported encapsulations and
packets that require the slow path remain with the normal bridge. This is
IP connection acceleration, not a replacement for every bridge operation.

## Enable and inspect

Install the complete updated Q1000K firmware, including its matching kernel
modules. The Q1000K device selects bridge-hw-offload, which depends on
kmod-nft-bridge (including nf_conntrack_bridge), kmod-nft-offload, firewall4
and flock. FlowSense 1.1.9-r10 also selects the service. No stock snapshot
module ABI is substituted.

Enable **Status → FlowSense → HW Flow Offload**, or use:

```sh
uci set firewall.@defaults[0].flow_offloading='1'
uci set firewall.@defaults[0].flow_offloading_hw='1'
uci commit firewall
/etc/init.d/bridge-hw-offload enable
/etc/init.d/bridge-hw-offload restart
```

Existing firewall preferences are retained; the firmware does not force
hardware acceleration on at first boot or overwrite a disabled setting.
Bridge IP filtering and the SoC Status VLAN/PPPoE filtering switches are
not required. They control the separate br_netfilter compatibility hooks.
Offloaded flows bypass later packet hooks and software queueing such as SQM.

The service discovers br-lan members and validates its proposed table before
installing a runtime fragment. firewall4 applies it with the main ruleset
in one nftables transaction. Kernel validation and firewall reload failures
are reported to stderr and logread, with the previous fragment restored on
reload failure. A missing fragment at early boot is valid. Disabling either
offload flag or removing the second bridge member removes the bridge table.
Boot, LAN interface updates and firewall/network configuration events
refresh the rules. No generated rules are written persistently to flash.

During a fresh 30-second client-to-client iperf run, inspect:

```sh
nft list table bridge fw4
cat /sys/kernel/debug/ppe/bind
grep -E 'sport=5201|dport=5201' /proc/net/nf_conntrack
ubus call luci.airoha_flowsense getStatus
logread -e bridge-hw-offload
dmesg | grep -Ei 'airoha|npu|ppe|firmware'
```

Expect `br_offload`, `devices = { lan1, lan2 }`, `flags offload`, and a bridge
forward rule for established TCP/UDP flows. Matching PPE BND entries and
`[HW_OFFLOAD]` in conntrack distinguish hardware forwarding from software
`[OFFLOAD]`. A configured table, a loaded NPU driver, or a single offload
percentage is insufficient proof. Initial packets still use the CPU.
FlowSense's PPE bound percentage is BND/(BND+UNB), not binds per second.
Test both iperf directions; run the server on a client, not the router.

If the table is missing, restart bridge-hw-offload and inspect its reported
error. If it exists but no flows offload, check `bridge link show`, `bridge
vlan show`, both offload flags, established bidirectional traffic and module
loading. If conntrack says OFFLOAD without HW_OFFLOAD, the software path is
working but the driver has not accepted/programmed that flow in hardware.

## Imported changes and corrections

Original commits are preserved on q1000k-dev:

- `57c9119d3d790e0cc9bc69adcd66733db8caf653` → `aaf8d452a1`:
  bridge flowtable, path helpers and encapsulated bridge conntrack.
- `947e9866808264a37396d1523fb933210e1af0df` → `f952558b55`:
  bridge-hw-offload package and AN7581 selection.

Airoha patches 9994/9995 apply after the existing DSCP offload changes:

- Let netfilter free a packet returned with NF_DROP; do not free it twice.
  Also avoid releasing route references again after flow initialization
  transferred their ownership on a failed flow-table insertion.
- Select the bridge path only for NFPROTO_BRIDGE, using hook ingress and
  egress devices on the same bridge. Do not infer ingress from skb->dev,
  which is already the egress port in the bridge forward hook.
- Require resolved ports in the selected flowtable and walk both routed
  directions even if the first neighbour lookup fails.
- Preserve TTL/hop limit and per-packet DSCP in the software bridge path.
  Routed forwarding retains its existing TTL and DSCP behavior.
- Emit an explicit kernel-internal TTL_KEEP action for bridged hardware
  rules. Airoha clears the PPE TTL-decrement bit and uses IPv6 source-MAC
  selector 0xf, as its existing L2 subflow code does. Other drivers without
  support reject the new action and retain software forwarding.
- Validate PPPoE headers relative to skb->data, run conntrack with data at
  the inner IP header, and restore the encapsulation before forwarding.
  Unsupported encapsulations are not admitted to the bridge flowtable.

## Validation limits

Service fixtures cover enable/disable, missing and changed bridge members,
invalid names, kernel rule rejection and failed firewall reload rollback.
A separate User Mode Linux build runs the actual patched netfilter/bridge
code with veth ports and private namespaces. It exercises IPv4/IPv6 TCP and
UDP, TTL/hop-limit/DSCP preservation, low TTL, IPv4 fragmentation, VLAN
access ports and routed forwarding through a bridge port. Rule counter
changes check that traffic actually bypasses the slow path, rather than
merely acquiring an OFFLOAD flag.

These checks cannot validate AN7581 hardware execution. Physical Q1000K
checks remain: matching PPE bindings in both directions, throughput and CPU
load, received TTL/hop limit and MAC addresses, cold boot, saved-setting
sysupgrade and VLAN trunk/PPPoE behavior. No device is flashed by this work.
