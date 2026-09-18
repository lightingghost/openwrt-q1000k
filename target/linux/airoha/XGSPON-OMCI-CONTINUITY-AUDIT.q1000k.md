# Q1000K OMCI continuity audit and next bench

2026-09-17. Development branch: `q1000k-xgspon`.

## Hardware evidence from f29a092f03

The main collection and short control are preserved under
`build-artifacts/q1000k-xgspon/collection-omci-provisioning-f29a092f03-20260917*`.
The auxiliary `bench-observations.json`, `mib-observations.json`, interface
counters and independent cleanup log preserve the cross-checks.

- Connected RX initially measured -18.54 dBm, without LOS, with frame sync.
- Fixed ranging: 668 authenticated requests / 668 queued replies. OEM ranging:
  668/668. Long repeat: 334/334. Short control: 334/334.
- Total: **2,004 authenticated requests and 2,004 queued replies, with zero OMCI
  response errors or authentication rejects in the captured exchanges**.
- OLT-G Get/Set succeeded. Six downstream-only GEM Creates succeeded, all class
  268, entity/port 0xfffe, direction 2, upstream pointers zero. The final OMCI
  completion in each exchange was this Create.
- The post-Create MIB had 328 objects. All 31 OMCI T-CONT objects still had
  Alloc-ID 0xffff, and service_rules remained zero. PLOAM Assign_Alloc-ID was
  received; absence of a later OMCI T-CONT assignment is a different fact.
- The first two comparisons each included one OLT deactivation and a second
  activation. The long repeat stayed authenticated O5 for its 600-second
  provisioning window without advancing beyond the same exchange. Continued
  downstream PLOAM traffic rules out complete downstream optical silence.
- The drained interface counters were RX 2,004 / TX 2,001. Intermediate totals
  were 668/667, 1,336/1,335, and 1,670/1,668. This is a real discrepancy to
  investigate, not proof of which reply was lost. In particular, the OEM
  comparison's increment was 668/668 and it still stalled.
- The improved AF_PACKET observer was attached before the short control and
  saw ordinary outgoing IPv6 but no OMCI. Native OMCI bypasses that socket tap;
  zero captured packets there cannot establish zero native transmissions.
- No serial panic, BUG, warning, or lockup markers were found. All planned
  captures and the extra control finished. Cleanup was independently checked:
  PON modules unloaded, ponraw down, lock/private staging/observers removed.
- Dot1X Set and deferred-response submission were not exercised by these OLT
  exchanges. They are not hardware-validated merely because the image has them.

## Primary source comparison

Research uses cached, pinned sources. Live GitHub access failed during this
work; no claim is made about newer upstream revisions. The source manifest and
function disassembly are in `omci-continuity-research-20260917` under the same
artifact root. No factory binary was executed.

### Factory NAND

The extracted `xpon_10g.ko` and its relocation-aware disassembly show:

- `gwan_create_new_tcont` (0x41db4) calls `gponDevEnableTCont` (0x120d8), updates
  channel records, and dispatches the individual channel/QoS operation.
  `gponDevEnableTCont` searches channels 1..31 and calls `gponDevSetTCont`
  (0x11d64). These paths do not perform a full PHY/MAC stop/restart.
- `gwan_create_new_gemport` (0x42150) updates a GEM record and calls
  `gponDevSetGemInfo` (0x13464), which checks the entry, invokes
  `gponDevSetGemInfoNoCheck` (0x127c4), and verifies configuration. Its call
  path does not retire the whole PON pipeline.
- The adapter audit from the preceding image already established that a
  downstream-only GEM bypasses upstream allocation lookup. The successful
  f29 exchanges now provide hardware evidence for that fix.
- OMCI normalization accepts a 44-byte baseline body or removes an existing
  four-byte trailer from a 48-byte input. The upstream MIC function appends
  four bytes. Our authenticated backend also computes/appends a four-byte
  MIC. Interface byte counters are not a measurement of skb length; the next
  native diagnostic records the actual DMA frame length instead of inferring
  it from aggregate byte counters.

This is evidence for targeted additions, not permission to copy the factory's
less restrictive deletion/reuse behavior into a concurrent native DMA owner.

### Sirherobrine

Pinned kernel revision `2e2cf91fe84467d77649efebd99a28284f2124b3`:
[airoha_xpon.c](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c).

- `airoha_gpon_omci_hw_set_gem_port` calls `gpon_set_gem_port_hw` directly. The
  latter writes one GEM command and verifies valid/encryption readback.
- `gpon_config_tcont_hw` programs one T-CONT, enables the corresponding native
  channel, and rolls it back if channel enable fails. A mutex serializes the
  T-CONT bookkeeping.
- These are GPON provider semantics on different hardware, not an XGS-PON
  register recipe for AN7581. We retain our XGS register definitions and
  command serialization, and copy the limited idea of adding unused entries
  without resetting the existing management path.
- The reference still rejects certain downstream/multicast service replacement
  operations. It is not evidence that our missing multicast forwarding works.

### 8311 WAS-110

Pinned builder revision `7d89440c7d9e1f209140910bb039f5d5a24dfbed`:
[MIB](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/mibs/prx300_1U.ini),
[OMCI wrapper](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/lib/8311-omci-lib.sh).

