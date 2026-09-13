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
GDM2 nor the PON PCS/MAC is enabled; no vendor hook adapter consumes this API
in this checkpoint.

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
queue lock; an eventual adapter must implement deferred retry and prevent lost
wakeup races around upper-queue stopping.

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

**This is callback detachment, not a hardware drain or optical-off operation.**
Already-submitted DMA can complete after release, and a generation does not
flush descriptors received in hardware before a new attachment. Physical
TX/RX gating, outstanding descriptor draining, actual GDM padding/CRC behavior,
reset/clock coordination, FE flow programming and the vendor adapter remain
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
resource ownership, the vendor-to-native QDMA/FE adapter, identity handoff from the launcher, required flow
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
