# Q1000K passthrough integration

This package adds the missing IPv4 WAN-event integration around the already
qualified Linux policy-routing, proxy-ARP, nftables and dnsmasq configuration.
It defaults to **router** mode. It does not change optical identity, netifd
topology, IPv6 lease ownership or the existing PON/PPE implementation. An
explicit one-shot IPv6 setup helper repairs netifd-owned address/connected-route
state during pool changes; the IPv4 service never calls it on WAN events.

IPv6 uses native odhcpd, with the user-selected **/61** delegated from the ISP
/60. Kea and a custom DHCPv6 server are not dependencies. See the workspace
learning `learnings/q1000k-odhcpd-61-plan-20260925.md` for rationale and the
hardware qualification boundary.

## Configuration and activation

The supported initial IPv4 profile has `wan` on routed `pon`, `lan` on
`br-lan`, the usual `wan` and `lan` firewall zones, one dnsmasq instance with
its default confdir, and standard IPv4 policy rules. Custom policy routing,
packet marks, DHCP directories or a concurrent bench owner are rejected for
review. Keep a fixed IPv4 management address on br-lan and a different subnet
on the main router's LAN. Both devices retain their own Ethernet MACs.

After installing the package, apply the existing IPv6 configuration as part
of the reviewed network setup. These settings are for the **Q1000K**:

```sh
# Capture the current netifd-owned assignments BEFORE changing configuration.
# Use a new directory; retain it until the transition or rollback is verified.
transition=/var/run/xgspon-ipv6-setup
xgspon-ipv6-transition snapshot "$transition"
cp -p /etc/config/network "$transition/network"
cp -p /etc/config/dhcp "$transition/dhcp"

# The old profile reserved a /64 on wan6; free that reservation so netifd can
# give the LAN the complete /60 pool. Preserve the working WAN DUID/IAID and
# reqprefix value/suffix, confirming the ISP actually supplies /60.
uci -q delete network.wan6.ip6assign || :
uci -q delete network.wan6.ip6hint || :
uci set network.lan.ip6assign='60'
uci set network.lan.ip6hint='0'
uci -q delete network.lan.ip6class || :
uci add_list network.lan.ip6class='wan6'
uci set dhcp.lan.ra='server'
uci set dhcp.lan.dhcpv6='server'
uci set dhcp.lan.ndp='disabled'
uci set dhcp.lan.dhcpv6_pd='1'
uci set dhcp.lan.dhcpv6_pd_min_len='61'
uci commit network
uci commit dhcp
/etc/init.d/network reload
xgspon-ipv6-transition apply "$transition"
/etc/init.d/odhcpd restart
```

These are configuration steps, not commands run by package installation.
Apply them during a planned network transition with fixed IPv4 management and
a working upstream /60. Stop if any command fails. Confirm both `ifstatus lan`
and `ip -6 address show dev br-lan` report the local pool address with **/60**;
confirm the retired WAN-side /64 address **and connected route** are gone.
The RA still advertises a /64 transit subnet. A RAM boot does not preserve UCI
configuration across a fresh RAM image boot.

The September 25 live bench showed why a reload alone is insufficient: replacing
an IPv6 address with the same address and a different prefix length retained
the old kernel mask, and deprecating the removed WAN assignment left an
overlapping /64 route. The helper records only netifd's pool assignments,
waits for status to match committed assignment lengths, and retires only those
obsolete addresses and their kernel connected routes. It then installs missing
current pool addresses with the current lease's remaining lifetimes and
`noprefixroute`. Static netifd routes and odhcpd's delegated routes are preserved.
It does not change UCI, restart WAN/PON, run in the background, or allocate PD.

This helper supports the reviewed routed `pon`/`br-lan` profile, one /60 or /64
pool per interface, and no unrelated global addresses on those devices. It
refuses other global addresses before mutation instead of guessing ownership;
custom IA_NA/manual-address topologies need review. Keep its RAM snapshot for
retry/rollback. To restore the captured configuration after a failed setup:

```sh
uci -q revert network
uci -q revert dhcp
cp -p "$transition/network" /etc/config/network
cp -p "$transition/dhcp" /etc/config/dhcp
/etc/init.d/network reload
xgspon-ipv6-transition apply "$transition"
/etc/init.d/odhcpd restart
```

The same helper is used by the guarded L3 bench on entry and restoration. Its
integration passed two automatic entry/rollback cycles on the September 25
RAM bench without manual corrections. The earlier live pass used equivalent
manual transition corrections; the reports retain both sets of evidence.

On the **main router**, set `network.wan6.reqprefix=61`; keep its normal
DHCPv6 protocol/uplink and RA default-route reception. Set its LAN
`ip6assign=64`, with hints 0 through 7 for additional LANs/VLANs. The ONU's
LAN /60 setting is the **server allocation pool**, not the prefix offered
whole to the main router: odhcpd reserves a local /64 and delegates /61 from
the remaining space. No NAT66 or RA/NDP relay is needed. Native odhcpd owns
IPv6 lease lifetimes, prefix updates and delegation routes.

Enable existing software/hardware flow offload in the firewall defaults. Keep
the default LAN-to-WAN forwarding. The package's fw4 includes add an IPv4
source-NAT exemption and WAN-to-br-lan forwarding for only the currently
selected public address. Normal native IPv6 firewall policy continues to
apply; inbound IPv6 services require the appropriate explicit firewall policy
on both routers.

The canonical configuration is `/etc/config/xgspon`, section `passthrough`.
In **Network → XGS-PON → Settings → IPv4 passthrough**, choose **L3 IP
passthrough**, enter the main router's WAN MAC and click **Save & Apply**.
Save commits without activating the change. Router mode is the default.

