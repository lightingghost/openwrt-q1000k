# Q1000K physical retirement evidence

This records static inspection of the cached QKX001-06.00.44.00 firmware.
The binaries were extracted locally, disassembled, and never executed.
No SSH, register reads, controller probes, resets, or firmware writes were
performed for this analysis.

The module file hashes are:

| OEM module | SHA256 |
| --- | --- |
| `fe_core.ko` | `8571f3969246a8524edc66924600e82cb80f0cde365e80de2bd73d91b5ea60e7` |
| `qdma_wan.ko` | `09bb59092940902608f14a42954f9e49286bf99aa75cd65b993c8913b4382a18` |
| `phy_10g.ko` | `caf1e56d0de40ae0faf9958d5155e7df8f1aa0d2f7dc54945b86a1df8a8f07aa` |
| `xpon_10g.ko` | `bd01941044584af4eee9fda21541f089f1f26d4d0ba3765b7986ae16dd304017` |

## FE and QDMA observations

Legacy `0xbfb5xxxx` addresses correspond to physical `0x1fb5xxxx`. They cross
several independently owned devices; a generic legacy FE mapper is invalid.
For GDM2, the relevant offsets within the native FE window are:

| Offset | Observed OEM use |
| --- | --- |
| `0x140c` | CDM2 hardware-forward channel bitmap |
| `0x151c` | GDM2 loopback configuration |
| `0x1520` | Channel release command/status |
| `0x1524` | TX channel enable bitmap |
| `0x1528` | RX channel enable bitmap |
| `0x1570` | Channel busy bitmap |
| `0x1574` | Additional whole-port retirement status |

`fe_api_set_channel_retire` at `.text+0x12ea0` checks the busy bit, temporarily
clears the target hardware-forward bit, and issues `(channel << 4) | 1` to
`0x1520`. It polls release bit 1 set and the channel busy bit clear, then clears
the release register. Its mode-1 branch additionally disables and restores the
entire TX/RX bitmaps. This routine does not test the QDMA per-queue status.

The newer `fe_api_set_channel_retire_one` at `.text+0x13720` calls the MBI
unlock helper and the local routine at `.text+0xf100`. It discards the latter's
return value and returns success. That behavior must not be retained.

The local routine at `0xf100` saves the TX/RX bitmaps, writes `BIT(channel)` to
both (temporarily isolating the entire port), and issues the release command.
Completion requires two consecutive observations with:

1. Release bit 1 set.
2. The target channel busy bit clear.
3. The channel's status byte at **QDMA1** offset `0x1280 + (channel & ~3)`
   equal to zero, shifted by `8 * (channel & 3)`.

It then clears the release register and restores TX/RX. The imported vendor
`QDMA_CSR_CHNL_QUEUE_EMPTY` and `qdmaIsChannelEmpty` macros corroborate the
register layout; the function name alone does not establish the polarity or
meaning of every bit. The OEM's completion test uses a zero byte.

`mbi_hang_unlock_by_terminate` at `.text+0x135d0` first polls release bit 19.
If stuck, it toggles bit 16 and polls again. It similarly pairs status bit 27
with control bit 24. These are recovery actions, not read-only diagnostics.
The all-channel retirement routine also contains an older loopback path that
touches legacy FE offset `0x5004`, which is physically within **QDMA0**. It
cannot be copied into a QDMA1-only PON implementation.

The scheduler read routine `qdmaGetTxQosScheduler` at `.text+0x17280` separately
confirms the native indirect scheduler layout: command at QDMA1 `0x1024`,
channel in bits 23:19, queue in bits 18:16, done in bit 30, and a 16-bit result.
Scheduler mode words start at `0x1040`, with eight channels per word. This
supports the new native QoS implementation but does not prove retirement.

## MAC and PHY boundary

