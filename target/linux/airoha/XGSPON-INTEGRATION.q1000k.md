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

OEM `xpon@1fb64000` describes GPON at `1fb64000`, XG-PON at `1fb65000`
and EPON at `1fb66000`. The imported unbuilt `bsp/core/ecnt_xpon.c` exports
accessors for these resources, but assumes the OEM DT layout and IRQ indexing.
It needs bounded register access, probe/teardown lifetime, interrupt mapping and
a current binding. Absence from `/proc/iomem` alone is not enough to add a raw
mapping or to infer that a hardware block is clocked or initialized.

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
A generic unchecked `ioremap` wrapper would hide an ownership bug.

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
The [19-symbol list](XGSPON-STATUS.q1000k.md#kernel-audit) is only the linker
boundary, not the complete runtime dependency list.

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
APK build, BSP/PHY build and symbol resolution, MAC C compilation and a failing
modpost that names the remaining dependencies. The normal builder and protected
source branches remain unchanged.

Outstanding software includes complete analog/SoC PHY sequencing, shared
resource and QDMA adapters, factory identity delivery to the MAC, required flow
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
