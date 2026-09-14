# Q1000K PON integration audit

This records the remaining runtime contract after the AN7581 BSP/PHY build
port, not a working PON data path. The current device is restricted to read-only
access; no modules, files, GPIO changes, I2C probes, reboots or firmware flashes
were performed during this continuation. Historical controller tests are in
[the checkpoint](XGSPON-STATUS.q1000k.md).

## Resource ownership

The running Linux 6.18.44 `/proc/iomem` inventory and the current AN7581 DTS
agree on these owners. Reading this inventory does not read device registers.

| Physical range | Current owner | Integration consequence |
| --- | --- | --- |
| `1fb00000-1fb0096f` | SCU clock/reset driver | Use the existing syscon/regmap and reset/clock providers; do not claim an OEM `/scu@1fb00000` platform device. |
| `1fb50000-1fb525ff` | `1fb50000.ethernet`, FE | Shared FE changes must go through the existing Ethernet driver. |
| `1fb54000-1fb55fff` | Ethernet QDMA0 | Existing LAN DMA ownership, queues, IRQs and NAPI remain with Ethernet. |
| `1fb56000-1fb57fff` | Ethernet QDMA1 | Already owned even with `gdm2` disabled; not an independently available vendor WAN DMA block. |
| `1fb58000-1fb5ffff` | DSA switch | Never expose this range through a generic PON FE accessor. |

The new `airoha_ecnt_xpon` resource provider replaces the unbuilt OEM
`ecnt_xpon.c` on AN7581. The disabled `quantum,q1000k-pon-mac` board node
names GPON (`1fb64000`, size `3e8`), XG-PON (`1fb65000`, size `ff8`) and EPON
(`1fb66000`, size `23c`) windows, plus MAC/GASP interrupts (GIC SPI 42/34).
The provider validates those OEM-derived addresses and sizes, maps only the
named resources, and publishes them only after all mappings and IRQ lookups
succeed. Register access accepts aligned 32-bit legacy offsets within each
window. Removal drains accessors before mappings are freed; manual unbind is
suppressed because legacy consumers borrow the device pointer. Full consumer
lifetime and IRQ handling still need integration. Probe performs no clock,
reset or DMA writes, and the disabled node prevents binding in this checkpoint.

The AN7581 SCU bridge now obtains the existing NP/chip syscon regmaps instead
of claiming an OEM SCU platform device or PBus IRQ. Masked updates use
`regmap_update_bits`; full writes use `regmap_write` to preserve strobe/W1C
semantics. Aligned accesses are bounded by the current DT windows. Existing
full-register callers still require an ownership audit, and reset/clock
operations must ultimately use their proper providers. Legacy value-only and
void accessors log invalid/failed accesses and return all-ones/discard writes;
they cannot propagate errno to their callers. Startup rejects absent PHY/MAC
resource providers before accessing hardware, but this is not complete
end-to-end runtime error handling.

Local host tests compile the actual production accessors against MMIO/regmap
fixtures and exercise every aligned/misaligned window offset, missing/removed
providers, failed reads/writes, full-write semantics and masked preservation.
All four BSP modules and the PHY pass kernel modpost; the board DTS compiles.
No new provider or DT has been executed on the device. Absence of a region
from `/proc/iomem` does not establish clocking or initialization.

The AN7581 optical PHY uses digital registers at `1faf0000` and the PON
analog/PMA windows at `1fa8a000`/`1fa8b000`. The earlier generic provider also
listed `1faf3000`/`1faf4000`, but the AN7581 tuning source uses the `1fa8xxxx`
windows. The existing `pon_pcs` node overlaps those analog resources and
shares XPON reset IDs. Its disabled state is deliberate: the optical PHY
provider and PON PCS must not bind concurrently. Copper `eth_pcs` uses the
separate `1fa7a000`/`1fa7b000` windows, which the optical provider rejects.
See [PHY evidence and integration](XGSPON-PHY.q1000k.md).

The old `READ_FE_REG` abstraction is especially misleading:

- AN7581 EPON FEC code accesses legacy `0xbfb57130`, physical `1fb57130`,
  inside QDMA1.
- `prepare_epon()` writes legacy `0xbfb59640`, physical `1fb59640`, inside
  the DSA switch region.

These are not both FE registers. XGS-PON work must retire the unrelated EPON
operation or route an explicitly supported operation through its real owner.
The Q1000K build now omits `prepare_epon()` and rejects EPON mode startup
and the EPON FEC API with `-EOPNOTSUPP`. A generic unchecked `ioremap` wrapper
would hide an ownership bug.

## Packet and queue contract

The current Ethernet driver chooses QDMA0 for LAN and QDMA1 for WAN roles in
`airoha_dev_set_qdma()`. Role changes, hardware QoS and LRO operate on shared
DMA resources. The PON adapter must participate in that lifecycle, including
queue allocation, reset, NAPI/IRQ handling and teardown. It cannot initialize
QDMA1 independently or overwrite a callback table used by other interfaces.

The vendor AN7581 little-endian `PWAN_FETxMsg_T` provides a concrete descriptor
starting point:

| Message word | Bits | Vendor meaning |
| --- | --- | --- |
| 0 | `2:0` | Queue |
| 0 | `7:3` | T-CONT/channel |
| 0 | `8` | Management/OAM flag |
| 0 | `13:9` | Fast/TSO/checksum flags |
| 0 | `29:14` | 16-bit GEM/XGEM port |
| 0 | `30` | OMCI MIC key index |
| 1 | `19:15` | NBOQ |
| 1 | `23:20` | Forwarding port |

The current Ethernet TX path calls the `29:14` field `SP_TAG` and fills it
from the DSA tag. Its ordinary Ethernet queue selection is not an OLT T-CONT
allocation. Reusing `ndo_start_xmit` unchanged would lose PON metadata.
The vendor RX word 0 separately carries channel `7:3`, management flag `8`,
CRC/runt/long errors `11:13`, GEM `29:14` and no-MIC flag `30`.

Required adapter behavior:

1. Encode explicit PON metadata at the final descriptor boundary. Do not rely
   on private `skb->cb` fields surviving bridges, classifiers, GRO or offload.
2. Separate OMCC management frames from Ethernet data before Ethernet parsing;
   preserve GEM ID, MIC validity/key context and exact frame length.
3. Define skb ownership on success, backpressure, DMA-map failure, RX delivery,
   unregister and reset. Do not use a vendor hook's return value as a Linux
   `NETDEV_TX_*` value without matching its ownership convention.
4. Preserve existing LAN/DSA queues and forwarding. Implement per-T-CONT/queue
   mappings and counters through the Ethernet owner. Keep PON offload disabled
   until the nonaccelerated path works.
5. Reject unsupported flow/QoS operations; success-only compatibility hooks
   cannot stand in for GEM, T-CONT or VLAN programming.

