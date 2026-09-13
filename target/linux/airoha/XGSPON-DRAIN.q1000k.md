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
