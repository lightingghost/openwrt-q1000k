# Q1000K: working service and IPv6 source selection

## Hardware evidence used for this change

Image `4600ee38c2c63b9aa20b7ac3f319b8b88e3fe0b7`, boot
`e97e6f19-5cf8-4abc-927e-11851d594d0c`. Raw captures are in workspace
`build-artifacts/q1000k-xgspon/collection-discovery-feedback-4600ee38c2`.
This boot started with RX -18.66 dBm, synchronized and LOS clear.

The combined-policy discovery probe reached ONU-ID assignment, ranging, O5,
398 authenticated OMCI requests, 56 installed service rules, IPv4 DHCP,
DHCPv6 address and delegated prefix. All four downstream-only GEM key-ring
updates were followed by another authenticated OLT request. IPv4 ping,
1500-byte packets and HTTPS passed. This is working upstream/downstream
traffic, rather than a conclusion drawn from an O5 display or a local TX IRQ.

The probe's 39 live passive samples observed BEN 0/1, MCU status 1/3,
hardware TSSI 21,284..34,380, mailbox TSSI 21,314..34,369, published TX power
100..4,693,300 nW and bias 0..17,240 uA. The upper power value is about
+6.71 dBm. These are internal reports, not a calibrated connector measurement.
Raw monitor ADC remained zero with its measurement mux unselected, so decoded
monitor current remains unavailable.

The completed strict control produced 2,758 authenticated requests and seven
key-ring boundaries with no continuation. The VLAN-only repair produced
3,546 authenticated requests and nine such boundaries with no continuation.
The latter eliminates the VLAN rejection but still loses OMCI at full
retirement of the data namespace. Both fixes must remain enabled.
The first combined case and these controls retained complete critical history.
The later combined and broader factory-policy cases each completed with 398
authenticated requests, four continued key-ring updates, zero deactivations,
working IPv4 traffic and successful cleanup. Broader factory normalization was
not needed for this OLT's observed provisioning.

Collection was interrupted during the direct-EqD comparison; the final long
repeat did not run. On 2026-09-18 the host collector was no longer running and
SSH to the bench timed out. Completed-case cleanup is verified, but final
cleanup of the interrupted case and private staging is unknown. The original
running-state checkpoint is preserved alongside `interruption-status.json`;
it must not be mistaken for a completed full suite.

### IPv6 failure isolated without another fiber outage

The original probes bound the PON device but allowed Linux to select its
source. The leased IA_NA WAN address was selected. That source failed ping,
1500-byte packets and HTTPS even though DHCPv6 and next-hop discovery worked.
The bench deliberately disables LAN prefix assignment, so no address from the
delegated prefix was available to these probes.

A bounded control used the same active PON session, destination, VLAN policy
and routes. It added one /128 from the unused live delegated prefix to `pon`,
bound **both** source address and interface, and then removed the address:

| Source | Ping | 1500-byte packet | HTTPS |
|---|---|---|---|
| Explicit leased WAN IA_NA | failed | failed | timeout |
| Explicit delegated-prefix address | passed | passed | HTTP 200 |

Removal succeeded. This establishes IPv6 Internet traffic on this hardware.
An independent control in the factory-policy case requested DHCP/DHCPv6
renewal; subsequent IPv4 ping/HTTPS and delegated-prefix IPv6 ping/MTU/HTTPS
passed, and that temporary address was removed too. The renewal call alone
does not prove a completed server renewal exchange.
It does not establish the provider's precise reason for not routing the IA_NA
source, or imply that every provider treats IA_NA this way. Changing PON laser
settings, identity or encryption is not justified by this source-dependent
failure. The new image corrects the bench's coverage and attribution.

## Focused primary-source comparison

Network retrieval initially failed during this session. The following uses the pinned
primary sources already downloaded, with extracts/hashes preserved under
`build-artifacts/q1000k-xgspon/wan-source-research-20260917` and the preceding
`omci-classifier-research-20260917` source manifest.

- **Factory NAND:** `libapi_omci_adpt.so.1.0.0` normalizes the captured untagged
  VLAN row before handing it to `ponvlan`. `xpon_10g.ko`
  `gwan_config_gemport_encrypt` updates the individual GEM while keeping RX
  and TX encryption distinct. Those narrow operations match the successful
  combined control. The detailed offsets and binary hashes are in
  `XGSPON-OMCI-VLAN-AUDIT.q1000k.md`.
- **Factory network userspace:** extracted `/lib/netifd/dhcpv6.script`
  publishes `PREFIXES` separately from `ADDRESSES` and installs source-specific
  routes for both. `/lib/netifd/proto/dhcpv6.sh` requests address and prefix
  independently. A prefix lease is not automatically a local WAN source.
  This is network configuration above the PON driver.
