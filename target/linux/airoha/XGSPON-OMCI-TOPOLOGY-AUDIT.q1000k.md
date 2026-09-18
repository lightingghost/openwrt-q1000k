# Q1000K OMCI topology and Dot1X bench

## Problem established by the c900f5a520 collection

Connected-fiber comparisons reached authenticated O5 and continued provisioning
only when both empty GEM and T-CONT additions preserved the live transport.
The three combined cases each returned the same 13 OMCI errors:

- class 290 / entity 0x0101 Set, mask 0xc000: parameter error;
- four class 277 Sets to 0xdead: unknown instance;
- four bidirectional class 268 Creates (0x03ff..0x0402) with upstream queue
  0xdead: provider -EINVAL;
- four later Sets to those absent GEM objects: unknown instance.

The saved local MIB contained 248 queues, 0x8000..0x80f7, and no 0xdead.
This does not establish why the OLT chose that value. No synthetic 0xdead
queue or alias is introduced.

## Source comparison

The sources are the previously downloaded, pinned primary sources. A fresh
web request failed and the shell download failed DNS resolution; this audit
makes no claim about a newer upstream revision. Reproducible binary extraction,
function excerpts, decoded attribute tables and SHA-256 manifest are in:

`build-artifacts/q1000k-xgspon/omci-topology-research-20260917/` under the
workspace root. The factory binaries came from the user's NAND extraction.

| Source | Verified behavior | Application here |
|---|---|---|
| Factory `omci`, `omciAttriDescriptListPriorityQueue` at 0x455200 | Related port is attribute 6, scheduler pointer 7 and weight 8. Pointer is read/write. | Advertise all 12 implemented queue attributes (mask 0xfff0); permit pointer Sets with provider validation. Unsupported buffer/drop controls remain read-only. |
| Factory `initPriorityQueueStruct`, 0x411e50..0x411f34 | Upstream related port combines T-CONT and reversed priority (7 through 0). Scheduler pointer is initially zero. | Retain the factory priority order; support direct T-CONT binding under strict priority. |
| Factory `omciMIBUploadCalculateCommomMe`, 0x419ca0..0x419f50 | Selects readable attributes, checks the 26-byte budget at 0x419d58 and starts another record at 0x419e00 rather than discarding the remaining attributes. | Count attribute-aligned fragments and return all fragments for descriptor-backed MEs. |
| Factory adapter Dot1X table at 0xd1f88 | Enable is one byte R/W; action register is one byte write-only. | Model the two control attributes separately; do not upload or expose action as a readable state. |
| Factory `setDot1XPortExtPkgDot1XEnable` at 0x50b10; `setDot1XPortExtPkgActionRegister` at 0x50bc0 | Accepts enable 0..1 and action 1..3; the success path logs and returns without calling an authentication engine. | Explicit factory-control-store bench mode. No PAE state, completed EAP exchange, or authenticated UNI is fabricated. |
| Sirherobrine kernel, pinned 2e2cf91fe84467d77649efebd99a28284f2124b3 | `omci_agent_upload_next_locked` selects one object per sequence and encodes at most 26 bytes. The Dot1X class is named, with no dedicated two-control descriptor in that snapshot. | The imported baseline algorithm does not solve pagination or provide a verified Dot1X implementation to copy. |
| 8311 builder, pinned 7d89440c7d9e1f209140910bb039f5d5a24dfbed | `prx300_1U.ini` describes related ports and scheduler pointers. `prx300_1V_32tcont.ini` uses reversed queue priority and null queue scheduler pointers, with per-T-CONT SP schedulers. | Confirms explicit queue relationships and direct binding are normal MIB representations; its hardware-specific queue sizes are not copied. |

Primary source links:

- [Sirherobrine OMCI agent](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon/omci/agent.c)
- [Sirherobrine ME schema](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon/omci/me.c)
- [8311 32-T-CONT MIB](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/mibs/prx300_1V_32tcont.ini)
- [8311 1U MIB](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/mibs/prx300_1U.ini)

The public 8311 builder supplies configuration and patches, not the full vendor
OMCI daemon implementation. Its MIB files establish the advertised topology;
they do not prove how its closed daemon implements Dot1X actions.

## Concrete defects and changes

1. **Missing wire topology.** Our previous Priority Queue upload mask was
   `BIT(8)` (weight only), and Traffic Scheduler mask was `BIT(13)` (policy
   only). The local MIB's related-port and scheduler bytes never reached the
   OLT in those Upload Next replies. Masks are now 0xfff0 and 0xf000.
   The inference to test is that the OLT can now resolve real upstream queues
   instead of using 0xdead.