The published files configure the startup MIB and inspect it through the
vendor OMCI pipe. They confirm the blank OLT-G instance used by the preceding
fix and describe selectable T-CONT/queue topology. They do not expose the
vendor daemon/driver's internal DMA retry or live table transaction code.
Consequently there is no justified 8311 queue implementation to transplant.
Its topology differs from ours; an OLT expectation or MIB/content discrepancy
remains a hypothesis if native transport and continuity are verified.

## Defect and changes

### Native OMCI admission race

`NETDEV_TX_OK` means the native driver consumed an skb, including discard.
The old admission path consumed packets on closure or stale native epoch.
The adapter prepared a packet and then submitted it in a separate call. Its
OMCI pause mutex covers the normal full-pipeline pause, but independent native
queue controls can still close admission between those two calls.

The new native path returns BUSY without changing the skb for a transient
OMCI closure/epoch race. The existing worker rechecks the immutable
**authentication epoch** and original **1,000 ms deadline**, then prepares the
native epoch again. No deadline renewal or cross-session retry is allowed.
Data retains stale-epoch discard semantics. Permanent validation/control
faults still consume the skb and produce an explicit outcome record.

This fixes a demonstrated software loss path. It is not yet proven to be the
cause of the observed three-packet counter discrepancy.

### Append-only table additions

`bench_live_add` is a read-only load parameter: bit 0 permits GEM additions,
bit 1 permits T-CONT additions. The driver default is 3; the collector sets
all control modes explicitly.

A live addition requires an assigned OMCC, no install/QoS/key callback, no
change to any old GEM or T-CONT record, and only new entries. Software table
validation and hardware invalid-to-valid comparison/readback remain required.
T-CONT install rejects an occupied hardware slot even if the Alloc-ID matches.
The native owner verifies closed queues and reclaimed DMA before enabling a
new channel. New queues remain closed until explicit service provisioning.

The protocol executor and table publication guard serialize the addition.
Existing OMCC admission, native epochs, keys, PHY, MAC, FCS state and queued
management replies stay active. Hardware failure contains the port and never
publishes a successful table. Deletion, replacement, rebinding, QoS install,
key changes and reset continue to use full retirement.

## Default single-image test matrix

`--suite continuity` is the collector default. Keep the fiber connected.

| Case | Live-add mask | Ranging | Question |
|---|---:|---|---|
| rx-startup | n/a | n/a | Are power, LOS and frame sync suitable before TX? |
| activation-omci-fixed | 0 | bounded resync | Does the full-rebuild control lose an identified reply or disturb MAC state? |
| activation-omci-live-gem | 1 | bounded resync | Does preserving OMCC during GEM Create allow the next OLT transaction? |
| activation-omci-live-tcont | 2 | bounded resync | Is the PLOAM allocation rebuild the disruptive boundary? |
| activation-omci-live-both | 3 | bounded resync | Do both targeted additions permit service provisioning? |
| activation-omci-live-oem | 3 | NAND direct EqD | Does remaining ranging behavior affect the same provisioning path? |
| activation-omci-live-repeat | 3 | bounded resync | Does the result repeat over a 600-second provisioning window? |

Earlier min48, allocation-revocation, and other controls remain selectable via
`--suite omci`; disconnected optical diagnostics retain their separate setup.
The default suite requires no fiber disconnect/reconnect actions.

## Diagnostics and interpretation

The retained critical stream adds:

- **Event 30, native OMCI TX:** id 1 admission discard, 2 DMA submission,
  3 descriptor completion, 4 descriptor DROP, 5 teardown abort, 6 DMA error,
  7 admission retry. `a` packs TCI/type/device, `b` class/entity, `c` GEM/actual
  skb length, `d` low 32 bits of the native epoch. No payload, identity, MIC,
  key or buffer address is retained. The current authenticated OMCI backend
  submits linear skbs; completion records are tied to their terminal descriptor.
- **Event 31, append:** id 1 begin / 2 end; `a` kind mask, `b` new channel
  bitmap, `c` new GEM count, `d` complete channel bitmap, result original errno.
- **Event 26 snapshots:** stages 20/21 bracket live additions, 22/23 bracket
  full retirement/reactivation. Existing safe MMIO readbacks include burst,
  activation, key-index and related MAC counters; no key material is read.
- Interface counters are sampled every five ticks and around cleanup; these
  sequential reads are not an atomic snapshot. Existing authenticated request,
  response result, MIB, service, PHY and serial captures remain enabled.

`collection.json` includes `omci_experiment.native_tx`, `live_additions`,
`provisioning`, and `interface_counters`. Header/epoch counts identify missing
terminal events. `omci-summary.md` reports submissions, completions and distinct
failure categories. Critical-stream gaps invalidate claims of complete counts.
Native callbacks stop at detach even if descriptors outlive the consumer.

A queued reply is not necessarily submitted; DMA completion is not an OLT
acknowledgement and does not establish optical delivery. If replies submit and
complete while counters/keys remain stable and the OLT still stops at the same
Create, investigate response content, MIC on the wire, OLT profile expectations,
and service/MIB capabilities next. Do not call authenticated O5 working service
without provisioned rules, addressing and PON-bound traffic.
