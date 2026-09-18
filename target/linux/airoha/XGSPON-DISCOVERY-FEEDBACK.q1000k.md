# Q1000K discovery regression and passive TX feedback

## What this boot established

The 9a58806899 image received light and downstream frames. Startup power was
−18.66 dBm; connected captures ranged from −18.73 to −18.42 dBm. Profiles
passed authentication and XGS profile installation. No capture reached local
ONU-ID assignment, ranging or authenticated OMCI. This is earlier than the
previous boot's VLAN/key-ring failures.

| Experiment on boot 11fc5d35-5467-4eea-aca5-d88f214f9f8c | SN request / sent IRQs | Local assignment / ranging | Result |
|---|---:|---|---|
| Current MAC, strict VLAN policy | 29 / 29 | neither observed | completed functional negative |
| Current MAC, narrow VLAN policy | 41 / 41 | neither observed | completed; 40-response reset observed |
| Current MAC, combined policy | 4 / 4 | neither observed | interrupted to change experiment; cleanup verified |
| Current MAC, full profile replacement | 50 / 50 | neither observed | interrupted after reset/control evidence; cleanup verified |
| Previous bea6898ee9 MAC, live profile/classifier settings | 41 / 41 | neither observed | completed; cleanup verified |

The previous MAC control changed only the MAC module and a temporary launcher
path. Kernel configuration, vermagic, dependencies and every other module
matched. Installed image files were unchanged. The temporary files were
removed. Identity and input-archive digests also match the previous successful
OMCI session. That lowers the likelihood that the latest VLAN code caused this
boot's discovery failure. It does not exclude shared initialization or hardware
state, upstream optical failure, or OLT-side admission state.

GPIO38 was low, XGS enable GPIO8 was active, GPON enable GPIO9 inactive, and the
controller reported TX enabled without a protocol fault. Published bias and
modulation responded; sparse BEN samples were zero and TX power was the MCU
floor. These observations do not establish whether valid optical bursts reach
the OLT. Rejected other-rate profiles (`-EOPNOTSUPP`) coexist with successful
XGS profile installation and also occurred in the previous successful session.

Evidence: workspace `build-artifacts/q1000k-xgspon/discovery-feedback-research-20260917`,
including `bench-analysis.json`, module-control hashes and source manifest.
The two intentional cancellations preserved their raw logs and verified cleanup;
they are not completed-case passes. No physical fiber actions were requested.

## Focused primary-source comparison

Live web retrieval failed during this session. These findings use pinned local
primary-source copies, not a claim about the current upstream branch head.

- **Factory NAND:** `phy_10g.ko`, `phy_tx_ctl` at `0xd8d0`, uses LED42 to
  release/assert the GPIO38 gate. `en7572.ko` has a separate internal disable.
  `mpd_current` at `0x4aa4` changes `0x208[6:4]`, `0x130[13:8]` and
  `0x120[26]`, waits 50+5 ms, reads `0x33c`, and restores the controls. It is
  an active probe, unsuitable as an unqualified passive measurement of
  registration bursts. Its lookup key is uint16(256 − (raw >> 7)).
- **Factory MCU:** the previously audited TX routine reads `0x3a4`, publishes
  LE16 TSSI at `0xf0`, and uses BEN `0x488[0]` to select reporting. `0xfe=3`
  marks the BEN-off power floor. Hardware and mailbox reads have different
  update paths. Conversion age remains unknown.
- **Sirherobrine, 2e2cf91fe84467d77649efebd99a28284f2124b3:**
  `drivers/net/optical/core.c:530` releases the external disable after the
  provider enables TX; shutdown asserts it first. `en7572_ddmi.c` reads BE16
  MCU diagnostic words for bias and power. This supplies useful control and
  measurement semantics, not evidence of Q1000K XGS-PON interoperability.
- **8311 builder, 7d89440c7d9e1f209140910bb039f5d5a24dfbed:**
  `_8311-poninit.sh` applies serial/registration/OMCI identity through platform
  helpers. `8311-support.sh` collects `pontop`, OMCI MIB, VLAN and logs together.
  That supports collecting independent protocol stages. Its proprietary lower
  PON implementation does not provide an EN7573 electrical timing fix to port.
  Software version and Ethernet MAC fields used in OMCI cannot explain their
  own rejection before any ONU-ID assignment is observed.

## Next image and hypotheses

The next image preserves the VLAN normalization and RX-only GEM metadata fixes.
No new transmitter strength or timing value is inferred from this O2 failure.

| Hypothesis / question | Collection section | Evidence and limit |
|---|---|---|
| Normal-operation upstream feedback is absent despite enables | `output_status`, initial TX-off → normal registration → MAC-stop / +1s / +2s | Both gate readbacks; BEN; bracketed hardware TSSI; mailbox TSSI; `0xfe`; drive codes; DDMI bias/power. Internal feedback does not certify the connector waveform. |
| DDMI floor hides real feedback or MCU publication lags | Same passive records | Hardware/mailbox/status correlation, raw ADC and calibration endpoints, with timestamps. No ADC-ready claim. |
| Full profile replacement disrupts discovery | `activation-omci-discovery-full`, followed by repeat | Controlled full vs live/coalesced profile paths; all OMCI correctness fixes held constant. Runs only if authenticated OMCI was not reached. |
| OLT admission, upstream path or shared initialization differs | All three discovery attempts | Authenticated downstream profiles, SN request/sent IRQs, assignment, ranging and actual authenticated OMCI counted separately. Hardware/OLT-side acceptance remains unresolved without the next response. |
| Captured untagged VLAN row is rejected | Retained strict / narrow / OEM VLAN cases | Exact received row and matched response; only run after authenticated OMCI establishes the prerequisite. |
| Downstream-only GEM key policy unnecessarily retires OMCC | Retained combined / OEM / EqD / repeat | Hardware GEM readback, native epoch and subsequent OLT request. Not reached in this boot's initial collection. |

`output_status` is read-only, under the existing controller lock. It reuses the
30-field EN7573 sampler, brackets the bus sample with GPIO reads, and performs
no monitor selection, loop hold, generator enable, ADC trigger or gate change.
Values with invalid read masks remain unavailable. Monitor current is decoded
only if the required selection readbacks and OEM lookup range are valid.

Default `--suite discovery` does RX, a 180-second combined-policy probe, then
full-profile and repeated probes if needed. It proceeds to all six VLAN/key-ring
comparisons only after authenticated OMCI. `--suite vlan` directly selects the
existing comparison suite. The previous disconnected suites remain available
for a separate explicitly disconnected session.

Host validation covers the actual sysfs encoder's lock/error behavior, bus-read
ordering, no-write sampling, byte order, partial/sentinel data, monitor-selection
requirements, exact launcher parameters, cleanup after measurement failure, and
prerequisite decisions using authenticated requests rather than an O5 label.
Build results and image hashes are recorded in the release checkpoint.
