# Q1000K OMCI provisioning errors: source audit and fixes

Date: 2026-09-17. Branch: `q1000k-xgspon`.

## What the bench established

The `3071b72b61` bench fixed both Ethernet runt admission and the ordinary
allocation/session-revocation problem. Four corrected activation cases
processed 1,184 authenticated requests and queued 1,184 replies without a TX
session-authentication rejection. The old revocation control reproduced one
`-EKEYREJECTED` reply failure. These facts establish local authenticated OMCI
processing; queue consumption alone does not prove OLT reception.

Provisioning still failed. Retained request/response diagnostics show:

| Operation | Local OMCI response | Definite observation |
|---|---|---|
| Get OLT-G, class 131 | 5, unknown instance | The startup MIB lacks instance 0 in the bench's SFU configuration. |
| Create GEM port network CTP, class 268 | 1, processing error | The hardware callback or service reconciliation rejects creation. The old trace lacks the candidate fields and underlying errno. |
| Set Dot1X port extension, class 290 | 5, unknown instance | No instance exists for this class. The old trace does not identify the requested entity, mask, or enable value. |

Most corrected attempts reached authenticated O5 briefly, then received an
accepted OLT Deactivate_ONU-ID. The previous capture does **not** establish
which error caused the OLT to deactivate. The longer old-control O5 interval
had no installed service rules and is not successful service.

Evidence lives under `build-artifacts/q1000k-xgspon/`:

- `collection-omci-3071b72b61-20260917/collection.json`
- `collection-omci-3071b72b61-20260917-aux/bench-observations.json`
- `collection-omci-3071b72b61-20260917-aux/mib-observations.json`

## Primary sources and provenance

This audit uses previously downloaded, pinned sources and the actual NAND
rootfs. Attempts to refresh GitHub and search the standard failed with network
connection errors. The comparison is against these revisions, not a claim
about current upstream HEAD. No OEM program was executed.

- Sirherobrine kernel: `2e2cf91fe84467d77649efebd99a28284f2124b3`.
  Generic OMCI files are the unchanged imported source from
  `33c035249af6c47be8645147f1a52240de56ff5b`; the earlier research manifest
  records that recovery and the provider blob verification.
- 8311 WAS-110 builder: `7d89440c7d9e1f209140910bb039f5d5a24dfbed`.
- Factory rootfs SHA-256:
  `156e52d086d9ec9f7b9026ad7c97c9b4502909613b87586062422a8120b2acfb`.
- Factory `libapi_omci_adpt.so.1.0.0` SHA-256:
  `53a25806e20589316f16e9fd98d36e8bdfc8d0eb139abbafbbbcfe7836bda4e3`.

Local reproducible extraction, decoded tables, disassembly excerpts, source
copies and hashes are in `omci-provisioning-research-20260917/`. The ELF reader
checks the exact binary hash and reads symbols/relocations without loading it.

## OLT-G: initialize the instance and use attribute offsets

