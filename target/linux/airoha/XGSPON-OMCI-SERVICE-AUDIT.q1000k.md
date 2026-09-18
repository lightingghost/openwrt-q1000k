# Q1000K first-service continuity and queue-weight bench

## Hardware findings motivating this image

The b2b25bc4c5 topology bench, boot ID
`1fde0393-24be-4fcd-99f6-b7ac9cc31faf`, passed runtime file pins. Initial
RX power was -18.66 dBm with LOS clear and downstream synchronization.
Captures are under the workspace's
`build-artifacts/q1000k-xgspon/collection-omci-topology-b2b25bc4c5`.

The corrected MIB upload describes all 248 upstream queues. The OLT now
selects queues 0x8000..0x8003 and creates the four bidirectional GEMs
0x03ff..0x0402. The former 0xdead requests and missing-GEM cascade disappear.
Strict Dot1X rejects enable=1/action=2; the explicit factory-control-store
comparison accepts the same request. It still does not implement an 802.1X
engine or claim completed EAP authentication.

A repeatable new failure is queue 0x8003 Set mask 0x0300, scheduler 0x8000,
weight 150. Our provider returns -ERANGE, translated to processing error.
The neighboring queue 0x8002's weight 50 succeeds. No scheduler-policy Set is
observed in those provisioning sequences; the advertised policy is SP.

The mapper Set (class 130/entity 0x1102/mask 0x7f80) now resolves 24 service
rules. Its installation runs the full transport reconfiguration even though
the T-CONT/GEM records already exist. OLT deactivation follows that boundary.
This is a second candidate cause, not proof that the OLT received or rejected
the last response. Native DMA completion alone cannot establish optical
receipt. The next control keeps full retirement with the weight fix, so it
can distinguish the weight error from disruption during first installation.

## Factory, Sirherobrine and 8311 comparison

Evidence extraction and SHA-256 source manifest:
`build-artifacts/q1000k-xgspon/omci-service-research-20260917/`.
The web request failed and a bounded raw-source download timed out. This
comparison uses the previously downloaded, pinned primary sources and the
user's NAND extraction; it makes no claim about newer upstream revisions.

| Source | Verified behavior | Consequence |
|---|---|---|
| NAND adapter `setWeightValue`, 0x45fd0 | Reads an octet, follows T-CONT/scheduler relationships, and has policy-dependent behavior. Its WRR branch compares against 100, not a universal 127 ceiling. | Do not copy vendor policy conversions or mistake 127 for the OMCI field width. SP retains the control byte. |
| NAND adapter `setPonMacQOSParam`, 0x45eb0 | Builds channel/mode/eight-weight request and calls `gponmgr_lib_set_qos_scheduler` through relocation 0xb8b50. | Scheduler configuration is a channel operation; no full-port restart is present in this function. |
| NAND `qdma_wan.ko`, `qdmaSetTxQosScheduler`, 0x17140 | Loads each weight with `ldrh` at 0x17190, writes channel/queue command at offset 0x1024, polls done bit 30, then changes the channel mode at 0x1040. | The hardware command carries a 16-bit weight. All 1..255 OMCI WRR weights fit without rescaling. This function does not retire all GEMs/T-CONTs. |
| Local native driver | `TWRR_VALUE_MASK` is bits 15:0. It serializes the indirect command, requires target queues closed and no pending target mappings, preserves global units, and verifies weights/mode. | Reuse these checks for first-service QoS while OMCC remains active. No native guard is relaxed. |
| Sirherobrine kernel 2e2cf91fe84467d77649efebd99a28284f2124b3, `airoha_gpon_omci_hw_replace_service` | Resolves the T-CONT, rebuilds a missing entity binding from retained Alloc-ID if necessary, then calls `airoha_eth_xpon_add_service`. | Service intent and resource binding are distinct. The reviewed wrapper does not perform our whole-port retirement. Its unreviewed lower implementation is not assumed equivalent. |
| 8311 builder 7d89440c7d9e1f209140910bb039f5d5a24dfbed | Public MIBs explicitly describe queue relationships, SP schedulers and zero/nonzero stored weights. | Cross-check of MIB representation, not a source for its closed OMCI daemon's scheduler transaction code. No claim that these profiles demonstrate weight 150 on Q1000K hardware. |

