# Q1000K OMCI QoS contract

Vendor r58 and core r12 carry the complete GEM CTP candidate across the
core/provider boundary, including the upstream queue/traffic-management
pointer, both traffic-descriptor pointers, downstream queue pointer and ONU
traffic-management option. The normalized service contains the same values,
so a locally seeded profile cannot bypass the checks applied to OLT Create/Set.

The implemented native policy uses 31 upstream T-CONTs, each with eight queues.
An explicit upstream priority-queue pointer must be in `0x8000..0x80f7`, belong
to the GEM's T-CONT and agree with its resolved service queue. Null pointers
`0` and `0xffff` retain the existing profile/mapper queue selection. SP and
WRR policy/weight replacement uses the physical drain and verified native
QDMA1 provider. Failed updates preserve installed software intent; uncertain
hardware changes remain contained by the existing transaction.

Rate-only traffic management, nonnull upstream/downstream traffic descriptors
and nonnull downstream queues currently return `-EOPNOTSUPP` before hardware
programming. Priority-and-rate mode is usable only without a rate descriptor.
The upstream descriptor is conservatively rejected even in priority-only mode.
Delete bypasses candidate QoS validation so unsupported attributes do not trap
an existing GEM. Advanced shaping, policing, backpressure and downstream queue
ownership remain unimplemented. These limitations must not be advertised as
successful QoS support.

The native driver's channel rate limiter cannot simply stand in for a GEM
traffic descriptor: multiple GEMs may share one T-CONT/channel. G.988 uses
upstream PIR/PBS shaping in rate-only mode, but CIR/CBS/PIR/PBS policing in
priority-and-rate mode. Implementing either requires its own explicit
resource and packet semantics, plus failure containment and testing.

Reference: [ITU-T G.988 (11/2022), section 9.2.3 and Appendix II](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.988-202211-I!!PDF-E&lang=e&type=items).
This is a local software contract, not evidence of optical throughput or OLT
interoperability. No device writes or firmware flashing were performed.

## Scheduler snapshots during retirement

The `d928eb20b6` disconnected-fiber RAM test completes native retirement but
fails at the first table-clear QoS snapshot with `-ESHUTDOWN`. FE retirement
sets every channel's retiring bit; namespace epoch replacement deliberately
clears those bits only after table clearing. Patch `9999m` allows an indirect
read in that interval only while CPU admission is paused and ready, all FE
channels are retired, RX is closed and drained, TX enables are zero and no
DMA is pending. RTNL pins the lifecycle during the command. Readback and
sticky-fault checks still apply; partial retirement and every scheduler write
remain rejected. No queue, service record or epoch is reopened by a snapshot.

The host fixture exercises all 32 channels and every missing drain condition.
The native Linux UML sequence now performs the actual pause/FE retire/RX
drain/snapshot/reset-epoch/QoS replay ordering; it previously tested QoS and
retirement separately. Hardware validation now passes on `30ea573aa3` in two disconnected-fiber
startup/shutdown cycles. All 32 schedulers are read; the unprovisioned cold
namespace replays channel zero. Loaded SP/WRR behavior on subscriber channels
remains unverified. This is a Linux lifecycle correction, not an http-uboot
issue.

## Full-byte weights and first service

The topology bench exposed an unjustified provider cap of 127: the OLT sets
weight 150. The NAND QDMA scheduler uses 16-bit command values, as does the
native driver. SP now stores all 0..255 OMCI values without hardware change;
WRR accepts 1..255 with exact readback. Native/global-unit checks remain.
The first data classifier can use unchanged bindings and closed data queues
without retiring OMCC; subsequent replacement retains the full transaction.
See [the service audit](XGSPON-OMCI-SERVICE-AUDIT.q1000k.md) for evidence, guards
and the selectable full-retirement control. Hardware validation is pending.