The imported AN7581 `MBI_MPI_STOP` layout at XG-PON relative offset `0x5004`
contains MPI TX stop/done at bits 24/31, MPI RX at 16/30, MBI TX at 8/15, MBI
RX at 0/14, DBRu stop/done at 12/13, and DEL RX stop at bit 4. The imported
`gponDevTxMbiStop`, `gponDevMbiStop`, and MPI stop helpers poll completion but
return success even if polling expires. A missing value-only resource provider
also returns all ones, which must never be accepted as stop completion.

The alignment-FIFO helper reads `DBG_TX_ALIGN_FIFO_STS` (`0x5814`) and waits
for its used field to be zero. The software-resync sequence has this wait
commented out. Neither a commented wait nor an empty native DMA ring establishes
that the MAC/optical pipeline has drained.

A correct retirement transaction must coordinate native admission, DMA
reclamation, FE/QDMA release, MAC pipeline completion, and the subsequent
T-CONT/GEM invalidation or reuse. Whole-port TX/RX isolation and termination
recovery require exclusive lifecycle ownership and checked restoration.
The supplied evidence does not justify treating an isolated FE busy bit,
callback teardown, or a software table reset as that complete transaction.

## Checked MAC stop implementation

The MAC resource provider now owns writes to `MBI_MPI_STOP`. Legacy raw writes
to that mixed control/status register are rejected. The checked API preserves
the other documented controls, never replays completion bits, checks write
readback, and waits at most 3,000 one-microsecond intervals for all requested
acknowledgments. Missing resources, all-ones reads, ignored writes and timeout
return errors. Failure latches a provider fault that blocks release; further
hold requests can attempt containment but cannot clear the fault.

The alignment-FIFO wait requires MBI TX stopped and acknowledged, with MPI TX
still running so buffered data can leave. It checks the low 16-bit used field
and reports timeout or invalid reads. It does not assert complete optical
drain. These operations serialize under the resource lock and are usable by
the imported atomic-context callers; a timeout can hold IRQs off for up to
3 ms. The eventual process-context lifecycle should avoid invoking them in
routine packet processing.

Vendor patch 023 routes all five stop helpers and the alignment-FIFO wait to
the checked provider. MAC initialization now aborts and releases its crypto
state if the initial MPI stop fails, before publishing timers or tasklets.
The legacy whole-port FE helper remains unsupported because it ignores FE
errors and enables channels without ownership checks.

All 30 PON host tests pass, including MAC stop faults and startup unwind. The
Linux 6.18.44 AArch64 vendor package builds successfully as r20. No stop
command has been executed on the Q1000K.

## Native FE/QDMA retirement and pause

Kernel patch `9999e-net-airoha-pon-fe-retire.patch` adds a live-port CPU pause,
FE release, and resume. Pause invalidates every previous TX admission epoch,
waits for submitted native DMA to complete while its hardware queues can
still run, then closes all physical queues. Timeout keeps CPU admission
paused and supports retry. Resume requires a completed pause, retains
concurrent explicit queue closures, and cannot revive retired channels or
clear a control fault.

The FE stage requires the entire port paused and mappings reclaimed. Under
RTNL, it excludes IRQ-side controls, verifies the native TX bitmap and queue
closures, disables hardware forwarding, isolates the target TX/RX channel,
and temporarily opens its hardware queues while CPU admission stays closed.
It preserves the MBI age fields and requires two consecutive release-done,
busy-clear, empty-status observations. An existing release or MBI recovery
operation returns `-EBUSY`; speculative termination recovery is not attempted.

Cleanup closes the hardware queues and clears the release request before
restoring peer TX/RX/forwarding bits. The retired channel remains disabled.
Every restoration is checked. Failure attempts all-off containment and
latches a control fault; successful software bookkeeping cannot hide failed
MMIO. Native QDMA0, DSA and other FE port registers are never accessed by this
transaction. The packet adapter provides sleepable wrappers pinned by its
lifecycle mutex.

The 31 host tests pass, including every channel, every FE write failure,
completion instability, all timeout predicates, pause/resume register faults,
stale packets and epoch overflow. The native transport and adapter UML guests
pass with lockdep, RCU and atomic-sleep checks. The native guest exercises
pause timeout/retry with real waitqueues and asynchronous DMA completion,
then FE release and resume; the adapter guest verifies sleepable forwarding
and exact errors. Linux 6.18.44 and vendor r21 build successfully; the vendor
APK is 335,387 bytes.

