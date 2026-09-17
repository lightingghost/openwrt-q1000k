# Q1000K upstream discovery: analysis and next bench specification

Prepared 2026-09-16 after normal internet recovery. Branch `q1000k-xgspon`.
Status: **source audit and test specification prepared; these new tests are
not yet implemented, built or run**. The running image remains `be8e34c5f6`.
Use one next RAM image and one portable collector. Preserve the working
receiver, exact unit MCU/DSD inputs, subscriber identity and 36-zero-byte
registration default. Normal optical TX is already authorized.

## 1. Where activation stops

The original fresh-start light/no-frame failure is resolved by the OEM
controller output initialization. Fresh RX is reliable with TX inhibited.
Passive reconnect requires the checked PMA out/in action; two independent
outages reproduced recovery. Normal callbacks recovered frames without that
manual action, but its acceptance window ended early on diagnostic EAGAIN.

The remaining activation failure is between **local discovery response and
observed ONU-ID assignment**. The reset-free fixed-40 run observed 36 requests
and 36 SN-sent interrupts with no assignment or O5. The extended run observed
94/94, two threshold resets and 5,089,516 advancing frames. Thus a threshold
reset is not the sole blocker, and RX lock alone is no longer the main question.
See [results and limitations](XGSPON-DISCOVERY-RESULTS-20260916.q1000k.md).

OLT means **Optical Line Terminal**: AT&T's provider-side PON equipment serving
the shared fiber. The Q1000K acts as the customer-side ONU/ONT. The MAC's
SN-sent interrupt is a local event, not an acknowledgement from that OLT.

```text
AT&T OLT -> discovery opportunity -> Q1000K MAC       observed
Q1000K MAC -> SN response event                      observed
MAC -> serializer -> burst enable -> laser -> fiber  not established end to end
AT&T OLT receives/accepts response -> ONU-ID          not observed
Ranging -> O5 -> OMCI/service                        prerequisites not met
```