- **Sirherobrine, `2e2cf91fe84467d77649efebd99a28284f2124b3`:** its
  `gpon_cb_set_gem_encryption` changes an individual GEM. The reviewed service
  provider rejects extended VLAN treatment, so it cannot replace the factory
  normalization for this XGS-PON case. Its GPON encryption treatment is not
  copied over the NAND-derived RX/TX distinction.
- **8311 builder, `7d89440c7d9e1f209140910bb039f5d5a24dfbed`, and bypass
  scripts `69f3c0e4b88505b168c89796386d12bfd705a30d`:** the bypass applies
  detected Internet VLAN push/pop in the data path. Its support script gathers
  OMCI, VLAN and interface state. These support independent service checks;
  the builder does not disclose the proprietary lower OMCI transaction code
  or supply a reason to modify EN7573 optical settings for this IPv6 result.
- The built iputils source retains both the device and explicit IPv6 source
  across two `-I` options. Built curl 8.21.0 supports `ifhost!pon!ADDRESS`.
  New probes use both constraints, so management connectivity cannot pass them.

## Next bench: one image and one collector

Default suite `wan` keeps narrow untagged-DEI normalization, live operation
mask 31, OEM Dot1X acceptance, inline keys, exact OMCI admission/MIC, and
ranging mode 1. Existing comparison and disconnected suites remain selectable.

| Case / collection part | Hypothesis or coverage | Evidence |
|---|---|---|
| `rx-startup` | Downstream reception remains healthy | Power, LOS, sync, frames, passive TX-off observations |
| `activation-omci-wan-source` | IPv6 source selection explains the previous failure | Original probes plus explicit IA_NA and delegated-prefix ping/MTU/HTTPS, routes, neighbors and IPv6 counters |
| `activation-omci-wan-lan` | IPv6 works through the routed LAN path | Isolated client, real collection computer, RA, DHCPv6, DNS, forwarding, firewall, PMTU and local /64 renumbering |
| `activation-omci-wan-renew` | DHCP/DHCPv6 renewal preserves data service and OMCI | Renewal requests, lease snapshots, second source comparison after 60 seconds |
| `activation-omci-wan-repeat` | Initialization and service results repeat after a fresh software start | Same settings, 600-second WAN window, independent registration/provisioning and traffic |
| All active cases | VLAN and RX-only key fixes preserve the management session | Exact OMCI responses, GEM readback, native epochs, subsequent OLT requests |
| Optional `activation-omci-wan-soak` | Natural renewal and sustained IPv6 traffic | 70-minute WAN observation with repeated source probes, lease and packet history; no forced renewal in this case |
| Every temporary source | Test state cannot leak into the next case | Lifetime bound, DAD observation, owned-address deletion before WAN/PON shutdown |

The new small `q1000k-pd-source` program only validates IPv6 addresses. It
accepts an aligned global /48 through /64 delegation, rejects malformed input
and any delegation already used by another local address, and emits one
candidate. The shell requires one live prefix with at least 300 seconds
remaining, adds the address with a 300-second lifetime, checks DAD, and owns
cleanup. No persistent network configuration is changed.

`wan-summary.md` and `collection.json` separate IPv4, default IPv6 and
delegated-prefix IPv6. A successful renewal method means that a renewal was
requested, not proof of a DHCP server reply. Lease and subsequent traffic
observations remain separate. A failed or incomplete cleanup cannot establish
successful service. The source helper is included in the runtime hash manifest.

Host tests cover all supported prefix lengths against independent address
math, prefix collisions, malformed/unaligned input, missing prefix, DAD and
deletion failures, device/source binding, two renewal cycles, cleanup order,
and per-family attribution across multiple completed cases.

## Still unestablished

Long-duration reliability, line-rate throughput, routed LAN/client behavior,
hardware attribution of encrypted GEM traffic and optical waveform quality
remain separate tests. Current internal power-floor samples are not evidence
of no light in a session with successful bidirectional Internet traffic.

## IPv6 follow-up: address failure and LAN coverage

The user requested an attempted repair of Internet traffic sourced from the
leased WAN address and tests for previously untested IPv6 behavior. The device
is currently unreachable, so the following are implemented experiments, not
new hardware passes. For the next real bench the collection computer will be
connected by Ethernet to the Q1000K LAN; Wi-Fi will be disconnected.

### Research and candidate repair