The imported sources call QDMA APIs for TX/RX, DMA start/stop, interrupt control,
initialization, QoS/weights, rate meters, thresholds, congestion and channel
closure. They call FE APIs for channel enable/retirement, forwarding, queue
reservation, packet lengths, meters and counters. These use dynamic ECNT hooks,
so most missing providers do **not** appear as unresolved linker symbols.
The [resolved linker inventory](XGSPON-STATUS.q1000k.md#kernel-audit) therefore
does not mean the runtime dependency list is implemented.

The API wrappers now initialize every request with an unsupported error and
zero the other fields. A missing/inactive hook returns the legacy hook error;
an unhandled request returns `-EOPNOTSUPP`. Negative dispatcher/provider
errors propagate. Getter fields copied from request storage are published
only after success (direct output pointers remain the provider's contract).
The MAC rejects absent QDMA WAN/FE providers before initializing hardware.
This is only a readiness snapshot: it neither pins callback registration nor
proves that all required operations exist. Many old callers still ignore API
errors, and shared DMA start/stop must be replaced by a proper consumer
lifecycle before the vendor stack is enabled.

Patch 013 makes ECNT registration/removal explicitly process-context APIs.
One writer mutex covers publication/removal and the RCU grace period; removed
nodes are reset for reuse only after readers finish. Unregister-by-ID no longer
waits from inside its own read section. Never-registered/double removal and
partial registration rollback are supported. Hook descriptors and callback
code must remain valid until unregister returns; readiness and enable queries
still provide no reference or capability guarantee. The raw QDMA callbacks
installed by `QDMA_API_INIT` are outside this registry's lifetime contract.
Patch 016 removes that packet callback path from Q1000K; other vendor targets
still use it. ECNT remains required for the outstanding control/QoS operations.

Patch 012 tracks completed MAC startup stages and preserves failure codes.
Netdev open and hook/RX/event dispatch remain closed until the worker and all
state are initialized; protocol interrupts are enabled after publication.
On teardown it closes ingress, masks protocol interrupts, unregisters hooks,
frees the acquired dying-gasp IRQ, closes WAN interfaces (including NAPI),
detaches the legacy QDMA callbacks and waits for RCU readers. It then removes
user interfaces, shuts down timers, stops the worker and releases WAN,
protocol and crypto state. All cached aliases are cleared before returning.
The idle worker can stop without enqueuing a synthetic quit job, including
when its queue is full. Producers and the consumer use the same queue lock.

Nested WAN initialization now propagates OMCI/OAM/EAPOL creation failures and
unwinds already-created interfaces and GPON work. The dynamic MCI cdev retains
the release function supplied by `cdev_alloc`; failed publication drops its
kobject reference, and only acquired device numbers are unregistered.
Crypto allocation occurs before PHY mode setup or protocol interrupt enable.
Protocol teardown also shuts down the previously omitted hardware and silence
timers, and drains tasklets before releasing their data.

These are software lifetime fixes, **not hardware rollback or a completed
transport attachment**. Legacy QDMA callback replacement has no documented
provider pin or synchronous detach contract. Detach errors are now logged;
they cannot make module unload safe by themselves. Before enabling this
package, replace those hooks with a native Ethernet consumer API that owns and
drains callbacks, preserves LAN DMA state, and prevents provider removal from
leaving references to unloaded code. Reset/clock/analog sequencing and ignored
FE/PHY operation errors remain separate blockers. No vendor module was loaded
to test the lifecycle changes.

## Native Ethernet consumer transport

Kernel patch `9997-net-airoha-q1000k-pon-consumer.patch` implements the native
packet attachment API in `include/linux/soc/airoha/airoha_pon.h`. The disabled
Q1000K GDM2 node now declares `airoha,pon-port`, with a matching binding. Probe
rejects this role on other boards, SoCs, GDM ports or nonzero NBOQ. Neither
GDM2 nor the PON PCS/MAC is enabled. Vendor patch 016 now consumes this API
directly for Q1000K packets, as described below.

The native Ethernet driver keeps ownership of QDMA rings, IRQs, NAPI, DMA
mapping, completion and shared LAN resources. The consumer passes explicit
GEM/channel/queue/OMCI/MIC-index fields into descriptor word 0. CPU ring
selection is separate from the OLT's T-CONT/queue allocation. PON submissions
bypass DSA tagging, disable descriptor accounting/meter selection, and always
ring the DMA doorbell even when an upper interface is batching packets.
Ordinary Ethernet submissions to a designated PON port are rejected. The
port cannot be bridged, has hardware features disabled, rejects TC setup and
PPE output flows, and uses direct QDMA1 CPU delivery when opened.

TX returns Linux ownership semantics: `NETDEV_TX_BUSY` leaves the packet
unchanged with its caller; `NETDEV_TX_OK` consumes it, including failures.
Nonempty frames with a linear head and ordinary page fragments are supported;
GSO, partial checksums, nested fragment lists and oversized frames are rejected.
After the ring-capacity check, `skb_orphan()` releases a consumer/socket
destructor before the native driver retains the packet for DMA completion.
Both ring-space recovery and BQL completions notify the consumer to schedule
another TX attempt. The wake callback must not submit inline under the native
queue lock. Patch 016 uses a deferred retry queue with a timer fallback so a
wakeup racing a busy return cannot strand a packet.

RX delivers complete raw frames before Ethernet header parsing, DSA, PPE,
hashing or GRO. It preserves the first descriptor's four host-endian metadata
words, accumulating later CRC/runt/long error bits before delivery. It rejects
aggregation, overflow, mixed-port fragments and excessive assembled length.
Once a fragment fails, the rest of that chain is discarded through its final
descriptor, including across NAPI polls. Dropped frames count against the poll
budget; processing stops when the available descriptors are exhausted. RX
cleanup frees a retained partial frame. The no-MIC bit is presence information,
not an authentication verdict; OMCI MIC verification remains unimplemented.

Attach/release serialize under RTNL. RX, TX submission and wake callbacks use
RCU; lower stop/unregister disconnects both pointers, waits for readers and
notifies the consumer once. No permanent lower-netdev reference blocks
unregister. The caller owns its handle and callback storage until release,
and must stop other users of that handle first. Releasing an old, stopped
handle cannot detach a later attachment. A generation rejects a software RX
assembly retained across reattachment.

**Release disconnects callbacks; quiesce additionally waits for native TX
descriptor reclamation. Neither is an optical-off operation.** Already-submitted
DMA can complete after release, and a generation does not flush descriptors
received in hardware before a new attachment. Physical TX/RX gating, FE/optical
FIFO draining, actual GDM padding/CRC behavior,
reset/clock coordination, FE flow programming and control/QoS integration remain
necessary before activation. The lower phylink/optical connection is also not
established by this patch. The vendor package stays `BROKEN`, unselected and
without autoload; service start continues to fail explicitly.

Local C/UBSan fixtures execute the actual attachment, TX descriptor, RX assembly
and TX cleanup functions. They cover metadata encoding, unchanged BUSY returns,
every DMA-map failure for zero through three fragments, cleanup and the ordinary
Ethernet TX/RX path. RX cases include damaged later fragments, mixed ports,
allocation failure, aggregation, stale software generations and all-invalid
rings. A separate UML guest executes the actual attachment implementation with
real Linux netdevices, skb queues, RTNL and RCU; only hardware-owner storage and
DMA submission are fixtures. Its 100 attach/release/stop cycles produced
1,538,479 RX/wake callbacks and 51 detach notifications, including unregister
while a handle remains owned, with no RCU/locking/kernel diagnostics. These
checks establish software behavior, not descriptor behavior on the Q1000K.
The complete Linux 6.18.44 target builds with all three API exports, and the
board DT compiles with GDM2 still disabled. All 17 host PON fixtures pass.

## TX descriptor drain checkpoint (2026-09-13)

Patch 9998 adds `airoha_pon_quiesce(handle, timeout_ms)`. It disconnects the
attachment, drains its callbacks, then waits for outstanding mapped TX
descriptors without holding RTNL. A zero timeout polls. Timeout returns
`-ETIMEDOUT` and leaves the attachment disconnected; the caller may retry or
release its handle. No shared DMA enable, interrupt or reset register is
changed by this API. A new attachment is rejected while the previous one's
mapped descriptors remain, including after its caller has released the handle.

Each mapped descriptor retains an internal attachment reference and increments
both attachment and lower-port counters. Native completion, mapping rollback
and queue cleanup release the same ownership after unmapping the buffer.
Completion also clears the retained skb pointer; duplicate notifications for
already-reaped descriptors are ignored. Counting every fragment prevents the
last packet fragment from reporting a drain while earlier mappings remain.
Ordinary Ethernet descriptors do not affect the PON counters. The caller's
release drops its reference after callback detachment, so delayed completion
can free internal storage without calling consumer code or reading its private
data. Native lower-device storage remains owned until queue cleanup finishes.

Host fixtures execute the actual submission, completion and cleanup paths,
including mixed Ethernet/PON traffic, out-of-order completion, duplicates,
every fragment-mapping failure, bounded timeout/retry and release with pending
DMA. The real Linux UML test now uses delayed synthetic descriptor completions:
it passes timeout/retry, 100 attachment/stop cycles and late completion after
unregister/release, with 986,082 RX/wake callbacks and no kernel/RCU/locking
diagnostics. All 17 host PON tests and the full Linux 6.18.44 target build pass;
`airoha_pon_quiesce` is the fourth exported consumer API.

This establishes the software reclamation contract, not physical FIFO emptiness
or packet delivery. The existing native DMA-stop timeout path still warns and
continues cleanup; its hardware recovery behavior needs validation before PON
activation. RX hardware draining, PHY/reset sequencing and FE/optical FIFO
checks remain separate. GDM2, the MAC and PON PCS remain disabled.

## Native queue admission checkpoint (2026-09-13)

Kernel patch `9999a-net-airoha-pon-queue-control.patch` adds native QDMA1
queue close/set/get operations and packet admission epochs. Attachment now
requires exclusive QDMA1 assignment: another assigned netdev blocks attachment
even when down, as does an existing QoS allocation or hardware aggregation on
the PON netdev. New GDM2 registration and copper QoS migration cannot take over
a QDMA1 attachment or its pending native descriptors.

After those checks, attachment closes all 32 channels' eight TX queues through
the native QDMA owner and verifies register readback. An IRQ-safe lock
serializes subsequent queue changes, TX admission and disconnect. Each channel
occupies one byte of the eight queue-close registers; changing one byte
preserves its neighbours. A failed readback blocks all software TX admission
until a new attachment. Getters return the last verified configuration, not
queue occupancy, and leave output arguments untouched on errors. Disconnected
handles reject control operations without accessing MMIO.

The vendor packet adapter captures the native admission epoch once before
enqueueing a frame. Closing any previously open queue invalidates all queued
software frames for that channel, conservatively including other queues.
Native TX checks the epoch under the same lock as closure. It consumes stale
frames even after the channel reopens; a BUSY retry never renews admission.
The adapter exposes channel close/get helpers without taking its callback
lock around the native call. Closed queues reject new submissions with
`-ESHUTDOWN`, leaving the caller's skb unchanged and owned by the caller.

The T-CONT conversion in patch 018 now uses these native queue controls, as
described below. Patch 020 subsequently synchronizes data GEM mappings;
physical retirement remains incomplete. Packets whose vendor metadata was prepared before admission also
need coordination with those transactions. Attachment opens no queues, and
neither provider-presence gate is removed. Queue closure cannot establish the
fate of frames already submitted to DMA/FE/optical FIFOs, or replace physical
RX/TX draining. GDM2 remains disabled and the service launcher remains unavailable.

Vendor patch 017 also fixes four AN7581-only control wrappers missed by patch
008: general TRTCM set/get, channel closure and OAM forwarding selection now
default to `-EOPNOTSUPP` and preserve negative dispatcher errors. No provider
or successful no-op implementation is registered.

Validation: all 21 host PON tests pass, including all 8,192 channel/mask
combinations, invalid arguments, readback failures, stale BUSY retries and
unchanged neighbouring registers. The native implementation passes 100 real
Linux UML attachment cycles with concurrent channel control, TX/RX and delayed
DMA completion (709,762 RX/wake callbacks). The adapter passes 50 cycles with
1,141,128 allocated/destroyed packets, including closure/reopening under BUSY,
unchanged rejected skbs and unregister/timeout cleanup. Reinspection of the
native guest log found a softirq warning despite its zero runner exit status;
patch 9999b below fixes that locking issue and the runner's failure check.
The earlier claim of a clean native log was incorrect. These are synthetic register/DMA tests,
not measurements of physical queue or optical behavior. The complete AN7581
kernel and experimental r14 module package build locally. No device access,
module loading, installation or flashing was performed.

## T-CONT command and setup checkpoint (2026-09-13)

Vendor patch 018 replaces the Q1000K indirect T-CONT table operations with
`q1000k_tcont.c`. One IRQ-safe lock covers each complete scan, command and
readback sequence. Commands explicitly encode the channel, valid bit and
14-bit register Alloc-ID field, leaving reserved bits zero. Lookups ignore
invalid entries with stale IDs; duplicate allocation returns `-EEXIST` without
deleting the existing binding. Ordinary allocation uses slots 1 through 31.
Slot zero belongs to ONU-ID assignment and cannot be written by this helper;
the old DVT setter also rejects Q1000K writes.

Every write receives a separate readback check. A command timeout or mismatched
verification latches a fault; later commands return `-EIO` without touching
MMIO, so a late completion cannot satisfy another command. Uncertain or
invalidated slots are permanently quarantined for that MAC module instance.
There is no software clear/reuse operation. Recovery requires a verified
hardware reset before a subsequent module instance; unloading/reloading alone
does not prove safe hardware state. The field-width check is not a complete
protocol policy for reserved Alloc-IDs.

T-CONT create/remove requests use a separate nonblocking transaction guard.
Concurrent or reentrant requests return `-EBUSY`; native/FE callbacks run
without the table spinlock held. Setup allocates the MAC entry, verifies native
queue closure, enables the FE channel and opens its queues before publishing
the software allocation and incrementing its count. Failures preserve the
original error and software bindings, quarantine the slot, and attempt queue
closure, FE disable and MAC invalidation. Cleanup errors are logged rather
than reported as successful rollback. PLOAM allocation and XMCS callers now
propagate errors and update counts only on successful setup; they no longer
invoke the shared legacy QDMA buffer reset for these operations.

**Removal is deliberately incomplete.** It quarantines the slot and attempts
native queue closure, preserving bindings and counts. Successful closure
returns `-EOPNOTSUPP`, because FE/optical FIFO retirement is absent. Patch 019
adds the native per-channel descriptor drain described below. Bulk removal
attempts every assigned
channel and preserves the first control failure. It does not use the vendor's
global `G_TX_FCS_TBL_INIT` write as a per-channel retirement mechanism. The
`gpon_disable()` and inconsistent ONU-ID reassignment paths stop before clearing
identity or resetting the MAC when bulk retirement is incomplete. Other legacy
reset paths and ONU/OMCC assignment still need their own lifecycle audit.

Patch 020 below subsequently synchronizes data GEM mutation and packet
metadata preparation with these transactions. This checkpoint tested setup with
a synthetic FE provider; patch 021 below replaces TX-channel enable/disable with
the native owner's verified API. These changes therefore
retain both readiness gates, disabled board nodes, the `BROKEN` package marker
and explicit service-start failure.

Validation: all 24 host PON tests pass against the prepared r15 package tree.
New fixtures cover every one of 33 scan/write/readback timeout points, reserved
bits, stale/duplicate IDs, all slots, rollback failures, caller errors, reset
guards and concurrent allocation. An isolated Linux 6.18.44 UML guest executes
the production helper with real spinlocks, IRQ handling and 64 racing callers;
it passes command, verification and quarantine checks without kernel/locking
diagnostics. Register behavior is emulated, not measured on hardware. All six
vendor modules pass modpost and the experimental r15 APK builds. No SSH,
device writes, module installation or flashing was performed.

## Per-channel TX drain checkpoint (2026-09-13)

Kernel patch 9999b adds `airoha_pon_quiesce_channel()`. It permanently closes
one channel for the current attachment and polls its native TX mappings without
sleeping. `-EAGAIN` means mappings remain; zero means that channel's mappings
have been reclaimed. Other channels and callbacks remain active. Queue opening
after this operation returns `-ESHUTDOWN`, even once the native count reaches
zero. Disconnect and failed queue readback return errors rather than a false
drain result. The caller must retain its handle throughout the operation.

Every mapped TX descriptor now retains its encoded T-CONT channel. Completion,
mapping rollback and cleanup decrement that channel's count only after native
DMA unmapping. Release/acquire ordering connects unmapping to the drain poll.
An out-of-order completion of the final packet fragment cannot hide outstanding
earlier fragments, and activity on another channel does not delay a completed
channel's poll. Attachment references still survive until all descriptors have
been reclaimed, including after consumer release.

The expanded Linux test exposed a softirq warning in the existing admission
path: it held an IRQ-saving lock around nested `_bh` unlocks. Native TX queue
locks now preserve IRQ state across submission, completion and cleanup. The
exclusive PON admission lock serializes direct submissions without taking the
ordinary netdev TX lock; ordinary Ethernet keeps its existing netdev lock and
cannot submit data to the PON port. Stop/disconnect continues to serialize
against admission before draining callbacks.

Vendor patch 019 uses the new operation for T-CONT removal and setup rollback.
The adapter pins its current transport with RCU and preserves native error
codes. An incomplete native drain remains an error. A completed native drain
still returns `-EOPNOTSUPP` from T-CONT removal, preserving bindings/counts,
because FE/optical FIFO retirement is not implemented. Reattachment or module
reload must not be used to bypass that missing hardware retirement.

All 24 host PON tests pass, including all 32 channels, every fragment mapping
failure, mixed-channel completion, duplicate completion, pending/retry behavior
and blocked reopening. The native UML test passes 100 attachment cycles with
concurrent TX/control, delayed completions and a real interrupt-context channel
operation (415,138 RX/wake pairs). The adapter UML test passes 50 cycles and
841,950 allocated/destroyed packets. Final guest logs contain no kernel/RCU/
locking diagnostics. PON UML runners now explicitly fail on diagnostics;
their previous negated `grep` was exempt from shell `errexit`. Historical
runner exit status alone is therefore insufficient evidence of a clean log.
These tests use synthetic registers/DMA and do not validate physical drain.

The final Linux 6.18.44 AN7581 kernel and r16 vendor package build successfully;
all six modules pass modpost against the eight native API exports. The final
prepared trees match the tested source files. Board nodes and service gates
remain disabled; no device access,
installation or flashing is part of this continuation.

## Vendor packet adapter findings

The native API cannot be connected to the original vendor callbacks unchanged.
Patches 014 and 016 address the following original findings on Q1000K:

- The original `pwan_cb_rx_packet()` expects DMA-written data in an skb whose logical length
  is still zero; it later calls `skb_put(pktLen)`. Native RX already sets the
  length and may assemble page fragments. The adapter must normalize that
  contract once, linearize before vendor direct-pointer parsers where needed,
  and avoid double growth or copying past a linear buffer.
- The original callback reads Ethernet bytes 12/13 before checking packet length and
  can reinterpret those bytes as an EAPOL marker even in an OMCI frame. Raw
  management frames must be selected from validated descriptor metadata and
  handled separately from Ethernet parsing. Merely setting protocol zero
  after `eth_type_trans()` does not restore the header bytes it removed.
- The original TX caller frees the skb again for nonzero QDMA results even though its
  comment says the QDMA call consumed it. Native `NETDEV_TX_BUSY` instead
  retains an unchanged skb; native `NETDEV_TX_OK` consumes it on success or
  failure. The adapter must define one ownership contract and deferred retry,
  and the vendor caller must use it consistently.
- The legacy receive-event path schedules its own NAPI and changes QDMA RX
  interrupts. Native NAPI already owns receive delivery; those event/poll hooks
  cannot control the same ring a second time.

These findings are from the prepared imported source, not from device tests.
The native packet adapter below replaces those interfaces. It does not provide
the remaining FE/QoS control operations or establish physical PON service.

## GEM command and binding checkpoint (2026-09-13)

Vendor patch 020 adds serialized AN7581 GEM commands and one software binding
lock shared by data GEM and T-CONT publication. `q1000k_gem.c` uses the imported
AN7581 register layout: command `0x5274`, status `0x5278`, a 16-bit ID, and
separate valid/unicast/encryption bits. In particular, the hardware type bit
is **unicast**, the inverse of the vendor multicast enumeration. Reserved
command bits remain zero. Reads, compare-and-replace and separate write
verification run under one IRQ-safe command lock. A timeout or readback
mismatch latches a fault for the module instance, with no software clear.
Invalid entries' stale type/encryption bits do not imply an existing binding.
A valid entry that differs from the expected value is never overwritten.

The old setter incorrectly treated matching type/encryption as success even
when asked to invalidate a valid GEM. The new helper includes validity in its
comparison. The legacy raw setters/reset, debug write handler, destructive
register tests and reset test are rejected on Q1000K so they cannot bypass
binding ownership. The getter delegates to the serialized helper and preserves
output values on failure. This does not complete the other PHY/OMCC/reset
lifecycle paths or make the vendor module safe to load.

A nonblocking control guard now covers both GEM and T-CONT transactions. GEM
creation validates the ID, type, channel and allocation; reserves no public
slot until the hardware write/readback succeeds; then publishes the entire
record, ID-to-index mapping and count under a separate state lock. An existing
hardware or software binding returns `-EEXIST`. The 256-entry software table
uses a full-width index; XMCS no longer truncates the `0x7fff` sentinel to 255
or dereferences it before validation. IDs through 65534 are supported; 65535
is invalid. Ordinary creation rejects zero (the classifier's unset value),
channel zero and the assigned OMCC ID. ONU/OMCC assignment needs a separate
protocol transaction.

A unicast GEM can wait for its T-CONT or ANI assignment. T-CONT publication
updates matching pending GEMs and the allocation atomically, making packet
snapshots usable only once both are present. Data TX/RX use one bounded
snapshot of GEM, ANI, channel and Alloc-ID; there are no mutable table reads
across later hooks. TX rejects a hook that changes its selected GEM/ANI after
the snapshot. Accounting validates the mapping and uses accepted descriptor
metadata, including when native TX frees the skb before returning. Multicast
supports an assigned receive binding; multicast TX remains unsupported.

Active ANI/channel reassignment and nonzero encryption/loopback requests return
`-EOPNOTSUPP`. Removal or ANI unassignment marks the binding and its associated
channel retiring, permanently closes native TX admission and quarantines the
T-CONT. Other GEMs sharing that channel also become unusable. Native mappings
still pending return `-EAGAIN`; after reclamation, removal still returns
`-EOPNOTSUPP` because FE/RX/optical retirement is absent. Hardware entries,
software records, counts and IDs are retained, with no slot reuse. In-flight
snapshots therefore cannot become a different service binding. Reset attempts
retire both T-CONTs and GEMs, including pending/multicast GEMs even if T-CONT
retirement fails. Empty software tables are not proof of physical quiescence
and can change before identity reset. These legacy reset/reassignment paths
therefore stop even when both retirement calls return zero, until a coordinated
MAC/PHY/identity reset lifecycle exists.

Legacy automatic GEM replay is blocked. Creation emits no service-up event or
backup/replay state, and does not schedule the legacy GEM MIB timer. Key
selection, encrypted activation, live replacement, protocol-reserved ID policy,
initial hardware table reset and physical retirement remain integration work.
Readiness/provider gates, disabled PON board nodes, `BROKEN`, no autoload and
explicit service-start failure remain in place.

Validation: all 27 host PON tests pass, including actual command/ABI callers,
every 16-bit GEM ID, all three command timeout stages, verification faults,
concurrent creation, index 255, malformed channels, coherent snapshots,
reentrant callbacks, staged allocation, immutable bindings and reset gates.
Production TX/RX consumers are tested across mutating/retiring filter hooks,
with UBSan and bounded metadata checks. A separate Linux 6.18.44 UML guest
runs the actual helper and registry with real spinlocks, kthreads and hard-IRQ
control. It passes with all 256 slots and 111,936 packet snapshots, with no
kernel locking diagnostics. The six AN7581 vendor modules pass modpost and the
r17 APK builds locally. No SSH, device command, module installation or flash
operation was performed. These tests model registers and native drain; they
do not establish physical command timing or working optical service.

## Native FE TX-channel checkpoint (2026-09-13)

Kernel patch `9999c-net-airoha-pon-fe-tx-channel.patch` adds
`airoha_pon_set_tx_channel()` to the native GDM2/QDMA1 owner. The existing
AN7581 Ethernet definitions identify the GDM2 TX-channel bitmap at FE offset
`0x1524`; the adjacent RX enable and channel-release registers are separate
operations. This checkpoint implements only that 32-bit TX bitmap. It does
not infer RX or retirement behavior from the upstream loopback setup.

Attachment requires exclusive QDMA1 ownership and rejects active GDM2
loopback before any write. It verifies closure of all 32 x 8 QDMA queues,
then verifies that every FE TX channel is disabled before publishing the
handle. Enabling one channel requires all its queues closed and no remaining
native mappings; it leaves the queues closed. Queue opening and both packet
admission stages require a verified enabled channel. Disable permanently
closes that channel's admission before changing FE, preventing reuse by this
attachment even after descriptor reclamation.

The existing IRQ-safe admission lock serializes FE control, QDMA closure and
TX submission. The entire FE bitmap is written from a verified shadow and
read back, preserving other channels. Any FE/QDMA readback fault blocks all
software TX admission and subsequent enables. A later disable attempts to
clear the entire FE TX bitmap but still returns `-EIO`; it cannot clear the
fault or prove physical retirement. An ignored disable write is an error,
with queues closed and the channel permanently marked retiring.

Vendor patch `021-q1000k-native-fe-tx-channel.patch` routes T-CONT setup and
rollback directly through the new RCU-pinned adapter wrapper. The original
operation's error reaches the caller; rollback still attempts native closure,
FE disable and MAC invalidation. No partial ECNT FE provider is registered.
T-CONT/GEM removal continues to preserve mappings and report incomplete
physical retirement. Disable can leave frames buffered in FE/optical FIFOs;
it is not a drain operation. Detach still disconnects callbacks, and native
descriptor reclamation does not establish RX/optical quiescence. The complete
FE/QoS, physical drain, PHY and OMCI gates remain in place, with board PON
nodes disabled and no autoload.

Validation: all 27 host PON tests pass against the prepared sources. Native
fault fixtures cover all 32 bitmap positions, preserved channels, closed-queue
and pending-mapping preconditions, ignored enable/disable writes, mismatched
readback, QDMA-close failure, fault containment and disconnected handles.
The native Linux 6.18.44 UML guest passes 100 attachment cycles, concurrent
FE/QDMA/TX controls and real hard-IRQ enable/disable. The adapter UML guest
passes 50 cycles and checks exact forwarding of native errors. Both logs
contain no kernel diagnostics. The AN7581 kernel and r18 vendor APK build;
all six vendor modules resolve against the nine native API exports.
Registers and DMA remain modeled in these tests. No SSH or device access,
module installation or flash operation was performed.

## Q1000K RX framing checkpoint (2026-09-13)

Vendor patch 014 and the new `q1000k_packet.c` helpers establish the populated-skb
contract at the Q1000K receive callback. It rejects absent/short metadata,
zero/oversized or mismatched packet lengths, descriptor errors and aggregation
before copying metadata or inspecting the payload. It linearizes assembled
page fragments before the imported direct-pointer parsers. The Q1000K path
removes both late `skb_put(pktLen)` calls and the management-frame recopy;
loopback also receives the existing length. Missing/down upper interfaces drop
the packet before accessing their private data.

Management selection uses the descriptor OAM flag. Q1000K no longer reclassifies
arbitrary bytes 12/13 as EAPOL, and its OMCI path does not call
`eth_type_trans()`. It preserves the entire raw frame, sets the packet type and
header offsets, and leaves checksum status unverified. Ethernet data still has
its Ethernet header parsed normally. The legacy targets retain their previous
receive contract; only AN7581/Q1000K compiles the new helper object.

OMCI validation here bounds reads made by the imported parser: its baseline
CMAC input is 44 bytes, while extended content length is the 16-bit field at
bytes 8/9 after a 10-byte header. If the descriptor selects the existing software
MIC verifier, four further bytes must be present. Length arithmetic uses an
unsigned value large enough to reject overflow/truncation. These checks do not
verify MICs or certify the descriptor's authentication semantics; key management,
MIC enforcement and service interoperability remain unimplemented.

A host fixture runs the actual prepared vendor callback with its native packet
helpers. It verifies unchanged raw management bytes, Ethernet delivery,
loopback length, missing/down interfaces, one owner on drops, metadata errors,
allocation failure and every possible extended length with both MIC-flag values.
All 18 host PON tests pass. A separate UML test executes the production helpers
with real Linux skbs, page fragments, receive backlog and an AF_PACKET socket:
three raw OMCI cases and one Ethernet case arrive byte-for-byte unchanged at
the raw socket, with no kernel diagnostics. This tests framing, not physical
optical transport or an OMCI daemon.

The complete six-module vendor package builds and passes modpost as
`kmod-airoha-xpon-en757x-6.18.44-r11.apk`; the real receive object references both
new helpers. It remains `BROKEN`, unselected and without autoload. No module,
package or firmware has been loaded on the device in this continuation.

## OMCI transmit framing checkpoint (2026-09-13)

The original transmit path changes `skb->len` directly while removing and
adding the four-byte OMCI trailer, leaving the tail inconsistent. Its extended
length calculation can wrap at 16 bits, accepts truncated input, and discards
unexpected extra data. Its software-MIC caller also returns success after a
CMAC error. These are prerequisites to resolve before native TX submission.

Vendor patch 015 uses `q1000k_omci_tx_prepare()` on Q1000K. It accepts only a
complete baseline/extended body, optionally followed by one four-byte trailer,
and rejects unsupported offload metadata. It linearizes fragments, makes
cloned data writable, reserves room for a replacement MIC and trims with the
skb API. The imported MIC caller validates direct calls as well, appends via
`skb_put_data()` only after success, and returns failure for CMAC errors.
Failure leaves buffer ownership with its caller. Other targets keep the
imported implementation.

All 19 host PON tests pass. New tests execute the helper and actual imported
MIC caller, including every extended length value and allocation/CMAC faults.
A Linux UML test passes 80 combinations of baseline/extended, linear/fragmented,
cloned, trailer present/absent, success and failures. Length/tail consistency,
unchanged parent clones, cleanup and failure propagation are checked with real
skbs; DMA and CMAC calls are fixtures. No kernel diagnostics occur. The complete
vendor APK builds as release 12 without unresolved symbols or autoload.

This checkpoint does not establish hardware CMAC/key selection, authenticate
incoming MICs, connect the native TX adapter or validate an OMCI service.

## Native vendor packet adapter checkpoint (2026-09-13)

Vendor patch 016 connects the MAC's Q1000K packet path to the native Ethernet
consumer API. An explicit read-only `pon_lower` module parameter selects the
lower netdevice; there is no guessed interface name or automatic device open.
Native attach validates the board/port/role and requires an already running
lower. Attachment occurs before MAC state/PHY initialization. The MAC readiness
predicate also checks the attachment, so a lower detach closes runtime packet,
hook and netdev-open entry points even if the module remains loaded.

The adapter preserves the callback's populated raw RX skb and copies the four
native descriptor words into call-local storage. Q1000K no longer installs
legacy QDMA RX/event callbacks, enables/disables shared DMA or interrupts during
packet attachment, or compiles the vendor NAPI controller. Legacy packet events
return `-EOPNOTSUPP`. The Ethernet owner retains the actual rings, IRQs and NAPI.

TX has an explicit ownership boundary: zero means the adapter accepted the skb;
negative errno leaves it unchanged with the vendor caller. The caller frees
rejected packets once and always returns `NETDEV_TX_OK` after consuming them.
Accepted packets enter a FIFO limited to 128 entries, including the worker's
in-flight entry, with a one-second expiry. A deferred worker retries native
`NETDEV_TX_BUSY` without re-running packet/OMCI preparation. It holds no adapter
lock across native submission, and always arms a one-jiffy fallback after BUSY;
the native wake callback only expedites work. Per-entry metadata is independent
of `skb->cb`. A submitting netdevice reference keeps `skb->dev` valid until native
consumption or a software drop, including upper unregister during backpressure.

Translation keeps GEM, optical channel, queue and MIC index, while the native
CPU ring/group remains zero. It accepts the vendor's channel-to-NBOQ convention
and disabled meter/account defaults; custom DMA groups, offload/PPE, checksum,
metering and accounting requests are rejected. Kernel patch 9999 preserves
the descriptor's OMCI no-drop hint, without changing data/Ethernet policy.
The hint does not guarantee delivery or bypass the software queue's limits.

Stop removes producer publication, drains RCU readers, cancels retry work,
quiesces native TX with a one-second reclamation bound, releases the handle and
purges unsent packets. A timeout is returned and logged, while callbacks and
consumer storage are still safely released under the native handle contract.
This neither gates physical TX nor drains FE/optical FIFOs. Unsolicited native
detach closes ingress and lets already scheduled work purge its software queue.

All 20 host PON tests pass, including the actual vendor TX caller, native
descriptor emission for management/data, and failure injection at every MAC
startup stage. The real Linux UML adapter test passes queue saturation, FIFO
ordering, a wake racing BUSY, recovery with no new wake, expiry, upper unregister,
early callbacks, failed attach/allocation, lower detach and quiesce timeout.
Fifty concurrent producer/RX lifecycle cycles processed more than 800,000 skbs
with balanced allocation/destruction counts and no kernel diagnostics. UML uses
real workqueues, RCU, netdevices and skbs, with a synthetic native DMA provider;
the native owner's implementation is covered by its separate existing fixtures.

The full Linux 6.18.44 build and release-13 vendor APK pass compilation/modpost.
No device connection, module loading or firmware flash was used. PON nodes remain
disabled and packages remain optional with no autoload. ECNT QDMA control and FE
providers are still mandatory: this packet adapter does not implement flow,
T-CONT provisioning, queue retirement, physical drain or PHY/OMCI service.

## MAC identity handoff

The Q1000K MAC now requires `wan_mac` (exact colon-separated 6-byte unicast
MAC) and `pon_serial` (four ASCII vendor characters plus eight hex digits)
module parameters. Both are parsed and copied to immutable module-local
storage before initialization; missing or invalid input returns an error.
The parameters are not exposed as mutable sysfs files. `get_ethaddr()` now
reads that validated cache, and GPON initialization consumes the decoded
8-byte serial. Q1000K EPON compatibility callers also use the cache instead
of raw flash offsets or the OEM `GetMacAddr` pointer API. WAN interface
creation rejects identity/registration errors and no longer reports success
when its netdev was not created.

The cached OEM boot log has `onu_type=71`; the vendor masks/enumerations
interpret `0x71` as XGS-PON (7), SFU (1), combo flag clear and BBF247 clear.
This board-specific integration selects those fields and rejects non-XGS
`mode` overrides. The physical pair of EN7573AN controllers does not imply
the vendor combo-PON software flag. Actual optical behavior still needs bench
validation. Registration ID/MSK defaults in the imported source are not a
validated authentication configuration.

The eventual launcher must pass the existing backend's selected factory or
UCI override identity. It must not source shell commands from identity data.
No launcher, automatic module load or working service is installed by this
checkpoint. Tests cover malformed/trailing/absent values, multicast/zero MACs,
FSAN byte order, unchanged outputs on errors and rejection before startup.

## Cryptographic primitives

The imported cipher-to-lskcipher conversion requested `aes`, which the current
kernel registers as a raw CIPHER algorithm. The port now requests `ecb(aes)`
as an LSKCIPHER and declares the ECB package/AES kernel dependency. It checks
set-key and encryption/decryption errors, serializes shared transforms across
each complete request, validates ECB block lengths, and leaves output buffers
unchanged on failure. CMAC no longer advances beyond vector bounds and handles
zero-length fragments and empty messages. Temporary derived blocks are cleared.
Security setup publishes the CMAC/ECB pair only after both allocations succeed;
failure releases the first transform. Teardown shuts down both initialized
timers synchronously before freeing transforms and clears their pointers.

The production functions pass NIST SP800-38B AES-128 CMAC examples (also in the
kernel's `crypto/testmgr.h`), an AES ECB known-answer test, all splits of input
lengths 0 through 257 with empty fragments against OpenSSL CMAC, and failure
injection at each crypto call. ASan/UBSan check the host execution. Separate
fixtures verify allocation failure, duplicate/retry initialization and timer
shutdown ordering. The extracted production helpers also pass known-answer
tests against the actual Linux 6.18.44 crypto API in an isolated UML guest,
including transform allocation. On-device key transitions, MIC enforcement
and complete callback teardown remain hardware/runtime acceptance work.

## Native scheduler integration (2026-09-13)

Kernel patch `9999d-net-airoha-pon-qos.patch` implements checked per-channel
scheduler access through the QDMA1 owner. All 32 channels and eight queues
are addressable. Supported modes are WRR8, strict priority, and WRR7 through
WRR2 with the remaining queues assigned higher strict priority. The channel
position within a scheduler word is masked to 0..7 before shifting; using the
full PON channel index would shift past the 32-bit word for channels 8..31.

Both set and get are process-context operations serialized under RTNL. The
attachment remains owned across indirect-command polling, and that channel's
queues must already be closed with native mappings reclaimed. Configuration
marks the channel busy so IRQ-side queue/FE controls cannot reopen it during
programming. Set verifies all eight weights through explicit read commands,
then verifies the complete scheduler mode word to detect changes to peers.
The global weight unit/scaling is preserved and checked, never silently changed.
Timeout or mismatched completion/readback latches a control fault. Get publishes
its output only after the entire operation succeeds. Get itself issues MMIO
commands and is not a permitted read-only device diagnostic.

The packet adapter pins these sleepable operations with its lifecycle mutex,
without holding RCU. Vendor patch 022 routes XMCS channel scheduler set/get
through those APIs. The ECNT RCU callback rejects the sleepable operation with
`-EWOULDBLOCK`; the process-context interface remains available. Native 16-bit
weights that cannot fit the legacy eight-bit XMCS ABI return `-ERANGE` rather
than being truncated. Scheduler programming does not open queues, retire
hardware traffic, implement rate shaping, or supply the remaining FE provider.

Host fixtures exercise every channel/mode/unit combination, preserve unrelated
register bits and QDMA0 storage, inject failure at every indirect command,
and check timeout, mismatched readback, ignored writes, stale ownership,
retirement and unchanged outputs. The complete native transport passes UML
with 32-channel QoS programming plus 100 attachment/packet/IRQ cycles. The
adapter passes 50 UML cycles, including sleepable wrapper calls, error
forwarding, real workqueues and RCU. These tests model registers; physical
scheduler behavior remains untested.

All 29 PON host tests pass. The Linux 6.18.44 AArch64 kernel and vendor
package build successfully; the r19 vendor APK is 335,048 bytes. All six
vendor modules resolve against the kernel and package symbol tables,
including the two new scheduler exports.

## OMCI implementation decision

The two candidates were compiled locally, without installation or execution on
the Q1000K:

| Candidate | Evidence | Decision |
| --- | --- | --- |
| PR #24577 native `econet-omcid` | AArch64 compilation succeeds; EN7528 procfs transport, baseline-only framing and DZS/H660GM-A MIB. | Retain as a reference. Do not import its successful no-op responses or unsolicited GEM setup. See the [PR review](XGSPON-PR24577.q1000k.md). |
| Generic `net/xpon` and `net/xpon/omci` at [2e2cf91](https://github.com/Sirherobrine23/airoha_kernel/tree/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon) | Both external modules compile and pass modpost on Linux 6.18.44/GCC 14.4.0. Baseline/extended wire codec, managed entities, service reconciliation, identity, sysfs and netlink are present. Only module-description warnings appeared. | Imported as the optional `q1000k-omci` core package; Q1000K hardware transport and service callbacks remain to be connected. |

The generic source requires callbacks for transmit, T-CONTs, GEM ports, UNI
state, service replacement/deletion, telemetry and operational-state reporting.
Its hardware driver remains GPON/EPON, not AN7581 XGS-PON. The generic agent
also needs corrections before use:

- `omci_device_register()` requires only `xmit`. Some T-CONT/GEM/UNI operations
  return success when their corresponding callback is absent. A Q1000K adapter
  must require its advertised capabilities and return unsupported errors for
  absent hardware operations.
- Service replacement checks for a callback, but deletion can be skipped when
  no deletion callback exists. Require symmetric installation/removal and
  observable rollback failures before reporting the software state committed.
- The exposed password normalizer accepts the GPON 10-byte password. Define
  the actual XGS-PON registration/authentication contract separately; do not
  impose that limit on XGS-PON credentials.
- Baseline/extended frame decoding permits frames with or without a MIC. MIC
  verification/stripping and XGS-PON key selection must be provided by the
  transport, with failed authentication rejected before managed-entity updates.
- Use the validated Q1000K factory FSAN. Do not call the generic random-serial
  fallback or substitute another board's managed-entity identity.

No service VLAN, OLT identity, authenticated OMCC exchange or successful OEM
MIB trace is available from the current disconnected, non-OEM installation.
The supplied OEM boot log also reports no PON signal. Therefore the intended
service model and its managed-entity behavior have not been established.

## OMCI core integration (2026-09-13)

The generic sources are now imported with original authorship in a separate
commit. `package/kernel/q1000k-omci` builds the PON and OMCI modules and stages
provider headers/symbols. Neither module autoloads; the package remains
experimental and does not enable board nodes or provide a hardware backend.

The adaptation fixes the missing-callback and service consistency issues
listed above. Startup requires explicit identity and all required callbacks.
RX requires an explicit per-packet authentication verdict, rejects CRC errors,
linearizes fragmented skbs, and enforces the queue limit under lock. Session
transitions serialize with request processing. Stop prevents new enqueues
before waiting for RX work and provider TX to finish. Failed startup cannot
process synchronous RX. Baseline trailers and extended length arithmetic are
validated, including rejection of 16-bit length overflow.

Service callbacks now replace a complete set atomically. Both software
snapshots exist before hardware changes, and publication requires no further
allocation. Failed removal preserves records and aborts MIB reset; an
inconsistent backend or failed session cleanup blocks subsequent provisioning.
Fake-success/permissive settings and their profile bypasses are disabled.
See the [provider contract](../../../package/kernel/q1000k-omci/README.md).

The complete core passes a local AArch64 module/package build and an isolated
Linux 6.18.44 UML run with lockdep, RCU and atomic-sleep checks. The fixture
covers authentication admission flags, malformed/nonlinear packets, identity
and callback gates, full RX queues, 20 stop races, missing provisioning,
service-set failures, MIB preservation and a latched backend inconsistency.
The fixture supplies authentication assertions; it does not implement or
validate the Q1000K's cryptographic receive path.

## Validation boundary and remaining plan

Completed local checks: factory/backend/LuCI host tests, controller transport
and every-transfer fault injection, read-only controller status tests, controller
APK build, BSP/PHY/MAC modpost, complete vendor APK generation, resource/hook
fixtures, validated MAC/FSAN handoff, unsupported-control error propagation,
AES/CMAC known-answer and fault-injection tests, native packet ownership and
DMA/RX fixtures, and real Linux attachment/RCU tests in UML. The normal builder and
protected source branches remain unchanged.

Outstanding software includes complete analog/SoC PHY sequencing, shared
resource ownership, native FE/QoS providers, physical per-channel/GEM retirement,
encrypted GEM activation and ONU/OMCC transactions, identity handoff from the launcher, required flow
operations, AN7581 OMCC transport, OMCI service support, and the actual
procd/netifd lifecycle. Do not install an init script that merely reports
success while these components are absent. CLI `start`/`restart`/`reload`
continue to fail explicitly.

Production-DT boot, PHY programming, registration, OMCI provisioning, optical
traffic, reconnect/reboot recovery and offload validation all require state
changes on test hardware. They cannot be completed through the current
read-only device connection. No full service image or completion claim is
appropriate until those acceptance gates pass. The optional diagnostics
profile is the only supported build profile at this checkpoint.

## Full XGS registration identity and software authentication

Vendor r31 requires a hidden `pon_reg_id` module argument containing exactly
72 hexadecimal digits. It decodes all 36 registration bytes into immutable
MAC-owned storage and passes those bytes to the imported registration field;
there is no imported placeholder or ten-byte GPON-password conversion.
Failure invalidates the complete cached identity. Parameter and cached buffers
are separate, and temporary registration bytes are erased.

The new authentication helpers implement registration MSK, session, OMCI,
PLOAM and KEK derivation with the existing serialized synchronous AES-CMAC
helper. They follow [G.9807.1 C.15.3](https://www.itu.int/epublications/ar/publication/itu-t-g-9807-1-2023-02-10-gigabit-capable-symmetric-passive-optical-network-xgs-pon).
MIC calculation prefixes downstream/upstream direction 1/2, covers the full
PDU including the baseline trailer and compares received MICs in constant
time. Exact frame lengths and the 1980-byte protocol limit are checked before
allocation. Fragmented skbs use `skb_copy_bits`; no hardware CMAC DMA buffer
is allocated. Failed operations preserve outputs and erase temporary keys.

Official key and downstream MIC vectors are checked in host and UML tests:
[G.987.3 Appendix IV](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.987.3-202505-I%21%21PDF-E&lang=e&type=items)
and [G.9807.1 amendment golden vectors](https://www.itu.int/epublications/es/publication/itu-t-g-9807-1-2023-amd-1-2025-05).
The host tests alter every baseline bit, exercise every extended length,
every packet split, crypto/allocation failures and the final registration
byte. The UML guest uses the real kernel AES implementation and fragmented
skbs. All 43 PON host tests pass and r31 builds for AArch64/Linux 6.18.44.

These helpers establish cryptographic byte processing. The backend must still
bind keys, packet admission and OMCI state to a verified registration session
and invalidate stale packets on session changes. No device was accessed.

## OMCI authentication epochs

OMCI core r2 binds RX admission and provider TX to explicit authentication
epochs. The provider closes the epoch and waits for current transactions and
TX callbacks before changing key material. A new verified key set needs a
strictly increasing token. Stale RX and TX are rejected, and channel/ONU
replacement invalidates authentication. Rekey preserves the MIB. The 64-bit
packet generation saturates and permanently closes admission; it never wraps.
The software-MIC capability gives the provider a PDU without claiming hardware
MIC support. The provider's asynchronous TX queue must retain the token.

The full-core UML suite passes 20 rekey/queued-RX races, an in-flight provider
TX barrier, old-token rejection, ONU/channel reassignment, generation exhaustion,
and the existing stop/service fault tests. The package now declares its actual
kernel module subdirectory for OpenWrt symbol collection and stages provider
symbols for the MAC backend. These are local checks, with mock provisioning;
MAC key publication and the actual backend remain to be connected.

## Authenticated MAC retry admission

Vendor r33 adds a dedicated OMCI transmit entry point carrying the verified
authentication epoch. The legacy packet entry point rejects OMCI without this
token. The bounded queue captures the token once; BUSY retries never refresh
it. A process-context epoch barrier waits for an in-progress native OMCI
submission, purges old OMCI retries, and leaves data packets intact. Old tokens
cannot be republished on the attachment. The lock order permits native wake
callbacks without holding the adapter queue lock across native TX.

The real-workqueue UML adapter suite passes 20 mixed data/OMCI rekey cycles,
BUSY wake races, an in-flight native submission barrier, stale-token rejection,
and existing attachment/expiry/unload races. All 44 host PON tests and the r33
AArch64 package build pass. This barrier covers software retries and native
submission calls. Descriptors already accepted by DMA and optical FIFOs still
require the physical drain sequence before namespace reuse or hardware reset.
The OMCI backend must coordinate both core and adapter epoch barriers.

## Verified integrity key-bank programming

The r37 MAC code derives both XGS-PON integrity/wrapping banks in software,
using all 36 registration bytes for bank zero and the default MSK for bank
one. Bank one's PLOAM key is the specified constant. Programming requires the
physical transaction's INSTALL owner, disables hardware OMCI MIC processing,
preserves unrelated control bits and key-selector policy, and verifies every
PON-tag/key register write. Key-word all-ones values are valid data; control
register all-ones reads remain errors. Software publication belongs to the
session owner after the complete transaction succeeds.

An unchanged-record refresh now runs a key installation callback after the
CPU, FE, MAC, RX and PHY drains and before GEM replay/reactivation. Existing
queue closures and service records survive a successful refresh; a failed
installation poisons the port. This closes the physical boundary missing from
software-only authentication barriers. The key fixture injects all 27 write
failures and all nine derivation failures; the record fixture checks refresh
ordering, service preservation and containment. This is local validation,
not hardware acceptance or an already connected OMCI session backend.

## Registration namespace owner

The r38 registration entry point replaces the complete ONU/OMCC/data
namespace under the physical drain coordinator. It accepts ONU IDs 0..1022
or an explicit unassigned value, clears old OMCC lookup slots, verifies the
10-bit ONU register and validity flag while preserving unrelated bits, and
leaves every queue closed until registration explicitly enables OMCC traffic.
An unchanged OMCC in a data-only transaction retains its distinct RX/TX flags;
a changed registration publishes the newly programmed record. All 50 host
fixtures and the AArch64 package build pass, including failure injection at
every registration replacement step. The PLOAM caller connection follows in
the OMCI session owner; the helper alone does not establish a usable session.

## Connected authenticated OMCI and unicast service provider

The r39 MAC package registers the generic PON/OMCI core on the `pon` netdevice.
Native OAM RX now verifies a complete software MIC with the selected verified
key bank and passes the exact authentication epoch to the core. TX appends its
MIC to a private copy and transfers the original only after bounded native
queue admission succeeds. Raw netdevice OAM injection is closed. Data traffic
uses the core's ANI-side VLAN/PCP rules, coherent GEM/T-CONT bindings and explicit
per-channel queue masks; VLAN offload tags are materialized, checksums completed
and short Ethernet frames padded before native submission. UNI disable and
missing/ambiguous service rules reject traffic. This provider does not rewrite
customer-side class-171 VLAN treatments: the imported core explicitly describes
the network-facing VLAN on `pon`.

PLOAM profile and ONU assignment now request ordered session work. A dedicated
control job runs on the MAC worker without its execution mutex, ahead of later
protocol events. Core RX/TX/session barriers can therefore finish an active
provisioning call that needs the executor. The control callback then obtains
execution ownership for physical retirement, verified key/ONU/OMCC replacement
and software publication. Stale request generations cannot publish an old ONU.
Profile acknowledgements follow verified key programming. The registration
namespace's receive-only activation keeps controller TX disabled after removal.

The core's new registration-reset API closes admission and restores the MIB's
initial ONU-created entities, removing the old OLT's objects. This is separate
from an ordinary key change, which preserves the MIB. Private staged core
headers and its symbol versions are explicit MAC build dependencies. The core
now reports Q1000K equipment identity instead of its imported EN7523 default.

Validation: all 52 host PON tests pass, including actual provider/session code,
packet ownership, stale requests, old-key rejection, every modeled physical
failure, VLAN/PCP rules and lifecycle unwind. The protocol UML test passes with
control work waiting outside the executor while another service enters it.
The complete OMCI core UML test passes, including registration MIB reset and its
existing RX/rekey/TX/teardown races. The AArch64 MAC/core package build and
modpost are checked as part of this checkpoint. No device access occurred.

Cold MAC startup and legacy reset/PHY-ready/loss callers, remaining scheduler
managed-entity controls, encrypted/multicast data and profile-seeded services
still need their final integration. The existing startup gate, BROKEN marking,
disabled PON node and lack of autoload remain until that work is complete. These
local test results do not establish optical service or hardware timing.

## OMCI scheduler controls

MAC r40 and OMCI core r4 connect the Priority Queue and Traffic Scheduler
managed entities to native QDMA1 QoS. A policy change or weight update on an
assigned T-CONT retires the complete physical namespace, reads the actual
shared units, programs and verifies the channel scheduler, then restores queue
admission. Unassigned entities retain their policy and weights; service
activation installs them into the PLOAM-selected channel before queue opening.
The native mode mapping is strict priority (1) or eight-queue WRR (0), with
weights 1..127. Queue entity order preserves the advertised reversed priority
order. Mixed native modes remain internal rather than claiming an unsupported
OMCI scheduler hierarchy.

The core now decodes complete scheduling candidates, rejects missing hardware
callbacks, preserves the previous MIB after rejection, and records EUCLEAN for
an uncertain hardware result. Fixed queue wiring, shared-buffer allocation,
backpressure and T-CONT policy are read-only. ONU2-G advertises only scheduler
policy flexibility. Policy values follow [ITU-T G.988 section 9.2.11](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.988-202403-I%21Amd1%21PDF-E&lang=s&type=items).

Validation includes the actual provider with unassigned/assigned entities,
WRR limits, strict-priority transitions, shared-unit preservation and every
modeled physical failure. All 52 host tests, the full OMCI UML suite, and the
AArch64 MAC/core builds pass. No device access occurred. Startup/reset, PHY
ready/loss callers, profile-seeded services and advanced data service support
remain the next integration work; optical service is not established.

## Profile-seeded services and PLOAM reconciliation

MAC r41 and core r5 install normalized profile-created GEM/T-CONT intent,
including profiles that seed their MIB without explicit OMCI Create requests.
The replacement candidate includes records, entity mappings, classifier rules
and channel QoS; its installation runs inside one physical drain. Missing
PLOAM allocation produces a dormant GEM mapping with no open data queue.
A later Alloc-ID assignment schedules ordered core reconciliation, resolves
the actual channel and installs its scheduler before queue admission. Removal
and reassignment cannot retain an open old channel. Explicit OMCI-created
GEMs retain their independent Create/Delete lifetime; profile-owned records
follow the complete service set.

A GEM's explicit upstream Priority Queue pointer now overrides PCP fallback
and must identify a queue on its T-CONT. Nonfatal preparation errors do not
permanently poison core service state; uncertain physical replacement still
returns EUCLEAN and contains the port. PLOAM allocation callers propagate
notification failures and handle duplicate assignment/deallocation requests
without repeating an invalid namespace change.

All 52 host tests pass, including seeded/dormant services, actual reassigned
channels, profile deletion, conflicting entities, error forwarding and ordered
backend reconciliation. The full core UML suite and AArch64 core/MAC package
builds also pass. No device access occurred. The next work includes the
userspace q1000k-omci command requested by the user, complete startup/reset and
PHY-ready/loss integration. Advanced encryption and multicast remain separate
from this unicast path.

## Userspace OMCI management command

The `q1000k-omci-tools` package provides the `q1000k-omci` command requested by
the user. It keeps the imported/adapted kernel core as the protocol owner and
uses its Generic Netlink API for status, MIB inspection and validated runtime
configuration. It does not add a second daemon. The [command reference](../../../package/network/utils/q1000k-omci-tools/README.md)
describes selectors, named profiles, JSON output and the exact local tests.

Core r6/API version 15 adds strict interface-index selection, authentication
admission status, configured rule count and service/reconciliation errors.
Absent telemetry stays unavailable; configured rules are not labelled as
working optical service. The client validates reply framing, supports bounded
MIB traversal, preserves 64-bit counter precision and does not expose keys or
registration-password writes. No activation or autoload is added.

The current PR #24577 head was rechecked through the GitHub API and remains
`d7569c5e26551084e7643b0e83ecda9c31f49f11`. The `econet-omcid` reuse review still
applies. Its user-facing management concept is useful; its EN7528 procfs
transport and successful no-op protocol replies are not imported. The user
confirmed continuing with the existing generic core.

## OMCI diagnostics in LuCI

The imported econet-xpon application now reads the q1000k-omci command's
structured status through a separate JSON namespace. It exposes activation
state, authenticated admission, agent exchanges, MIB size, configured service
rules, reconciliation errors and exact decimal packet counters. Missing, failed
or wrongly typed samples become unavailable. Configured rules may be dormant;
service readiness remains unknown. No configuration write RPC was added.

The MIB page requests a live read only when its button is pressed, instead of
walking hundreds of entities on every status poll. RPC selects the fixed `pon`
interface and fixed read command. Failed reads replace old results. CLI absence
is supported, so the normal diagnostics package does not force experimental
kernel modules into an image. All 13 factory/backend tests and the LuCI view
fixtures pass. Matching PON/core/CLI/backend/LuCI AArch64 packages build locally;
no package was installed on the device.

## Checked cold-reset primitives

Vendor r42 adds a cold-reset variant of the existing physical namespace
transaction. It drains CPU/FE/MAC/RX/PHY first, resets the exclusive MAC,
releases its local reset while MPI/MBI remain stopped, and verifies/clears
all 65,535 usable GEM IDs plus the data T-CONT namespace. It publishes an
empty ONU/OMCC/data record set and resumes receive only, with every packet
queue and controller TX closed. The authentication owner must supply the
complete MAC install callback after its core/retry barriers.

The new install primitives program and verify full serial/registration data,
AN7581 XGS response time and FEC, software OMCI MIC mode, dying-gasp count,
idle threshold, invalid data AES banks, invalid ONU, cleared ranging delay
and burst-profile validity, and O1/O7 state. Key selection verifies both the
SW command and actual current bank-one selectors. AN7581-specific mixed
register fields exclude read-only status and software triggers from writes.

Cold reset also cancels old deferred timer/task/PHY events without destroying
their registrations or losing queued IRQ/control jobs. Real Linux UML tests
verify cancellation, reuse and balanced IRQ disable depth. All 54 host tests
and the AArch64 vendor build pass. Legacy startup/reset/PHY callers are the
next integration stage; their startup gate remains in place. No device access
occurred.

## Cold lifecycle connected to registration

Vendor r43 connects module bootstrap, deactivation, ranging timeout, LOS and
emergency stop to the ordered OMCI registration owner. It closes authentication
and service admission before waiting for core work, drains the physical pipeline,
installs the cold discovery state, and publishes O1/O7 only after success. Cold
PHY configuration runs inside the drained install phase. Old ranging/key timers,
duplicate PLOAM state, AES validity and software registration state cannot survive
this boundary. Repeated LOS while already in O1/O7 only checks that TX is off.

PHY-ready changes to O2/3 only after the MAC state write and optical TX enable
succeed. Legacy direct reset/enable and MSK replacement entry points reject
bypasses. The Q1000K PHY-event dispatcher uses the fixed XGS mode and routes rogue
or TX-fault events to emergency reset. Unsupported fast recovery and mode changes
remain rejected.

Controller r4 adds a verified TX-state snapshot under its lease lock. PHY callers
wait for active callbacks before sampling it. Ordinary profile, registration and
QoS rebuilds preserve that optical state, including discovery TX before ONU
assignment; cold reset and ONU removal always resume receive only. A failed
snapshot contains the port. Key-selection verification accepts self-clearing
command-enable bits while checking both requested and actual hardware indices.

All 55 host tests pass, including actual legacy caller extraction, bootstrap
failure injection, emergency/LOS routing, TX-mode preservation and invalid
full-width activation/reset arguments. Real Linux UML PHY callback/lifetime
tests pass with lockdep enabled. Matching controller and vendor AArch64 packages
build successfully. This checkpoint does not remove the remaining
legacy FE/QDMA startup gate; key-transition and remaining default-call audits
continue before enabling that path. No device access occurred.

## Verified registration-key transitions

Vendor r44 uses a single checked read of the hardware PLOAM/OMCI selectors
before accepting ranging or entering O5. Both must select registration-derived
bank zero. Partial switches, read faults and unavailable keys close admission
and contain the protocol; software indices and registration-reported state
remain unchanged. A verified switch updates PLOAM, OMCI and KEK together and
requests the existing ordered authentication boundary.

Repeated Request_Registration messages in O5 retain that verified bank and send
the requested registration reply. They no longer toggle software indices.
Legacy userspace MSK, broadcast-key and MIC-mode writes reject before changing
cached security state. Direct legacy selector setters cannot bypass the owner.
Mutual-authentication rekey and data-encryption provisioning still need separate
transactions; this checkpoint does not claim those modes work.

All 56 host tests pass, including selector-pair failures, read failures, repeated
requests, no publication after a failed O5 transition and unsupported security
writes. The complete AArch64 vendor package builds successfully. No device
access occurred; the remaining FE/QDMA and PHY default-call audit continues.

## Native default-path ownership and packet accounting

Vendor r45 removes the legacy FE/QDMA hook-presence gate. The native attachment
now validates and pins the real Ethernet owner before MAC initialization, and
all packet admission, queue closure, scheduler, drain and retirement operations
on the Q1000K default path use that owner. A compiled/preprocessed call audit
confirmed that legacy GEM packet helpers and EPON FE hooks are bypassed by the
native OMCI/service callbacks. Optional ToD and customer hooks do not provide
packet or queue ownership.

Q1000K no longer requests the EPON software dying-gasp IRQ handler, which called
OEM power-optimization hooks and wrote shared SCU state. XGS hardware dying-gasp
configuration remains in the checked cold MAC defaults; CPU power optimization
is not provided. The EPON-only periodic traffic notification is not armed, and
the legacy MCI ioctl rejects EPON operations on the uninitialized EPON stack.
The board nodes remain disabled and the package remains experimental without
autoload; removing an obsolete provider check does not validate optical service.

Native RX now returns a defined error for every drop and consumes each packet
exactly once. It checks the data interface is running, accounts accepted frames
before removing their Ethernet header, and separates OMCI from data counters.
Successful service TX/RX updates the per-GEM counters through the guarded
registry. Failed classification and failed submissions do not count as traffic.

All 57 host tests and the AArch64 vendor build pass. Tests cover every RX drop,
OMCI independence from data-interface state, statistics, and full startup and
unwind with no legacy FE/QDMA hooks. PHY burst-profile programming still needs
its checked install boundary; no device access or optical test occurred.

## Verified burst-profile installation

Vendor r46 carries all four upstream burst profiles through the ordered
registration owner. PLOAM reception validates XGS line rate, destination,
profile index, version, FEC and the supported nonzero 1–8 byte pattern lengths.
PHY preamble, delimiter, repetition and FEC fields are written and read back
only while physical producers are drained. The MAC publishes each profile's
length/version/valid bit after its PHY programming succeeds. Rebuilds replay
the complete accepted set; a changed PON tag or cold reset invalidates it.
ONU assignment and authenticated service require an installed profile.

Unicast ACKs are queued until installation succeeds. Conflicting profiles or
tags cannot supersede an outstanding ACK, including a repeat of the current
tag that needs no key derivation. Generic reset, mode, TX and profile SETs
cannot bypass typed lifecycle ownership. Receive enable and FEC settings have
checked writes and preserve peer controls without replaying clear strobes.

All 58 host tests, the PHY UML callback/lifetime test and the AArch64 vendor
build pass. Fault injection covers each profile operation, all four banks,
MAC validity ordering, pending tag changes, and malformed FEC requests.
This validates software ordering against simulated registers. Cold optical
registration and interoperability with an OLT have not been tested; no device
access or firmware installation was performed.

## OMCI rollback failure containment

Core r7 and vendor r47 connect a nonblocking service-fault callback. Failed
CREATE, SET, DELETE or profile rollback now records `-EUCLEAN`, marks the
agent nonoperational and closes Q1000K packet admission before the protocol
worker performs physical containment. The callback does not call back into
the OMCI core while its mutex is held. Subsequent provisioning and MIB reset
cannot clear the fault; diagnostic snapshots remain available, and recovery
requires a new device registration.

The full core UML suite exercises actual T-CONT SET and GEM CREATE/DELETE
transactions with service rejection, successful undo and failed undo. It
checks preservation of the old MIB, one fault callback and rejection of later
mutations. Backend host tests check immediate data/authentication closure.
All 58 host tests, full OMCI core UML and matching AArch64 core/vendor builds
pass. These tests use simulated providers, with no device connection.
