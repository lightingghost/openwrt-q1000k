# Q1000K VLAN classifier continuity bench

## Observed failure on 45aea00d3d

Runtime-pinned RAM boot `5b16ed91-53d8-4140-81a7-3261dbd69b99` at
192.168.255.1 starts at -18.66 dBm, LOS clear, downstream synchronized.
Evidence is in `build-artifacts/q1000k-xgspon/collection-omci-service-45aea00d3d`
and its adjacent `-aux` directory. Final counts and cleanup are recorded there
and in the release's bench evidence, because collection continues during this
build's preparation.

The weight-150 fix eliminates the previous queue Set error, but the control
still loses the session after installing 24 rules through full retirement.
The first-service live path advances one authenticated request beyond that
boundary: the OLT sends class 171/entity 0x0101/Set mask 0x0400. This is the
received-frame VLAN operation table. Our provider replaces 24 rules with 24
rules through full retirement, locally reports success, and the OLT then
sends Deactivate ONU-ID. No subsequent authenticated OMCI request is observed
before those deactivations. OEM direct EqD reaches the same boundary.

This is strong evidence that transport interruption during service updates
is a remaining defect. It does not reveal the OLT's internal rejection reason
or prove that the OLT received a reply merely because native DMA completed.
Stable activation/provisioning and Internet traffic remain unproven.

## Factory, Sirherobrine and 8311 evidence

Pinned extracts, source hashes and an extraction script are in
`build-artifacts/q1000k-xgspon/omci-service-followup-research-20260917`.
Fresh web access failed and a bounded raw-source fetch timed out; this review
uses cached primary sources, with no claim about newer upstream commits.

| Source | Verified behavior | Scope of conclusion |
|---|---|---|
| NAND `libapi_omci_adpt.so.1.0.0`, `setExtVlanTagOpTblValue` 0x70130 | Parses/replaces an individual table rule, calls `delVlanTagOpRuleByRuleInfo` and `pon_add_vlan_rule` (call target via GOT 0xb8c80), and schedules WAN configuration commit paths. | A dedicated VLAN update, with no direct all-T-CONT/OMCC retirement in this reviewed function. The lower proprietary driver is not assumed identical to our software classifier. |
| NAND `addOmciGemPortMappingRule` 0x4ba10 and `addConvertedGemPortRule` 0x4b280 | Converts a mapping and calls `addGemPortMappingRule` via GOT 0xb8968; keeps mapping state separately. `omciReconfigQueueMappingRule` calls `addQueueMappingRule`. | Supports separating rule programming from namespace replacement. Does not justify bypassing binding, encryption or DMA ownership checks. |
| Sirherobrine kernel 2e2cf91fe84467d77649efebd99a28284f2124b3, `airoha_gpon_omci_hw_replace_service` | Rejects `vlan_treatment_valid`, multicast and downstream-only services; simple services resolve T-CONT and call `airoha_eth_xpon_add_service`. | Its reviewed provider cannot directly supply the class-171 treatment required here. The unavailable lower implementation is not claimed to handle it. |
| 8311 builder 7d89440c7d9e1f209140910bb039f5d5a24dfbed, `8311-extvlan-decode.sh` | Reads class 171, attribute 6, and decodes four 32-bit words into filter/treatment fields. | Cross-check for the new public VLAN diagnostics; not a driver implementation. |
| Same 8311 revision, `8311-vlansd.sh` and `8311-omci-lib.sh` | Monitors configuration hash every five seconds, invokes VLAN-fix scripts under a lock when the OMCI netdevice exists, and queries the vendor daemon via `omci_pipe.sh`. | Public orchestration and MIB access are visible. The vendor daemon's internal reconfiguration and authenticated reply behavior are not exposed by these files. |

Primary references:

- [Sirherobrine provider](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c)
- [8311 VLAN table decoder](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/usr/sbin/8311-extvlan-decode.sh)
- [8311 VLAN monitor](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/usr/sbin/8311-vlansd.sh)

## Implementation and boundaries

`bench_live_add` bit 3 (value 8; default total mask 15) enables a classifier
update without physical retirement. It requires an existing nonempty classifier
and a nonempty replacement, unchanged used channels and queue masks, and
unchanged scheduler configuration. SP control-byte weights do not alter SP
hardware. A saved scheduler snapshot prevents an unapplied QoS change from
being skipped. The namespace owner independently compares the full current,
expected and desired GEM/T-CONT table, including encryption and OMCC identity.

