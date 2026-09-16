# Q1000K discovery/recovery bench test map

One RAM FIT and one portable Python/OpenSSH collector. Hardware outcomes remain untested until the new image is booted.

| ID | Test | Script selection | Hypothesis |
|---|---|---|---|
| B01 | Input, firmware, runtime and ownership checks | All sessions | H1-H8 |
| B02 | Three fresh RX starts | rx-startup,rx-repeat-1,rx-repeat-2 | H5-H6 |
| B03 | Connected-fiber RX soak | rx-soak | H5-H7 |
| A01 | Normal discovery, 240-second wall deadline | activation | H1-H4 |
| A02 | Matched discovery with fewer full snapshots | activation-quiet | H7 |
| A03 | SN reset threshold 20 versus 40 | activation-long-sn; conditional on observed threshold reset | H4 |
| A04 | Wire-byte shadow decoder and repeated profile accounting | All activation sessions | H1,H7 |
| R00 | Passive reconnect before any intervention | rx-reconnect | H5 |
| R01 | Checked PMA out/in | Recovery action 1 | H6 |
| R02 | PMA out/in plus known PLL restoration | Recovery action 2 | H6 |
| R03 | OEM clock cycle and full digital reset | Recovery action 3 | H6 |
| R04 | OEM gain plus fresh analog calibration | Recovery action 4 | H6 |
| R05 | Full PHY initialization with retained controller | Recovery action 5; drained MAC/DMA transaction | H6 |
| R06 | Controller reinitialization with retained PHY settings | Recovery action 6; drained MAC/DMA transaction, same verified MCU/DSD | H6 |
| R07 | Full module stack reinitialization | Recovery action 7 | H5-H6 |
| R08 | Winning action alone after a new outage | rx-confirm; conditional or explicit --recovery-action | H6 |
| R09 | Reconnect with normal registration callbacks | activation-reconnect | H5 |
| R10 | Initialize in darkness, then reconnect | rx-dark-start; opt in | H5-H6 |
| R11 | Short and long dark controls | rx-short-outage,rx-long-outage; opt in | H5-H6 |
| D01 | Stable O5/authentication | Automatically on any successful activation | H8 |
| D02 | OMCI opcode/class/result events, MIB and rules | Automatically after O5 | H8 |
| D03 | Key selection/epoch and directional GEM/OMCI counters | All activation snapshots; encrypted-traffic attribution unavailable | H8 |
| D04 | DHCPv4, DHCPv6 address/PD, routes and PON-bound traffic | Automatically after provisioning | H8 |
| D05 | 1500-byte MTU, HTTPS and optional iperf | Automatically after provisioning; iperf needs numeric server | H8 |
| D06 | Post-traffic soak and checked teardown | Automatically after traffic; all sessions check cleanup | H8 |

## Interpretation

The default run attempts connected RX controls, normal discovery, passive RX reconnect plus the recovery ladder, a fresh independent test of the winning action, normal-mode reconnect, reduced sampling and (only when observed) the longer SN reset threshold. R10/R11 are selectable extra physical controls. Service cases run immediately after O5/provisioning when their prerequisites exist. Each image contains every listed control; a single collection will not necessarily exercise every branch.

Actions 1–6 are fixed kernel controls, once per action per PHY lifetime, with TX inhibited. Action 7 fully unloads/reloads the known stack after exporting history. Actions 5/6 use the full MAC/DMA drain transaction, so a success includes that transaction; it cannot by itself attribute causality exclusively to PHY or controller initialization. A cumulative ladder win is labelled `recovered-after-sequence`. Only `rx-confirm` with one selected action establishes an independent recovery result.

Raw counters and event records are timestamped; PHY pairs are coherent. Controller and MAC captures are separate observations. Fast samples read three ordinary PHY MMIO words without I2C. These raw clock words do not establish measured frequency or clock lock.

The event ring stores 4096 records plus exact event/id counts and first occurrences. The collector drains and deduplicates it; missing sequence numbers make causal evidence inconclusive. Cached postmortem snapshots retain their original timestamps and are excluded from healthy/live sample counts.

Optical burst quality, OLT reception, measured TX bias/power and attribution of encrypted GEM traffic are unavailable. Key installation and successful traffic are reported separately. Dynamic kernel tracing is not a dependency and is not enabled in this image.

Private MCU/calibration and subscriber identity inputs are separate from the kit. The unit DSD, OEM PM/DM pair, automatic receiver output initialization and 36-zero-byte registration default remain fixed.

## Event fields and passive reads

`q1000k-pon-events` exports monotonic nanoseconds, an absolute ring sequence, hardware generation, event/id, result and four numeric data fields. The host adds the module-stack generation and associates the file with its case and boot ID. First occurrences survive ring replacement; event/id/error counts survive ring replacement until the hook module unloads. Counter snapshots finish after the header's event cutoff and are not an atomic cross-component snapshot.

- `ploam_verify`: ID is message type; fields are destination category (broadcast 1, profile-only broadcast 2, local 3, other 4), entry state, sequence, wire length.
- `ploam_dispatch`: same metadata; last field is stage (0 duplicate suppression before verification, 1 verification, 2 supported-type filter, 3 handler presence, 4 handler return). `-EALREADY` at stage 0 is the existing vendor duplicate filter, not an integrity rejection. No accepted wire formats or MIC requirements change.
- `profile`: IDs 0–3 carry header bytes 4/5/6/15, repeat count, state and sequence. A zero result means the actual bitfields matched the observational byte decoder. IDs 4–7 carry the delimiter's two words and preamble's two words, excluding PON tag/key/identity data.
- `profile_queue`: requested generation, equality with the installed profile, equality with the installed tag, pending mask. `profile_apply` IDs 0/1/2/3 mark begin/commit/failure/superseded. Commit fields contain generation, installed mask, key-valid flag and assigned ONU ID.
- `mac_irq`: ID is the asserted interrupt bit, captured before ACK. Bits 2/3/4/5 mean SN request/SN sent/ranging request/registration sent. `mac_error` IDs 0/1/2 retain FIFO+TX errors/FIFO drain outcome/RX+BWmap errors from the existing owner before clearing.
- `reset`: ID 0 is the backend reset request; 1 is the SN-send threshold caller (state, count, threshold, PHY reset); 2 is the late-TX caller. `job_queue`/`job_run` include the timer registration slot or PHY source/event and timestamps.
- `recovery`: action ID 1–6; first data field 0/1 marks before/after, then the consumed-action mask. Each action consumes its budget before hardware writes. The checked OEM recipe phase records carry sequence length and the cumulative checked-write count.
- `omci`: ID is opcode; fields are class, flags, optional response result, zero. Flags describe duplicate/unsupported/fake/operational-change/result-byte-valid. Attribute payload, transaction secrets and credentials are absent.

The MAC diagnostic register set is fixed: 5100/5104/5108 for reply mode/state/timing, 511c/5120/5124 for profile validity/version/length, 5920 for invalid-profile grants, 5950/5954 for PLOAM counts, 5958/595c for OMCI counts, 5960/5964 for GEM counts, and 5984 for ACK count. These use the existing driver's ordinary status/counter read paths. FIFO data, W1C status, key registers and subscriber identity words are excluded. Cold identity readback is exported only as a match boolean.

Lease-provided DNS addresses are captured. An active interface-bound DNS query, optical TX sensor and per-encrypted-GEM traffic attribution are unavailable and listed in the manifest. Failed ICMP and successful HTTPS remain separate outcomes; a DHCPv6 prefix without an interface address is recorded as prefix delegation, not silently counted as a usable interface address.