[8311's default startup MIB](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/mibs/prx300_1U.ini#L40)
creates class 131, instance 0. Vendor, equipment and version are blank padded
strings; the 14-byte time-of-day field is zero. All four cached 8311 MIB
variants do this.

The NAND adapter's `omci_attr_olt_g_op` at `0xcd650` defines lengths 4, 20, 14
and 14, all read/write. It names separate vendor/equipment/version setters.
Our imported agent had only a Nokia HGU-specific seed and no class-131
attribute descriptor. Its generic Set fallback also copied each partial Set
at offset zero, so it could overwrite an earlier attribute.

Changes:

- Create blank OLT-G instance 0 in the common startup MIB and restore it on MIB
  Reset, including SFU mode.
- Define the four offsets and lengths so partial Get/Set handles the correct
  field and preserves other fields.
- Remove the HGU profile's invented ALCL/ISAM identity. OLT-provided identity
  still drives profile detection through the existing parser.
- This is the OMCI record; it does not add a physical time-of-day clock service.

## GEM lifecycle: creation is separate from an upstream service

[Sirherobrine's provider](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c#L1721)
programs the GEM at `airoha_gpon_omci_hw_set_gem_port()` without requiring an
allocated T-CONT. Its later service replacement resolves the T-CONT. That
provider explicitly rejects downstream-only and multicast service replacement;
it is not evidence of complete multicast support.

Factory evidence:

- `omci_me_gem_ctp_create_op` at `0x45580` calls the GEM Set path at `0x45430`.
- That Set path calls general attribute handling and invokes
  `omciAddQueueMappingRule` at `0x44600` for its relevant mask bits.
- At `0x44670` the factory loads direction; at `0x44674` it compares with 2 and
  branches to `0x44a5c`. This path sets downstream flags and uses Alloc-ID
  `0xffff`, bypassing the upstream T-CONT lookup. The normal upstream branch
  separately looks up the T-CONT and queue information.
- This is static control-flow evidence. It is not a trace of our exact OLT
  request or proof of every factory service path.

Our provider previously rejected every GEM with an invalid T-CONT pointer and
then rejected an unallocated T-CONT with `-ENODATA`, including direction 2.
That restriction conflicts with the above separation.

Changes:

- Direction 2 ignores upstream T-CONT/queue/descriptor fields. Downstream QoS
  and supported encryption policy remain validated.
- A valid upstream GEM can be created while its T-CONT has Alloc-ID `0xffff`.
  It receives an actual hardware GEM entry with an unusable channel sentinel.
- The table validator accepts `0xffff` only with the unknown-channel sentinel.
  It cannot accidentally select the first unused hardware channel.
- Later OMCI allocation and PLOAM channel assignment bind the upstream GEM.
  T-CONT deallocation preserves the GEM but removes its usable binding.
- Native packet guards and the complete service graph still control traffic.
  An unbound GEM does not open an upstream queue or pass subscriber packets.

Limit: this fixes GEM object admission, not the unimplemented multicast
forwarding/broadcast-key path. Broadcast key ring 2 and unsupported downstream
QoS still fail explicitly. Whether those are requested by this OLT will now be
visible rather than hidden behind result 1.

## Dot1X: implement the disabled control without inventing an authenticator

The NAND descriptor `omci_attr_dot1x_port_extension_package_op` at `0xd1f88`
defines the 12 attributes with separate read/write access. Its enable setter
at `0x50b10` rejects values above 1; its action setter at `0x50bc0` checks
1 through 3. The visible enable/action functions validate and log, then return;
no authentication state machine is established by those functions alone.
This does not establish how the full factory system creates or uses instances.

The cached Sirherobrine OMCI core contains a class-name entry for 290 but no
specific descriptor or startup instance. 8311's checked-in MIB files do not
seed it explicitly; the builder wraps a vendor OMCI runtime, whose internal
Dot1X behavior cannot be inferred from that absence.

Changes:

- Create a class-290 instance alongside each physical Ethernet UNI.
- Support Get/Set of enable=0, the transparent/disabled configuration.
- Reject enable=1 as unsupported, other enable values as invalid, and
  unimplemented state/action attributes. Failed writes preserve enable=0.
- Do not create instances for nonexistent ports or report fabricated
  authenticator states.

This resolves a disabled-control request to a real UNI. It is not a complete
802.1X implementation. The next capture must establish the requested mask and
value before concluding that it resolves the observed class-290 transaction.

## Diagnostics and next bench

All OMCI completion records and the following extra records use the independent
critical retention stream. Sequence gaps remain explicit. Only public numeric
protocol/configuration metadata is added; no serial, identity payload, MIC or
key bytes are logged.

| Event | Fields |
|---|---|
| 22, OMCI completion | `id=opcode`, `result=reply transport errno`, `a=class`; `b` low bits retain previous flags, bit 5 marks Dot1X control present, bit 6 marks new metadata, high 16 bits are TCI; `c=OMCI result` when bit 4 is set; `d=entity<<16 | attribute_mask`. Create has no mask and uses zero. |
| 28/id 1, GEM candidate | `result=provider errno`; `a=entity<<16 | port`; `b=TCONT<<16 | direction<<8 | key_ring`; `c=traffic_management<<16 | alloc_id`; `d` bit 0 is valid/create-or-set, bit 1 says allocation was sampled under executor ownership. A clear bit 1 is unknown/inapplicable, not measured unallocated. |
| 28/id 2, GEM QoS | Same entity/port/error; `b=upstream_queue<<16 | upstream_descriptor`; `c=downstream_queue<<16 | downstream_descriptor`; `d=valid`. Kept separately if one record is lost. |
| 29, operation | `id=first failing stage` (1 validation, 2 hardware, 3 reconciliation, 4 MIB storage); `result=underlying errno`; `a=class<<16 | entity`; `b=TCI<<16 | mask`; `c=opcode`; `d` bit 8 marks a public Dot1X enable value in its low byte. Stage 0 can accompany a successful Dot1X observation. |

The collector decodes these into `omci_experiment.provisioning`, includes a
managed-entity response table in `omci-summary.md`, and treats pre-fix traces'
missing entity/mask as unknown. Duplicate polling snapshots are deduplicated;
actual duplicate OMCI replies remain counted and labeled.

Use the same image with connected fiber throughout:

| Collection case | What it tests/collects |
|---|---|
| `rx-startup` | Optical power, LOS, synchronization and safe startup/cleanup. |
| `activation-omci-fixed` | Startup Get OLT-G; actual GEM directions, allocations, QoS/key rings; Dot1X requested entity/mask/value; provisioning progress after these fixes. |
| `activation-omci-oem` | Same fixes with the existing NAND direct EqD ranging comparison. Distinguishes provisioning rejection from a ranging-mode difference. |
| `activation-omci-repeat` | Longer repeat; determine whether errors recur, whether OLT deactivation stops, and whether service/WAN/traffic prerequisites are reached. |

Select all four with `--suite omci --cases rx-startup,activation-omci-fixed,activation-omci-oem,activation-omci-repeat`.
The earlier min48 and allocation-revocation controls remain selectable in the
same image; they already reproduced their intended behaviors in the last bench.
No physical disconnect/reconnect is required by this matrix.

## Validation

- Host C tests execute production service/table code with modeled MMIO and
  UBSan: downstream ignored pointers, unallocated Create, OMCI then PLOAM
  binding, deallocation/rebinding, packet/queue containment, invalid table
  rejection, unsupported QoS/key ring, deletion and numeric diagnostic fields.
- Full OMCI core runs in a disposable UML Linux guest with lock/RCU checks:
  blank OLT-G Get, partial Set preservation, Reset, strict Dot1X control,
  nonexistent instances, actual authenticated Create failure/reply, duplicate
  suppression and separate transport/provider diagnostics.
- Collector tests cover new metadata, old unknown fields, failure-stage
  decoding and polling deduplication.
- Image build/inspection and complete package tests are recorded in the release
  artifact. None substitutes for the next physical provisioning test.