This implements the FE/QDMA stage, not the complete per-GEM/T-CONT retirement
transaction. MAC FIFO, pending downstream RX, service-table invalidation,
and optical quiescence must be coordinated before identifiers can be reused.
The whole-port isolation may interrupt peer channels; it is not a lossless
per-channel operation. No retirement or pause command was run on hardware.

## Downstream FE/QDMA1 receive boundary

OEM `fe_api_set_channel_retire_all` at `.text+0x13948` selects GDM2 status
`0x1574`; after individual releases it waits for the entire word to become
zero at `0x13a30`. Its timeout only prints diagnostics. Kernel patch `9999g`
requires two consecutive zero observations of that word and the channel-busy
bitmap, with checked TX/RX/forwarding isolation. A separate bitmap records
successful physical FE releases; a software quarantine bit is insufficient.
All 32 channels must have completed release before this receive drain.

The new API first closes RX callback admission and waits for admitted callbacks
through `synchronize_net()`. It then verifies FE isolation and retirement,
disables only QDMA1 RX DMA and waits for idle. A timeout or failed readback
returns before descriptors or pages are touched. The helper requires QDMA1
with exactly one active user; it never changes QDMA0 or TX DMA controls.

With DMA idle it masks RX completion interrupts, drains IRQ handlers, disables
RX NAPI instances, and masks again to account for a racing NAPI completion.
It discards completed descriptors and any partial scattered packet, using
non-direct page-pool recycling outside NAPI. No partial or stale frame reaches
the consumer or GRO. Descriptor refill uses the existing native owner. NAPI
lifetime is balanced on every path; RX DMA and callback admission remain closed
for explicit service-namespace reactivation. Failures remain sticky.

The native and adapter UML tests pass with real RTNL/RCU and lifecycle locks.
The native fixture retires all 32 channels and checks receive admission; its
DMA hardware stage is mocked. Dedicated host tests execute the production
DMA/IRQ/NAPI orchestration with injected timeouts, dropped mask writes and
incomplete descriptor drain. The production RX assembly fixture verifies
partial-chain and whole-ring discard. All 39 PON host tests pass. Linux
6.18.44 and vendor r26 build successfully and the prepared sources match the
tested files. No device was accessed.

This completes the downstream FE/QDMA receive stage. Coordinated MAC ingress
stop, optical stop, service-table replacement and verified reactivation are
still required around it; an isolated call is not a complete PON shutdown.

## Generation replacement after a complete receive drain

Kernel patch `9999h` allows a new namespace only after all 32 physical FE
releases, completed RX DMA drain, closed queues, zero native TX mappings and
verified FE isolation. It checks every TX epoch and the RX generation for
64-bit overflow before changing any of them. Resetting the generation leaves
admission and DMA closed and never clears a hardware fault. The caller must
hold MAC ingress/egress stopped and remove the old service tables first.

After replacement tables and channels are verified, explicit RX activation
checks the channel mask, enables only RX DMA, verifies the FE RX bitmap and
publishes the new admission boundary with release ordering. Failed readback
attempts FE/RX-DMA containment and keeps a sticky fault. CPU TX remains paused
until an explicit resume; old retry packets and old RX generations stay stale.
Sleepable adapter wrappers pin the native attachment throughout each call.

All 40 PON host tests pass. The native UML guest covers complete retirement,
receive drain, generation replacement and reactivation, including stale TX/RX
rejection with real RTNL/RCU. Adapter UML verifies context and error forwarding.
Linux 6.18.44 and vendor r27 build successfully. These are local tests with
mocked hardware; no device access or physical acceptance was performed.

## Exclusive PON MAC reset

