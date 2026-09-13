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

The SoC PON PHY uses the OEM `1faf0000`, `1faf3000` and `1faf4000` regions.
The existing `pon_pcs` node instead describes the `1fa08xxx`/`1fa8xxxx`
serial-interface PCS/PMA and shares XPON reset IDs. Enabling that PCS is not
proof of the internal optical path. The board keeps both `pon_pcs` and `gdm2`
disabled pending the correct connection/reset sequence.

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
described below. GEM mapping synchronization and physical retirement remain
incomplete. Packets whose vendor metadata was prepared before admission also
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
unchanged rejected skbs and unregister/timeout cleanup. Both guests report no
kernel, RCU or locking diagnostics. These are synthetic register/DMA tests,
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
returns `-EOPNOTSUPP`, because neither per-channel native descriptor drain nor
FE/optical FIFO retirement exists yet. Bulk removal attempts every assigned
channel and preserves the first control failure. It does not use the vendor's
global `G_TX_FCS_TBL_INIT` write as a per-channel retirement mechanism. The
`gpon_disable()` and inconsistent ONU-ID reassignment paths stop before clearing
identity or resetting the MAC when bulk retirement is incomplete. Other legacy
reset paths and ONU/OMCC assignment still need their own lifecycle audit.

GEM mutation and packet metadata preparation are not yet synchronized with
these transactions. The setup success path is tested with a synthetic FE
provider; a native FE implementation is still absent. These changes therefore
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

## OMCI implementation decision

The two candidates were compiled locally, without installation or execution on
the Q1000K:

| Candidate | Evidence | Decision |
| --- | --- | --- |
| PR #24577 native `econet-omcid` | AArch64 compilation succeeds; EN7528 procfs transport, baseline-only framing and DZS/H660GM-A MIB. | Retain as a reference. Do not import its successful no-op responses or unsolicited GEM setup. See the [PR review](XGSPON-PR24577.q1000k.md). |
| Generic `net/xpon` and `net/xpon/omci` at [2e2cf91](https://github.com/Sirherobrine23/airoha_kernel/tree/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon) | Both external modules compile and pass modpost on Linux 6.18.44/GCC 14.4.0. Baseline/extended wire codec, managed entities, service reconciliation, identity, sysfs and netlink are present. Only module-description warnings appeared. | More complete transport-independent foundation to adapt after the hardware interface is defined; not imported or selected as a Q1000K service. |

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

## Validation boundary and remaining plan

Completed local checks: factory/backend/LuCI host tests, controller transport
and every-transfer fault injection, read-only controller status tests, controller
APK build, BSP/PHY/MAC modpost, complete vendor APK generation, resource/hook
fixtures, validated MAC/FSAN handoff, unsupported-control error propagation,
AES/CMAC known-answer and fault-injection tests, native packet ownership and
DMA/RX fixtures, and real Linux attachment/RCU tests in UML. The normal builder and
protected source branches remain unchanged.

Outstanding software includes complete analog/SoC PHY sequencing, shared
resource ownership, native FE/QoS providers, per-channel retirement and GEM
transaction synchronization, identity handoff from the launcher, required flow
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