The live callback reads all 31 data queue masks and checks them against the
expected values. It does not write QoS, queues, GEM/T-CONT registers or native
epochs. The service owner publishes an immutable classifier and waits for old
RCU readers before freeing the old classifier. Already admitted packets retain
valid complete old GEM/channel/queue metadata: no existing binding changes.
Management replies retain their authenticated session and native epoch.

Changed resources, queue masks, used channels, scheduler configuration and
empty-service transitions use full retirement. A disabled comparison bit or
changed namespace selects that fallback before mutation. Unexpected actual
queue state or a failed live transaction contains the port, preserving the
old classifier behind closed admission; it does not retry as a full update.
Native pending-DMA, encryption, MIC, authentication and epoch guards remain.

## Diagnostics

- Event 31 kind bit 8 records the actual classifier transaction; register
  boundaries 20/21 surround it. Boundaries 22/23 still identify full retirement.
- Event 33/id 1 carries classifier rejection bits in `d`: 1 empty classifier,
  2 used channels changed, 4 queue masks changed, 8 scheduler changed. Bit 8
  (0x100) marks this diagnostic version; older captures report unavailable.
- Event 33/id 10 reports channel, actual mask, expected mask and read/check
  error for used channels and any failure. All data channels are checked.
- Event 33/id 4 `c` is 0 full, 1 first service, 2 classifier live. The collector
  identifies the next OMCI response and later authenticated requests before
  reset/deactivation. A following response is correlation, not causal proof.
- Class-171 Set bytes join the existing explicit public-attribute allowlist.
  Complete 16-byte table rows decode to filters, tag removal and tag treatment;
  missing trace chunks stay unavailable. Identity classes are excluded.
- OLT continuation checks now exclude empty service removal during teardown,
  so a later registration cycle cannot count as successful service continuation.
- WAN observation gets a wall-clock deadline as well as a sample limit, matching
  activation/provisioning. Slow diagnostics cannot indefinitely extend a case.

## Default `filter` suite in one image

Fiber stays connected; no physical action is required. All active cases use the
already supplied identity, factory Dot1X control storage, exact OMCI runt/MIC
handling, inline key processing and the accepted full-byte SP weights.

| Case | Live mask / ranging / stage limit | Hypothesis and observations |
|---|---|---|
| `rx-startup` | TX inhibited; 30 samples | Optical power, LOS, frame synchronization. |
| `activation-omci-filter-control` | 7 / mode 1 / 180 s | Reproduce successful first install followed by full-retirement VLAN failure. |
| `activation-omci-filter-live` | 15 / mode 1 / 300 s | Does classifier-only publication let the OLT continue past class 171? Verify eligibility, queue state, exact VLAN row, actual path, responses and deactivations. |
| `activation-omci-filter-eqd` | 15 / OEM direct mode 3 / 300 s | Check residual ranging sensitivity after the same classifier fix. |
| `activation-omci-filter-repeat` | 15 / mode 1 / 600 s | Fresh initialization and longer stability; capture provisioning, DHCP, IPv6 and bound traffic probes if earlier stages pass. |

Each stage has its own limit; total case time includes initialization and
cleanup. Earlier suites remain selectable. S4 identifies the actual live
classifier path; S5 checks subsequent authenticated requests. O5 alone is not
reported as a working WAN service.

## Validation and unresolved conditions

Production-source fixtures test live classifier publication, exact namespace
and QoS preservation, unchanged pending packet destination metadata, stale
native masks/read failures, SP weight 150, changed scheduler/queue fallback,
and the disabled-path control. Namespace fixtures check changed/stale table
rejection. Real-kernel UML tests exercise the new VLAN payload allowlist through
authenticated OMCI receive, plus identity payload exclusion. The shell fixture
runs all four new cases and checks parameters and cleanup. Build checkpoint
records full host, status, view and FIT/initramfs checks.

Hardware validation of this new classifier path awaits the next RAM boot.
Queue-changing service updates, GEM encryption changes, later unsupported MEs,
and any subsequent EAP/DHCP/VLAN data-plane errors may still need work. The
collector reports unobserved prerequisites as not reached.
