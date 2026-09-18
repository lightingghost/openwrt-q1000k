# Q1000K: untagged VLAN and downstream key-ring continuity

## Bench evidence (2026-09-17)

Image `bea6898ee9b12ce6b24627ebd2e88cc01c0df70a`, boot
`e539a9c1-3862-4936-91cf-315d35a59ddf`. Evidence directory:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/collection-omci-classifier-bea6898ee9`.
Startup RX was -18.66 dBm, synchronized, with no LOS. Each retained case has
complete critical history (zero missing positions and no concurrent wrap).

| Case | Authenticated OMCI requests | Observation |
|---|---:|---|
| RX control | 0 | 30 synchronized samples, -18.70 to -18.39 dBm |
| Previous classifier retirement | 3064 | 8 nonempty full installs, no following authenticated request before reset/deactivation; 8 initial live installs did continue |
| Live classifier | 5122 | 130 classifier updates followed by another authenticated request; 13 repeatable untagged-row errors; 13 later key-ring Sets followed by no further authenticated request before reset/deactivation |
| OEM EqD comparison (partial) | 1570 | Same untagged failure 4 times, 3 key-ring boundaries; validator observed service_error=-22 and stopped with cleanup |

The 600-second repeat did not run after the containment stop. No stable
provisioned WAN or working traffic was established. Capture hashes and an
independent cleanup check are preserved. Modules unloaded, private staging and
lock removed, ponraw down and unenslaved; no panic/Oops/lock warning found in the
captured serial log. Collection has ended. No fiber outage was requested.

### Confirmed VLAN compiler defect

Class 171/entity 0x0101 Set mask 0x0400, exact public row:

`f8000000f03d5000000f0000000003d2`

This describes untagged input, add VID 122 / PCP 0 using the configured output
TPID, treatment mode 2 (copy inner DEI). There is no input DEI. Our strict
compiler returned -EINVAL. The agent replied result 1 (processing error),
with reconciliation-stage original error -22. All 17 errors in the live and
partial EqD cases are this same row. Other retained OMCI responses succeeded.

The public row is traced AFTER the response. Match stack, generation, TCI,
class, entity and mask. In the first live cycle, failing response sequence
10654/TCI 10910 matches row sequence 10656. The preceding double-tagged row
belongs to TCI 10909 and succeeded. The collector now performs this match.

### Independent leading continuity hypothesis

A subsequent class 268/entity 1023 Set mask 0x0040 selects key-ring 3
(downstream unicast only). The GEM was previously ring 0. Our provider kept
upstream encryption false but retired the complete data namespace because
`rx_encrypted` changed. In all 13 live cycles, a full retirement occurred,
native epochs changed, a success response was locally submitted/completed,
and no further authenticated request arrived before reset/deactivation.
Request-to-response time was 158.443 to 358.914 ms. First cycle: native epoch
9 -> 12, response after 168.325 ms, OLT deactivation another 72.147 ms later.

This is correlation, not proof that retirement alone caused deactivation:
the earlier VLAN rejection is a confounder. Separate next-image controls test
VLAN repair alone versus VLAN repair plus RX metadata continuity. A DMA
completion is not an OLT acknowledgement, and key flags are not proof of
successful encrypted user traffic.

## Primary source audit

Focused local source evidence and hashes:
`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/omci-classifier-research-20260917`.
Fresh network requests failed/timed out; these are pinned cached revisions,
not a claim about the latest remote head.

### Factory NAND adapter and driver

Extracted NAND rootfs SHA256:
`156e52d086d9ec9f7b9026ad7c97c9b4502909613b87586062422a8120b2acfb`.
`libapi_omci_adpt.so.1.0.0` SHA256:
`53a25806e20589316f16e9fd98d36e8bdfc8d0eb139abbafbbbcfe7836bda4e3`.

`setExtVlanTagRuleByPkt` (0x6f6e0) accepts the untagged mode-2 row. Then
`setExtVlanTagOpTblValue` 0x70440..0x70468, before the 0x704ac
`pon_add_vlan_rule` call, checks tag_num=0, nonliteral first treatment TPID,
and BBF247 disabled. It substitutes configured output TPID and DEI 0.
The equivalent second path is 0x70734..0x7075c before 0x70798. The older
bundled GPON adapter describes this as a ZTE OLT interoperability workaround.

The extracted factory `ponvlan.ko` treatment helper at 0xb0 itself rejects
copy-inner-DEI without 1 or 2 input tags (0x2e8..0x310); outer DEI copying
requires 2 tags. The adapter normalization prevents this rejection. This is
an OEM interoperability policy, not a claim that every absent-field copy in
G.988 should silently become zero.

In NAND `xpon_10g.ko`, `gwan_config_gemport_encrypt` (0x425b8) maintains
RX and TX flags separately. Ring 3 selects RX=1/TX=0, ring 0 selects both 0.
At 0x42918..0x42964 it updates one GEM entry using `gponDevSetGemInfo`, with
the TX flag as the hardware encryption argument. It does not retire all
T-CONTs or native DMA in this function. Our RX-only fix follows that narrow
scope while retaining command readback and the existing PLOAM key machinery.

### Sirherobrine

[Kernel repository](https://github.com/Sirherobrine23/airoha_kernel), reviewed
revision `2e2cf91fe84467d77649efebd99a28284f2124b3`.
The cached `provider.c` `airoha_gpon_omci_hw_replace_service` explicitly
rejects extended VLAN treatment; it is not a ready-made fix for this row.
Its `gpon_cb_set_gem_encryption` performs a targeted GEM update and exempts
GPON GEM 0 from encryption. This is useful evidence for narrow updates,
but its GPON encryption-bit behavior must not be copied into this XGS-PON
MAC. The NAND XGS-PON implementation establishes the RX/TX distinction here.

### 8311

[WAS-110 builder](https://github.com/djGrrr/8311-was-110-firmware-builder),
revision `7d89440c7d9e1f209140910bb039f5d5a24dfbed`.
`8311-extvlan-decode.sh` confirms the treatment mode meanings and no-tag
priority encoding. The cached `8311-xgspon-bypass` revision
`69f3c0e4b88505b168c89796386d12bfd705a30d` `8311-fix-vlans.sh` handles an
untagged Internet side by adding the detected unicast VLAN with PCP 0
upstream and removing the VLAN downstream. It does not hardcode our VID 122.
These scripts operate the data path; they do not disclose the closed vendor
OMCI daemon's authenticated reply/reconfiguration transaction. No unsupported
claim is made about its internal pending-response handling.

## Implementation and experiment mapping

One RAM activation image, no physical fiber actions. All active cases keep
exact OMCI admission/MIC, OEM Dot1X acceptance, inline keys and initial key
readback. The received MIB row remains unchanged. No calibration or subscriber
identity is embedded in the release.

| Collector case | VLAN policy | Live operation mask | Ranging mode | Limit | Hypothesis |
|---|---|---:|---:|---:|---|
| rx-startup | n/a | n/a | n/a | 30 samples | Optical reception remains healthy |
| activation-omci-vlan-control | 0, old strict | 15 | 1 | 120 s | Reproduce the same missing-DEI error |
| activation-omci-vlan-narrow | 1, absent untagged DEI = 0 | 15 | 1 | 180 s | Does repairing VLAN alone avoid deactivation? |
| activation-omci-vlan-combined | 1 | 31 | 1 | 300 s | Does preserving OMCC on RX-only ring changes allow continuation? |
| activation-omci-vlan-oem | 2, factory first-treatment normalization | 31 | 1 | 300 s | Is broader factory normalization necessary? |
| activation-omci-vlan-eqd | 1 | 31 | 3 | 300 s | Does OEM direct EqD change the outcome after both fixes? |
| activation-omci-vlan-repeat | 1 | 31 | 1 | 600 s | Reproducibility and longer provisioning/WAN observation |

The strict control stops immediately as a functional negative if -EINVAL is
observed; cleanup is still required before the next case. Any protocol fault,
hardware mismatch, other error or error in a repair case remains containment.
Do not treat a missing expected row as success.

Policy 1 only supplies missing DEI for untagged mode 2/3. PCP, VID and TPID
copies from absent tags remain invalid; single-tag outer copying stays
strict. Policy 2 models NAND's untagged, nonliteral first-treatment TPID/DEI
normalization with BBF247 disabled. Invalid/reserved modes remain invalid.

Bit 16 allows only isolated RX policy changes on existing data GEMs, with
unchanged GEM, allocation, channel, ANI, multicast role, all T-CONTs and TX
encryption=false. It verifies the unchanged hardware GEM tuple, publishes
metadata, and preserves OMCC/native epochs. Binding changes, upstream
cipher changes, combined edits and removals retain physical retirement.
Readback failure contains the session, with no fallback or false publication.

Diagnostics add compiler policy/errno, unchanged GEM hardware readback,
key-ring request-to-response timing, native epoch comparison, and the next
authenticated request before reset/deactivation. Header/public class-171 bytes
only; no key or identity payload is exported. Summary continuation lookup is
indexed to avoid quadratic processing of large histories.

## Validation

Regression tests parse all ten captured raw rows with the actual OMCI parser.
They reproduce strict rejection, check both fixes, tagged PCP/DEI preservation,
reverse mapping, reserved modes, missing PCP/VID copies and unchanged output
on rejection. Service tests exercise all policies through the actual provider.

Production transaction tests cover both RX transitions, unchanged queued
binding metadata, control mode, actual rebind/TX cipher changes, stale expected
state, hardware mismatch, read timeout and asynchronous fault containment.
Collector tests check exact response correlation and independent epoch/OLT
continuation evidence. The real validation shell is exercised with all six
parameter selections, the strict negative control and retained fault cleanup.

Real Linux UML skb tests passed for the captured untagged treatment, cloned
and nonlinear packets, all split points/headroom, upstream and reverse edits.
Full image build checks and final test counts are recorded in checkpoint.json
and release validation logs. New-image hardware validation remains pending.