The equivalent UCI configuration is:

```sh
uci set xgspon.passthrough.mode='l3'
uci set xgspon.passthrough.client_mac='02:11:22:33:44:55' # replace
uci commit xgspon
reload_xgspon_config
```

On the first package installation into a running image, reload firewall4 to
load its includes and enable/start `/etc/init.d/xgspon-passthrough`. Normal
firmware startup selects the saved mode. Subsequent CLI commits require
`reload_xgspon_config`; LuCI Save & Apply calls it. The command validates the
committed file and applies changed mode/MAC settings without a config watcher.

The example MAC is documentation only. Renew the downstream WAN DHCP client
after initially entering or leaving passthrough; a pre-existing private lease
cannot be changed instantly by editing the ONU's DHCP configuration. Once in
passthrough, the selected client receives two-minute IPv4 leases. WAN changes
update the generated options; the client's renewal/retry determines when it
adopts a new address. Other LAN DHCP clients keep ordinary router service.
Ensure only the intended main router requests an IPv6 /61 on the link.

Restore ordinary IPv4 router mode with:

```sh
uci set xgspon.passthrough.mode='router'
uci commit xgspon
reload_xgspon_config
```

IPv6 /61 delegation may remain enabled in router mode. L2 bridging is a
separate topology/configuration choice: stop this L3 owner before using the
existing bridge profile. L2 can request the ISP /60 directly and does not use
the downstream odhcpd server on that link.

## Event and failure behavior

The apply command compares `xgspon.passthrough` independently of the optical
settings. Passthrough-only edits never cycle
optics. Uncommitted, unchanged or invalid mode/MAC settings leave the running
mode alone. Changing the selected MAC stops and cleans up the previous owner
before starting its replacement. A cleanup failure prevents replacement.
Plain CLI `uci commit xgspon` does not emit procd's `config.change` event;
the explicit apply command is required. Standard `reload_config` events also
call this command through the optical service's procd trigger.

Netifd WAN up/down/update events wake the existing reconciler with SIGUSR1.
Lease reconciliation runs on those events and at startup. There is no periodic
lease audit or nftables refresh. The public IPv4 is also written to a generated
RAM-backed nftables include; firewall4 reads that file when it rebuilds its
set on reload. Both nftables sets use persistent elements until a WAN event or
normal teardown changes them.
The reconciler selects the default route explicitly, ignores IPv6 DNS entries
in the IPv4 DHCP option, validates the WAN tuple, and rewrites only its own
RAM fragment. No UCI/flash commits occur on renewal. It restarts dnsmasq only
when its fragment changes, because SIGHUP does not reread dnsmasq configuration.

On WAN loss or invalid WAN state, it withdraws the public route/firewall state
and stops offering a lease to the selected MAC. It avoids issuing a long
private lease during the outage. Address changes retire only the old address's
routes, neighbor and conntrack entries. Gateway/DNS-only updates preserve the
address. A recreated PON device has its required rp_filter setting restored.

Normal stop and caught errors restore local routing priority, sysctls and
ordinary dnsmasq service. IPv6 state is never stopped, copied or restored by
this owner. On an unexpected owner exit, procd restarts it after a five-second
delay. At startup the direct owner requests recovery of
any stale L3 state through the same reconciler script, even if the saved mode
is now router. If L3 remains selected, the new owner then checks the current
WAN lease and applies it. Recovery uses its RAM journal to clear stale firewall
elements, routes, rules and its DHCP fragment. If it cannot safely remove an
externally changed fragment, startup stops and retains the journal for review.
If an older watcher left an orphaned owner during upgrade, service startup
stops that owner and waits for its lock before selecting the saved mode.
An abrupt death leaves the old live state in place until the restart succeeds;
the generated include is removed during startup recovery. Reboot clears RAM
state; normal startup is then governed by the saved mode. Do not remove the
ownership directory alone while its rules/routes remain installed.

An externally changed DHCP fragment is retained and reported, rather than
overwritten during cleanup. Fixed management IPv4 addresses and routes must
remain independently reachable during test transitions.

## Validation boundary

`tests/q1000k/test_passthrough_integration.py` uses real Linux policy routes,
nftables and dnsmasq syntax validation inside a disposable user/network
namespace. UCI/netifd status and service restarts are fixtures. It covers
acquisition, DNS/gateway changes, renumbering, loss/reacquisition, fw4 set
recovery, malformed status, cleanup and foreign-fragment preservation. It
optionally runs real dnsmasq, odhcpd and odhcp6c exchanges through a veth
client using `passthrough_wire.py`. The upstream prefix and netifd kernel state
are fixtures. This passed simultaneous public IPv4 and /61 acquisition,
renewal, first/last /64 reachability, and release-route removal. It is not a
hardware offload test. `test_native_pd_allocator.py` executes the pinned odhcpd allocator's
actual code to verify /61 fits a /60 pool and equal-length allocation fails.

The September 25 image qualified the installed IPv4 service with simultaneous
public IPv4/native /61, first/last /64 traffic, DHCPv6 renewal/release, WAN
down/up recovery and hardware/software/hardware controls, after manual IPv6
transition correction. See the workspace learning
`learnings/q1000k-passthrough-bench-20260925.md` for exact image and captures.
The automated transition helper added afterward is covered by real Linux
namespace regression tests for mask changes, stale routes, rollback and refusal
of foreign state. A subsequent two-file RAM overlay on the same boot passed
two automatic entry/rollback cycles without manual corrections, native /61,
and IPv4/IPv6 hardware/software/hardware controls. The original image bytes
remain unchanged. The physical main router, prefix renumber/rebind/expiry,
PON recovery, cold boot and maximum throughput remain separate qualification
work.