The MAC resource provider now acquires the exclusive `EN7581_XPON_MAC_RST`
line without changing its state during probe. The Q1000K reset adapter uses
that line instead of replaying the shared SCU reset word. It rejects IRQ/RCU
contexts, pins the provider with a lifecycle mutex and excludes register/stop
operations while reset-controller calls execute without an IRQ lock.
Assertion and release are verified. Failure attempts asserted containment and
leaves a sticky fault; a subsequent reset cannot clear it. Probe/remove use
the same lifecycle mutex, and the disabled board node declares the reset.

All 41 PON host tests pass, including reset errors and wrong status at each
assert/release stage, sticky-fault rejection and register-access exclusion.
The updated DT compiles, and vendor r28 builds for Linux 6.18.44/AArch64.
No hardware reset or other device operation was performed.

## Coordinated physical shutdown

Vendor r29 joins the stages in `q1000k_pipeline_shutdown`: pause CPU admission
and drain native mappings; stop MPI RX ingress; retire all 32 FE channels;
stop MBI TX and wait for the transmit alignment FIFO; stop MPI TX and MBI RX;
drain downstream FE/QDMA1; then stop PHY callbacks and disable the owned
EN7573 transmitter. No service record, identity or key is cleared
by this routine. It records the last completed stage and exact retired bitmap.

The outer MAC teardown closes readiness and unregisters control/event paths,
drains RCU and protocol timers/tasklets, and stops the daemon before invoking
this sequence. The native attachment and all protocol state remain alive until
physical shutdown returns. Only then does teardown detach native packet DMA
and free software state. Failed startup unwinds only resources it acquired.

A failure preserves the original error, independently attempts all native TX
channel disables, MAC stops and PHY quiescence, and records containment errors
separately. It invalidates the BSP MAC provider so a subsequent MAC load cannot
silently reuse a partially drained device. Repeated calls do not clear faults
or repeat a successful shutdown. Runtime service replacement and coordinated
restart still need to use the stopped namespace explicitly.

The new blocking PHY quiescence entry point rejects calls from its own callback
and waits for callbacks from an external lifecycle worker. UML tests pass for
both poll and IRQ callbacks held across shutdown, alongside 50 start/stop
cycles and the existing unload races. All 42 host tests pass, including every
physical-stage failure crossed with all 34 containment failures and outer
teardown fault paths. Linux 6.18.44/AArch64 vendor r29 builds successfully.
Hardware stages in tests are mocked; no device access was performed.

The r30 review closes two shutdown edge cases. T-CONT/GEM status `0xffffffff`
is now a sticky I/O failure, never a valid command-complete response. Tests
inject it into every allocation and replacement command position. A transmit
FIFO already empty with MPI TX stopped can establish emptiness without
releasing that stop; a stopped FIFO containing data returns `-EBUSY` rather
than reopening egress. All 42 host tests and the AArch64 vendor build pass.

## Port frame limits and scheduler units

Vendor r32 and kernel patch 9999i expose native GDM2 frame limits and QDMA1
weight units. A change requires paused CPU admission, all FE channels retired
(or a verified new epoch), drained RX, disabled/idle RX DMA, zero FE TX/RX and
hardware forwarding masks, and all 256 CPU queues closed. The expected/current
configuration comparison rejects stale updates. Register writes preserve
unrelated fields and verify readback; uncertainty poisons subsequent controls.
An identical configuration is a read-only success while running.

The imported global scheduler ioctl now uses this owner. Its historical `1B`
enumeration denotes the AN7581 64-byte setting; the 16-byte selector remains
explicit. Per-channel indirect commands reject an absent all-ones completion,
including channel 31/queue 7, without publishing output. No global ECNT FE/QDMA
provider is introduced. Remaining startup callers still need the coordinated
namespace reset and reactivation transaction.

All 44 PON host tests pass. Native, adapter and PHY Linux UML tests pass with
lockdep/RCU checks; the native kernel and r32 vendor packages build for AArch64.
The PHY shutdown now relies on actual controller TX disable and callback drain:
the imported XGS `FW_READY` command was a no-op and is explicitly unsupported.
No hardware was accessed for these checks.