2. **Missing continuation records.** A descriptor-backed object larger than
   26 attribute bytes previously lost its later attributes, while the upload
   count remained the object count. A shared fragment planner now governs
   counting and sequence lookup, preserving whole attributes. Existing opaque
   vendor representations are unchanged; this is not a full OMCI conformance
   claim.
3. **Queue writes rejected before hardware validation.** Scheduler pointer is
   now writable. The provider accepts its fixed same-T-CONT scheduler and the
   null direct SP binding. A zero weight is valid while SP ignores weights.
   Cross-T-CONT bindings, mixed direct/WRR scheduling, zero WRR weights and
   weights above the hardware limit remain rejected with rollback.
4. **Unnecessary reconfiguration.** Changing SP weights or its equivalent
   direct binding only updates software intent. It does not drain/rebuild the
   transport when hardware is unchanged. Real scheduler changes still use the
   existing serialized hardware update and rollback path.
5. **Dot1X controls.** Mask 0xc000 now decodes both bytes. The default strict
   mode rejects unavailable authentication/actions explicitly. The read-only
   module option `omci.bench_dot1x_oem=1` selects the factory-style validated
   control store for comparison. The mode does not implement an authenticator.

## New observations

Critical trace event 32 records upload command count and exact public wire
attributes for classes 262, 277 and 278, plus class 277 Set requests. Header
records identify transaction, sequence, class, entity and mask; three following
records hold up to 26 bytes. Missing fragments remain incomplete in the host
report. There is no arbitrary-ME payload logger or identity payload capture.
Dot1X diagnostics now preserve both enable and action values, plus selected
mode, even when a Set fails before applying it.

The collector writes decoded `omci_experiment.topology` and the existing
`provisioning`, native TX, live-addition, authentication and critical-gap data.
The new wire evidence avoids relying on the old globally redacted local
`data_hex` fields, whose long zero strings could be obscured by credential
redaction.

## One image, default `topology` suite

Fiber stays connected throughout. Every activation uses live-add mask 3,
inline key processing, initial key readback, the existing exact-length/MIC
receive guards and bounded cleanup. Private identity/calibration stay external.

| Collector case | Variable | Hypothesis / distinguishing observation |
|---|---|---|
| `rx-startup` | TX inhibited, 30 samples | Fresh RX power/LOS/synchronization before activation. |
| `activation-omci-topology-strict` | Corrected topology; strict Dot1X; 300 samples | Does the OLT select real queues and create upstream GEMs even with an explicit unsupported Dot1X response? |
| `activation-omci-topology-oem` | Factory Dot1X control store; 300 samples | Does accepting the two validated controls remove the remaining provisioning failure? Record the actual action byte. |
| `activation-omci-topology-eqd` | Same fixes plus OEM direct EqD mode; 300 samples | Does ranging application affect the now-continuous provisioning path? |
| `activation-omci-topology-repeat` | Repeat factory-control case; 600 samples | Reproducibility, late OMCI Tests, response continuity and any downstream service/traffic. |

Q1 observes wire queue topology; Q2 checks queue Set acceptance; Q3 checks
Dot1X controls; Q4 tracks upstream GEM creation; Q5 distinguishes service and
traffic from O5. Existing O1..O8 diagnostics continue in every activation.
Unreached reconfiguration windows or deferred replies remain untested, not
passed. DMA completion alone is not an optical acknowledgement.

## Validation

The real OMCI module passes the expanded disposable UML fixture with lockdep,
RCU checking and atomic-sleep checking: complete descriptor-backed uploads,
repeat/out-of-range sequence handling, authenticated count/chunk replies,
public-payload whitelist, strict/factory Dot1X writes, invalid-value rollback,
queue masks 0x0300/0x0200 and unknown 0xdead. Existing concurrency, session,
rekey and transactional rollback tests remain included.

The production service-provider host fixture tests null/same-bank pointers,
zero SP weight without hardware rebuild, invalid WRR transition rejection,
cross-T-CONT/0xdead rejection and unchanged hardware on failure. Host collector
tests cover incomplete wire records, request decoding and fixed case selection.
The release checkpoint records the complete build, host/status/JavaScript
checks, image inspection, runtime hashes and protected-branch/config checks.

Hardware success is pending the next collection. The leading explanation for
0xdead is now backed by a specific wire-encoding defect, but its disappearance
and working WAN service must be observed on the OLT connection.