Primary pinned references:

- [Sirherobrine provider](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c)
- [8311 32-T-CONT MIB](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/mibs/prx300_1V_32tcont.ini)
- [8311 1U MIB](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/mibs/prx300_1U.ini)

## Changes and boundaries

1. Remove the provider's 127 cap. SP stores the complete 0..255 OMCI octet
   without changing hardware. WRR programs 1..255 exactly through the existing
   16-bit native command/readback; zero WRR weight and mixed direct/WRR banks
   remain unsupported. No silent clamping or ratio conversion is introduced.
2. Add bit 2 (value 4) to `xpon_10g.bench_live_add`. The service owner may use
   it only when no previous data classifier exists and the new rule set is
   nonempty. The namespace owner verifies unchanged complete GEM/T-CONT
   bindings, an assigned OMCC and all 31 data queue masks closed before QoS
   programming. Native QoS checks each target has no pending DMA mapping.
   Channel zero is excluded from this programming.
3. A changed namespace or disabled comparison bit returns -EAGAIN without
   mutation and selects the full transaction. A failure during live install
   contains the port and does not retry a partially completed operation.
   Existing service replacement/removal retains full retirement. The new
   branch does not bypass authentication, MIC or old-session retirement.
4. Trace event 33 records eligibility, actual pre-install scheduler state,
   verified requested state, packed weights and installation outcome. Event
   31 kind bit 4 identifies the live service branch. The host correlates later
   authenticated OMCI requests up to the next reset/deactivation. It reports
   incomplete weight records as unavailable, not zero. Existing public queue,
   native TX, timing, critical-gap, activation and WAN evidence remain.

## One image, connected fiber, default `service` suite

All active cases use factory Dot1X controls, the corrected topology, exact
OMCI runt authentication, inline key handling and the full-byte weight fix.
No physical fiber action is required. Earlier suites remain selectable.

| Case | Live mask / ranging | Hypothesis and observation |
|---|---|---|
| `rx-startup` | TX inhibited; 30 samples | Verify current optical power, LOS and synchronization. |
| `activation-omci-service-control` | 3 / mode 1; 240 limit | Is accepting weight 150 alone sufficient? Retains the earlier full service retirement. |
| `activation-omci-service-initial` | 7 / mode 1; 300 limit | Does preserving OMCC during first service let the OLT continue? Record QoS readback, queue Set success, GEMs, service rules and deactivations. |
| `activation-omci-service-eqd` | 7 / OEM direct mode 3; 300 limit | Is residual failure sensitive to ranging application after the same service fix? |
| `activation-omci-service-repeat` | 7 / mode 1; 600 limit | Independent initialization and longer observation; proceed to DHCP/IPv6/traffic if provisioning remains stable. |

S1 checks full-byte weight acceptance; S2 identifies actual full/live install;
S3 measures subsequent OMCI before a reset. Q/O tests still distinguish MIB
acceptance, local native completion and actual service. Stable O5 without
provisioning/addresses/traffic is not reported as successful WAN access.

## Validation

Production-source host fixtures exercise the OLT-like T-CONT/GEM/service
sequence, initial live vs full paths, unchanged OMCC and global units, ordinary
replacement retirement, stale/changed namespace rejection, failure containment,
SP weight 150/255 storage and exact WRR weight 150. The native QoS fixture
keeps an open, pending OMCC while reading/writing only a closed data bank.
The complete shell launcher test checks all four phase names, module choices,
private-input ownership and cleanup. Collector tests check case selection,
missing diagnostic chunks, failed installation and reset-bounded continuation.
The build checkpoint and release manifest record the complete validation run.

Hardware validation of these new branches is pending the next RAM boot. Later
GEM encryption changes, active service replacement and advanced QoS can still
expose additional reconfiguration paths; their absence in a capture is not a
passing test.