The optical receiver can acquire downstream frames before upstream discovery
works; our TX-inhibited captures demonstrate that on this unit. AT&T need not
hear us first for that reception. Upstream transmission is required to finish
activation. The [XGS-PON specification](https://www.itu.int/rec/T-REC-G.9807.1/en)
is the protocol reference; state 2 in our driver combines O2/O3.

## 2. What the requested references contribute

All snapshots below were pinned during this analysis. A source implementation
or an author's success claim is not a Q1000K hardware result.

| Reference / revision | Concrete difference or lesson | Q1000K consequence |
|---|---|---|
| [Sirherobrine23 OpenWrt branch](https://github.com/openwrt/openwrt/compare/main...Sirherobrine23:openwrt:airoha_en7523), `1cf599c745d65a83f6571e1dfbc7f5aeca3198ee` | Patches restore complete EN7523 GPON burst timing, retry EN7571 transmitter rearm after timeout, and describe transceiver signal polarity. | Audit complete AN7581 TX timing/gates and recovery. GPON timing constants and another board's polarity are not Q1000K settings. |
| [Sirherobrine23 optical driver](https://github.com/Sirherobrine23/airoha_kernel/tree/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha), `2e2cf91fe84467d77649efebd99a28284f2124b3` | EN7572 reads separate bias, modulation, TX-power and BEN status; it also has host adaptive-loop tasks. | Add passive TX observations. Compare OEM loop startup/host assistance before considering a narrowly matched change. Generic calibration signatures and MCU sizes are not a substitute for this unit's OEM layout. |
| [OpenWrt PR 24577](https://github.com/openwrt/openwrt/pull/24577), `d7569c5e26551084e7643b0e83ecda9c31f49f11` | EN7571 experimental TX work distinguishes bias from modulation, tests monitor-photodiode on/off response, and investigates burst-envelope timing, BEN polarity and loop initialization. | A serial-number counter and even DC laser light can coexist with invalid bursts. Borrow the measurement strategy; do not copy EN7571 drive-current/GPON constants or treat all experimental comments as enabled code. |
| [8311 WAS-110 builder](https://github.com/djGrrr/8311-was-110-firmware-builder/tree/7d89440c7d9e1f209140910bb039f5d5a24dfbed), `7d89440c7d9e1f209140910bb039f5d5a24dfbed` | Starts from vendor firmware and configures its existing PON stack. Startup sets PON serial/mode/registration and OMCI identity; optical-module type determines TX-enable mode unless overridden. | Identity settings operate on an already working vendor MAC/PHY. They do not replace Q1000K laser/serializer initialization. Check our TX control semantics and hardware response template before chasing later OMCI/VLAN settings. |

Specific evidence:

- Sirherobrine's [EN7572 diagnostics](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_ddmi.c)
  reads big-endian diagnostic words; its
  [register definitions](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_regs.h)
  identify bias `0x64` (2 µA), TX power `0x66` (0.1 µW), modulation `0x6a`
  (2 µA), and separate gate/loop controls. BEN status is `0x488[0]`.
- The actual [8311 startup code](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/etc/init.d/_8311-poninit.sh)
  chooses TX-enable mode 3 for Potron/Fullvision and 0 otherwise, with an
  override. Those values belong to the WAS-110 optical stack, not EN7573.
  Its [configuration library](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/common/lib/8311.sh)
  pads an absent registration ID to 36 zero bytes. That matches our current
  default; the source does not establish that every AT&T line accepts it.
- The builder retains vendor components; its bypass submodule at
  `69f3c0e4b88505b168c89796386d12bfd705a30d` detects configuration and fixes
  service/VLAN mappings. It is not an alternative AN7581 optical driver.
- PR [PHY initialization source](https://github.com/AKoo7/openwrt/blob/d7569c5e26551084e7643b0e83ecda9c31f49f11/package/kernel/econet-xpon/src/phy/src/phy_init.c)
  contains many historical/experimental claims. Inspect effective parameter
  defaults and call paths. For example, the inspected code defaults
  `tx_closeloop`, `erc_openloop` and `tx_force` to zero despite nearby comments
  discussing experiments with those paths enabled.

Downloaded public sources and SHA256 provenance are retained under
`build-artifacts/q1000k-xgspon/post-bench-analysis-20260916/references/`.
The kernel recursive tree response is truncated; this review used the actual
downloaded optical/MAC/PHY files and selected OpenWrt patches, not an assumed
complete tree inventory.

## 3. Q1000K OEM comparison and concrete gaps

### 3.1 TX enable is currently only one verified controller bit

`en7573_set_tx()` writes and verifies `0x3e0[9]` (TX disable). This confirms
that request's readback. The current collector does not read laser-bias or
TX-power diagnostics, and does not measure a transmitted waveform.

Offline extraction of this unit's OEM `en7572.ko` confirms `bob_info()` reads
`0x64`, `0x6a` and `0x66`, byte-swaps the diagnostic words and prints current
and optical-power values. This corroborates the family driver's register
selection. It does not prove this MCU updates them at every discovery burst
or that the reported optical power is accurate at low duty cycle.

OEM provenance: firmware squashfs SHA256
`57954266ed93c02c0383f10c773da488f643d2e3e491d4dab7f423b0e4e9b6f3`;
`en7572.ko` SHA256
`86b8af889dab8f54b96ccaca0cb2b4973d5bafe28b9f5ecda0e8efed5a831a02`.
`bob_info()` is at symbol offset `0x3670`; the pertinent reads occur at
`0x3878`, `0x3934`, `0x39f0`. Original modules remain private and were not run.

Two misleading function names must not become background probes:

- OEM `ddmi_tx()` (`0x1ff0`) writes calibration mailboxes `0xb4..0xba` and
  toggles `0x3e0[9]` with a delay. It is not a passive power accessor.
- OEM `mpd_current()` (`0x4d10`) changes monitor selection/gain (`0x208`,
  `0x130`), triggers conversion through `0x120[26]`, waits and restores
  settings. It is not safe to treat as a free concurrent sensor read.

The family source also has an independent BEN gate at `0x100[3:2]`, loop
enable `0x208[0]`, force-current controls `0x210`, and live current words
`0x3c4/0x3c8`. Their raw values are useful next observations. Their meanings,
read side effects and applicability must be checked against the Q1000K OEM
before adding decoded assertions or mutations.

OEM `phy_10g.ko` (`caf1e56d0de40ae0faf9958d5155e7df8f1aa0d2f7dc54945b86a1df8a8f07aa`)
has `phy_tx_ctl()` at `0xd8d0` calling `ledTurnOff(42)` to enable and
`ledTurnOn(42)` to disable. Our replacement uses the controller bit directly.
The board's mapping/side effects behind OEM LED function 42 still need to be
traced; this is a concrete audit target, not proof of a missing GPIO write.
Our board-profile values already match OEM table 82 (`0x9`, `0x10001`,
`0x1010100`). Do not invert those simply because another board does so.

### 3.2 Identical profiles repeatedly interrupt TX

`q1000k_omci_burst_profile()` unconditionally queues an install after logging
that profile and tag are identical. The apply path invalidates profiles and
reinstalls them in a transaction that quiesces TX. The longer run completed
1,261 transactions and retained 18.763 seconds of TX-off time.

This is confirmed redundant work. Its responsibility for activation failure
is unproved: all 36 request and 36 sent IRQs in the gap-free reset-free case
follow a successful TX-enable event. Software IRQ time is not burst time.
Coalescing must preserve reinstall after reset, generation changes, changed
profile/tag, pending acknowledgements, failure and hardware invalidation.

### 3.3 Hardware response details are incompletely observed

The bench uses hardware O2/O3/O4 replies (`0x5100[0]=0`), XGS response time
`0x5108[13:0]=0x1600`, and exact serial-register readback. That is not a capture
of the transmitted Serial_Number_ONU message. The software reply routine is
a reference, not the executed path.

The imported AN7581 register definitions provide useful uncaptured fields:

| Register | Question | Collection rule |
|---|---|---|
| MAC `0x509c`, US_RATE_CAP | What upstream capability is the hardware advertising? | Read and compare with OEM and applicable XGS message definition. Imported DVT expects reset value 3; do not assume the live register has it. |
| MAC `0x510c`, `0x5128` | Random-delay configuration/current value and extended delay | Record generation and sampling time; these are not measured optical arrival times. |
| MAC `0x5944`, TX_BST_CNT | Does the MAC count a burst at its MPI/SOF output? | Correlate deltas with SN interrupts; still internal to the chip. |
| MAC `0x511c`, `0x5120`, `0x5124` | Valid/installed profile and lengths at response time | Capture through the owner at transitions, not by racing a transaction. |
| MAC `0x5960/64`, `0x5968/6c` | OMCI and XGEM counters if activation succeeds | Fix existing labels, replace reserved `0x5958/5c`. |

The software verifies/dispatches only Burst_Profile in these captures. Its
trace precedes local serial matching, so a bug solely in that match is a
weaker explanation. MAC filters/FIFO delivery or an upstream response that
the OLT cannot accept remain possible. One zero-depth FIFO read in the long
capture warrants better diagnostics, not a claim of corrupt assignment data.

## 4. Ranked hypotheses and tests in the single next image

Names in this table are **planned collector sections**, not current CLI cases.
Every record must say implemented/selectable/run/skipped/incomplete separately.

| ID / priority | Hypothesis | Planned collector section | Discriminating evidence |
|---|---|---|---|
| TX1 / High | The laser is not emitting, or drive/monitor initialization is incomplete despite TX-disable readback clearing. | `tx-sensors`: passive baseline with TX inhibited, then normal discovery; audit unit OEM drive/loop startup. | Valid bias/modulation/TX-power values and gate/loop state before/after activation. An external optical observation is the stronger emission test. Zero averaged readings during sparse bursts alone are inconclusive. |
| TX2 / High | Light exists but BEN polarity, envelope timing, serializer modulation or burst profile timing is wrong. | `burst-path`: collect MAC burst count, owned TX errors, profile timing, PHY TX controls, controller BEN/gates and OEM comparison. | MAC bursts without downstream physical evidence localize the gap. External burst waveform or OLT reception distinguishes adequate timing/modulation. An instantaneous low BEN sample does not prove absent short bursts. |
| TX3 / High | Hardware Serial_Number_ONU response contents, capability, key selection or timing differ from the expected XGS/OEM path. | `sn-template`: audited readback and synthetic offline encode/compare fixtures; conditional exact OEM correction. | Match booleans for private identity, public capability/timing fields, selected integrity-bank indices, reply mode, profile validity. Assignment after one justified correction supports it. Readback alone cannot prove on-fiber serialization. |
| TX4 / Medium-high | Reinstalling unchanged profiles disrupts discovery opportunities or repeatedly resets useful analog state. | `profile-baseline` versus `profile-coalesced`, same fresh-stack inputs and fixed reset threshold. | Removed redundant quiesces, unchanged required-profile/key handling, SN opportunities/responses and OLT assignment. If quiesces disappear and discovery still fails, it is an improvement but not the solution. |
| TX5 / Medium | Assignment is emitted by the OLT but lost in local MAC filtering, FIFO handling or dispatch. | `assignment-path`: exact per-message counts before filtering/verification and reasoned FIFO observations through existing owner. | Identify first missing boundary, local-match boolean, integrity/handler result and assignment commit. Do not let a diagnostic reader consume a FIFO or clear IRQs. |
| TX6 / Lower as sole cause | The SN threshold or a diagnostic read is the registration blocker. | Retain reset counters and first/last critical events; compare stable discovery windows and diagnostic retries. | Reset-free failure already weakens threshold-only explanation. Transient EAGAIN should reduce coverage, not falsely report a hardware fault. |
| TX7 / Conditional | Lower layers work; provider identity or later OMCI/service provisioning blocks progress. | `o5-service`: branch automatically after assignment/ranging/O5. | Observe milestones and actual OMCI/GEM/DHCP behavior. Keep identity fixed for TX comparisons. 8311 presentation changes cannot establish missing optical output. |

### Required collector and driver work

1. Add a controller-owned read-only TX snapshot, serialized with existing I2C
   users: raw BE16 bias/TX-power/modulation plus converted values, per-field
   validity/error and timestamp. Retain raw zero/sentinel values distinctly
   from I/O failure. Reading successfully does not prove MCU freshness.
   Start at 1 Hz and sparse phase boundaries; do not block IRQs on I2C.
2. Add an audited allowlist of TX control/status registers, including BEN,
   drive/loop controls and AN7581 TX timing. Capture before activation,
   immediately after profile installation and around reset/recovery. Record
   configuration versus live-state semantics; verify read/clear behavior.
3. Capture SN request/sent IRQs with current cached gate, profile generation,
   MAC burst-count sample/age and reset count. Retain critical reset, error,
   assignment and state events independently of repetitive IRQ/job records.
   Keep exact counters even when ordinary events are coalesced.
4. Implement a generation-aware unchanged-profile comparison. A no-op requires
   a completed matching hardware install, valid keys/tag, no conflicting
   pending update/ACK, no reset/fault and unchanged hardware generation.
   Changed content with the same index/version must still install. ACKs must
   retain required protocol behavior. Export a skip/force-install reason.
5. Retry only genuine diagnostic EAGAIN for a short bounded budget; log every
   unavailable interval and its cause, preserve hardware-fault stops, and do
   not count cached samples as fresh. Keep the working fractional-delay helper
   and cancellation transport. Separate SSH stderr from structured values.
6. Export machine-readable case decisions and a concise final coverage table.
   Keep per-phase time, opportunities and resets separate. Preserve captures
   before teardown; verify module unload, endpoint shutdown and private-stage
   deletion, with no NAND writes.

### Experiment order and physical effort

- Prepare all research, binaries, scripts and source comparisons before the
  user-declared slow-internet bench begins. Ship one image and one collector.
- Start with a short TX-inhibited baseline and normal activation. Run matching
  baseline/coalesced cases from fresh stack initialization with the same
  fixed-40 limit. Use up to 600 seconds per case or stop earlier for sustained
  O5; report actual opportunities and reset-free segments. A baseline,
  coalesced, baseline order checks drift without a physical reconnect.
- If a verified Q1000K OEM discrepancy justifies a second mutation, include
  its individually selectable bounded recipe in this image and compare only
  that difference. A candidate that is not fully audited at release remains
  observational/unsupported; no raw arbitrary-register interface.
- Reuse the already demonstrated passive reconnect controls. Do not repeat
  three manual outages. If normal reconnect acceptance still needs testing,
  use at most one outage after all connected cases, with the EAGAIN fix.
- External optical measurement is conditional on available equipment. A
  burst-capable inline PON meter/scope can observe normal upstream operation;
  a basic average power meter may miss discovery bursts. An isolated emission
  test may use the disconnected connector and a suitable meter/termination,
  but requires an audited bounded pattern/control and automatic TX-off restore
  before inclusion. No forced continuous/test-pattern emission on the live
  shared PON. The bench must not call this test completed without measurements.
- If starting with the fiber already disconnected and performing the isolated
  test, do all dark/emission observations before one connection for the
  discovery suite. Do not request another unplug unless it resolves a distinct
  question. Without a meter, label physical emission unverified and collect
  internal sensors; no repeated physical action substitutes for instrumentation.

### Verification before the image is called ready

- Unit checks for diagnostic endianness/units, legitimate zero versus invalid
  data, read failure, lifecycle ownership and cleanup.
- Meaningful profile tests: identical installed profile, changed payload under
  the same version, tag change, reset invalidation, pending ACK and failed
  install. No skip may preserve stale hardware after reset.
- Host tests for transient EAGAIN versus real faults, critical-event retention,
  counter wrap/reset generations, cancellation and planned/skipped cases.
- Compile affected packages and the complete FIT. Verify packed scripts,
  modules, manifest, checksums and RAM-image limits. No hardware result may be
  inferred from a successful build.

## 5. What this analysis does not establish

The exact root cause of absent O5 remains unresolved. There is no direct
measurement of Q1000K upstream optical emission, burst timing or OLT reception.
We have not run OEM firmware or obtained an OEM live-register baseline. Doing
so still requires the previously requested extracted RAM-boot/root-access
method; a NAND backup alone does not provide that experiment.

The strongest next step is to add the missing TX evidence and isolate the
confirmed repeated-profile behavior while preserving the working RX path.
This plan deliberately distinguishes tests we can instrument internally from
claims that require actual optical or OLT-side observation.