- [RFC 6724, source selection](https://datatracker.ietf.org/doc/html/rfc6724#section-5)
  gives a preferred address precedence over a deprecated one. A reversible
  `preferred_lft=0` experiment can therefore distinguish bad automatic source
  selection from actual inability to forward IPv6. The address remains assigned
  and can still be tested explicitly. Its valid lifetime is preserved; rollback
  reduces the old preferred lifetime by elapsed time and respects a newer lease.
- [A first-hand AT&T router report](https://bbs.archlinux.org/viewtopic.php?id=261789)
  describes the same `2001:506` WAN-address symptom with working delegated LAN
  clients and a successful source-deprecation workaround. This is corroborating
  evidence from one installation, not an authoritative provider routing policy.
- The pinned local OpenWrt `netifd/interface-ip.c`,
  `interface_set_prefix_address()`, assigns a delegated /64 and separately
  handles source routes, valid/preferred lifetimes and deprecation on removal.
  The next image uses `network.add_dynamic` with a delegation class filter for
  real netifd prefix assignment. It does not fabricate a DHCP lease.
- The shipped `odhcp6c` hook separates `ADDRESSES`, `PREFIXES`, `RA_ADDRESSES`
  and DNS data; the factory NAND hook uses the same basic separation. The
  reviewed factory/Sirherobrine PON and 8311 VLAN code provides no reason to
  change optical timing, encryption, VLAN treatment or identity for this
  source-dependent IPv6 failure.
- The local `odhcpd` implementation supplies actual RA and DHCPv6 service to
  the test clients. The namespace client uses Linux SLAAC and the shipped
  `odhcp6c` executable. The collector independently tests the physical client.

The leading explanations remain upstream admission/return routing for the
IA_NA address and local source selection with no PD address installed. Local
filtering/NDP and destination-specific behavior need packet controls. None of
these explains away the two successful PD-source Internet comparisons.

### Experiments in this one image

| ID / part | Hypothesis or previously missing test | Deciding observations |
|---|---|---|
| W1/W2, explicit sources | IA_NA versus PD Internet reachability | Same-interface/source-bound ping, 1500-byte packets and HTTPS; original failed control retained |
| W5, `source` | Automatic selection can be repaired locally | Automatic probes before, during and after reversible IA_NA deprecation; actual HTTP local address and routing evidence |
| W5, second endpoint | One destination is blocking the lease address | Independent Google DNS IPv6 ping/HTTPS alongside Cloudflare controls |
| W5, PON packet capture | Local firewall/NDP versus missing upstream reply | Timestamped decoded packets before the local input filter, IPv6 errors, neighbors, interface counters and nftables rules/counters |
| W6, `lan` | Prefix assignment and advertisements | Real netifd-assigned /64, odhcpd RA capture, Linux client SLAAC address/default route and DAD state |
| W6, DHCPv6/DNS | Clients obtain DHCPv6 addresses and DNS service | Real odhcp6c events, advertised and direct resolver controls, namespace HTTPS with DNS |
| W6, forwarding | LAN clients can use the live PON | Client ping, 1500-byte packets and HTTPS through the actual LAN/WAN firewall chains |
| W6, firewall | Return traffic works; unsolicited inbound UDP is rejected | Outbound echo/return, live listener control and a fresh reverse flow from a separate simulated WAN namespace |
| W6, PMTU | IPv6 Packet Too Big handling works | Controlled 1280-byte hop, rejected 1500-byte packet, successful 1280-byte packet and client route cache/capture |
| W3/W7, renewal | A lease renewal actually completes and service survives | Matching DHCPv6 Renew/Reply transaction IDs on PON, lease snapshots, same-client traffic afterward |
| W6, renumber | LAN prefix change reaches clients correctly | Change the assigned LAN /64 within the same real delegation; capture old/new prefixes, RA, addresses/routes and subsequent traffic |
| W8, `physical` | A real copper LAN client can use IPv6 | This computer's Ethernet address, source route, ping, MTU1500, HTTPS and DNS over IPv6 to the router |
| Optional soak | Natural renewal and sustained operation | 70 minutes, periodic source probes and prefix/lease history; records an actual ISP prefix change if one occurs |

The virtual devices join the existing firewall's LAN/WAN chains. The virtual
WAN uses documentation-only addresses and cannot send test traffic to the OLT.
No optical/fiber action is required. The virtual LAN and physical LAN phases
run sequentially. The physical phase uses a separate netifd alias on `br-lan`
for its IPv6 /64; the original IPv4 management address and bridge ports stay
configured. RA/DHCPv6 are enabled only for that finite experiment, then the
advertisement and owned alias are removed. The collector changes no host
network configuration and rejects an indirect/Wi-Fi management route.

The original bench disables LAN assignment and advertisements. This follow-up
provides the missing delegated LAN configuration during the controlled tests,
and tries a source-selection repair for router-originated traffic. A success
using a PD source is never labeled a repair of IA_NA Internet routing.

`ipv6-summary.md`, per-case `ipv6_experiments` and the physical-client JSON keep
each result separate. A no-reply firewall test needs working positive controls;
setup failure is not a successful rejection. A returned `renew` call is not a
server reply. A change of the LAN /64 is not an ISP PD replacement. Longer-term
reliability, an actual ISP prefix change, external inbound reachability and
line-rate IPv6 throughput remain limited to what the next capture observes.
