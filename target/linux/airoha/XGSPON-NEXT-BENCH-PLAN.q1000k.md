# Q1000K next consolidated bench: discovery and receiver recovery

Status: **implemented and built as `be8e34c5f6`; hardware discovery/recovery
tests completed with coverage limits**. See the
[2026-09-16 results](XGSPON-DISCOVERY-RESULTS-20260916.q1000k.md) for the two
PMA-only recoveries, incomplete normal-callback acceptance, and reset-free
activation timeout. The plan below preserves the pre-build rationale.
The current follow-up specification is [TX/discovery tests and reference
comparison](XGSPON-TX-DISCOVERY-PLAN.q1000k.md); its new tests are not yet built.
See [implemented test map](XGSPON-DISCOVERY-BENCH.q1000k.md) and the packaged capability manifest for supported controls and explicit measurement limits.
Prepared 2026-09-16 from image `280555a076`, its three hardware collections,
and the source at `ec3c22c811`. Develop on `q1000k-xgspon` and produce one RAM
bench image with one portable collector. Normal optical TX is already authorized.

## 1. What the previous session actually established

| Finding | Evidence | Consequence for this image |
|---|---|---|
| Fresh initialization works | 90/90 startup/reload samples, 180/180 soak samples and 30/30 samples after the reconnect failure synchronized with TX inhibited | Keep automatic OEM `0x110[8]`, exact unit DSD and the working OEM MCU pair fixed across comparisons. |
| Discovery does not finish | 180 synchronized activation samples, no assigned ONU ID, no O5; 651 counted PLOAM rejects | Instrument the complete discovery path, including successful messages, grants and hardware-generated replies. |
| Some profile processing probably works | `security_keys_valid=1` in 176/180 activation samples; source publishes it after successful profile-derived installation | Do not assume every burst profile is rejected. Export installed profile state and transaction completion. This is not authentication or O5 evidence. |
| O2 briefly returns to O1 | TX closes near 150.677 s and reopens near 153.008 s, while sampled RX remains synchronized | Record the reset caller/reason, SN-sent interrupt count, threshold and timer events. Do not infer a timer expiration from elapsed time alone. |
| Passive reconnect fails | 36 confirmed dark samples; restored light without frames for 241.735 s; full reinitialization recovers | Preserve the failed state long enough to test smaller recovery actions. Confirm the successful action independently. |
| Passive and normal recovery differ | RX bench branches in `qphy_poll_work`/`qphy_irq_thread` suppress vendor recovery/event dispatch; `rx_reacquire=false` | Test passive behavior, controlled RX-only recovery and normal activation-mode reconnect separately. Normal recovery is not yet proven broken. |
| OCP value is not failure-specific | All seven captures change `ocp_control` from `0x5` to `0x43000005` about 4.7 s after the first sample, including healthy captures; in the physical case it precedes darkness by about 36 s | Retain it in telemetry; demote it as a leading fault hypothesis. Do not add a speculative OCP-clear experiment. |
| Collection loses causal detail | One last PLOAM errno, one-second status, generic failure exit and cleanup; normal serial output contains many repeated zero PMA status prints | Add event history and reasoned counters, preserve evidence before teardown, and continue independent cases after a valid negative result. |

The fresh-start control is a complete controller/PHY/MAC reinitialization on
the same boot, not a power-cycle test. Registration stays at the 8311-compatible
36-zero-byte default, with the existing optional override. Keep subscriber
identity and the selected BGW320 presentation fixed during discovery comparisons.

References: [hardware results](XGSPON-ACTIVATION-RESULTS-20260916.q1000k.md),
[offline cross-capture comparison](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/next-bench-reanalysis-20260916.json),
[existing bounded recovery recipes](XGSPON-REPEAT-RX.q1000k.md),
[OEM receiver audit](XGSPON-DEEP-RX-AUDIT.q1000k.md).
Validate protocol interpretations against the applicable
[ITU-T G.9807.1](https://www.itu.int/rec/T-REC-G.9807.1/en) edition and amendments
before changing accepted wire formats. This plan does not claim such a change
has already been justified.

## 2. Ranked questions and discriminating measurements

| ID | Priority / question | Measurements that distinguish outcomes |
|---|---|---|
| H1 | High: does the required burst profile reach hardware correctly? | Authenticated received profile, decoded rate/index/version/length/FEC, queued and committed generation, PHY programming result, MAC validity/length readback, invalid-profile grant count. Distinguish irrelevant other-rate profiles from rejected required profiles. |
| H2 | High: do discovery opportunities produce upstream responses? | SN-request and SN-sent MAC interrupt counts, hardware upstream PLOAM count, auto-reply mode, active profile, response-time setting, TX gate and TX-error causes. A software enqueue count cannot represent a hardware-generated reply. |
| H3 | High: is a relevant Assign_ONU-ID/ranging message missing, ignored or rejected? | Counts before/after address and local-serial matching, MIC result, handler result, assignment transaction and ranging state. Export `matches_local_serial`, not serial bytes. |
| H4 | High: does a local reset discard useful discovery progress? | Caller/reason, old/new state, outstanding work, profile generation, SN-send count/threshold and IRQ/timer history before every reset. Compare the existing threshold with one bounded longer threshold only if that reset is observed. |
| H5 | High: is failed reconnect simply absent RX-only recovery, or a recovery sequence defect? | Compare passive, checked recovery-only and normal activation callback modes under the same physical controls. Log observed LOS edge, delivered event, chosen branch, action and result. |
| H6 | High: which receiver state needs restoration after LOS? | Pre-outage, dark, LOS-clear/no-sync and post-action PCS/PMA/CDR/clock/controller snapshots; independently compare checked PMA out/in, clock restoration, recalibration, PHY initialization, controller initialization and full stack initialization. |
| H7 | Medium: do event ordering, repeated profile installs or diagnostics disturb acquisition? | IRQ-to-worker latency, queue/coalescing counters, callback duration, requested/applied generation, reasons for RX/TX quiesce, identical-profile reinstalls and time TX is disabled. Matched quiet-versus-detailed capture. |
| H8 | Conditional: after O5, what blocks usable service? | OMCI message/result history, MIB changes, provisioning rule failures, GEM/TCONT state, key selection, DMA/packet counts, DHCP/PD, interface-bound traffic and cleanup under load. |

Existing `gpon_sw_resync()` resets after the SN-sent counter passes `max_cnt`
(currently 20), and is called from the SN-sent interrupt path. It is a concrete
candidate for the O1 transition, not a confirmed explanation. Capture it before
considering a policy change. The shipped image uses hardware O2/O3 and O4
responses (`0x5100[0]=0`); telemetry must cover that path.

## 3. Instrumentation included before the next image is built

### 3.1 Continuous counters and a bounded event history

Implement a versioned, read-only diagnostic interface in the driver, plus a
separate fixed-command bench control interface. Proposed names below are new
interfaces to implement, not commands available on the current bench.

- Allocate a bounded event ring before PHY/protocol startup. Initial target:
  4096 fixed records of at most 96 bytes, plus small counter tables. Report
  capacity, wrap/drop counts and oldest/newest sequence numbers.
- Identify every record with boot/session ID, hardware generation, case ID,
  monotonic timestamp, sequence, event type, caller/reason and result.
- Keep exact per-type/per-result counters even when repetitive ring records
  are coalesced. Preserve first occurrences, changes, resets and failures;
  drain to the host regularly so repeated broadcasts do not erase startup.
- Default to approximately one full snapshot per second. Add a lightweight
  cached/fast-status view at 5–10 Hz for short transition windows. Do not issue
  full I2C/controller/eye measurements at that rate.
- Record capture begin/end and age for each controller, PHY and MAC component.
  Keep the existing coherent PHY/diagnostic pair. Cross-owner components must
  have explicit age/generation checks, not an invented atomicity claim.
- Capture events in existing IRQ/worker paths before they acknowledge or clear
  status. A userspace poller must not consume a PLOAM FIFO, clear an interrupt
  or reset a hardware counter used by the normal driver.
- Preserve the first fault and a final cached snapshot before marking the
  driver inactive. Diagnostic reads must still work after a contained fault;
  current live-only snapshot availability is insufficient for postmortems.
- Replace repetitive zero-status console printing with counted events and
  occasional summaries. Compare quiet and detailed modes for timing effects.

Required evidence must not depend on dynamic probes. As a fallback for unknown
issues, consider KPROBES/KPROBE_EVENTS and the required tracing support in the
same image if the FIT/RAM budget and build checks permit. Leave tracing off by
default and validate its availability; current image lacks KPROBES/FTRACE.

### 3.2 PLOAM: record what succeeded as well as what failed

For each received message retain message ID, destination category, wire length,
sequence, state at entry, verification result, dispatch decision, handler result
and resulting queued/applied generation. Separate these outcomes:

1. Malformed/truncated FIFO input.
2. Not addressed to this ONU / serial belongs to another ONU.
3. Integrity failure or unavailable key.
4. Unsupported type or unsupported subtype/rate.
5. State/parameter rejection, missing handler, busy/retry.
6. Accepted, queued, installed, superseded or failed during installation.

For Burst_Profile, also export the nonsecret profile header bytes and decoded
rate/index/version/FEC, preamble/repeat/delimiter fields, requested versus applied
profile and hardware readback. Represent PON-tag changes with a session-local
generation/equality indicator; no raw tag is needed for this comparison.
Track repeated identical profiles and whether each unnecessarily quiesces TX
or rewrites the PHY. Compare C bitfield decoding against an explicit-byte
shadow decoder; the shadow decoder must not drive hardware.

For assignment, registration and ranging, capture local-match booleans,
assignment validity, requested/effective delay, transaction result, selected
integrity-bank index and reset reason. Keep authentication checks in place.
Sanitized wire-layout fixtures support offline decoder tests; cryptographic
verification uses dedicated known vectors, not redacted packets as valid MIC
fixtures. Test messages are never injected onto the subscriber fiber.

### 3.3 MAC/discovery/TX evidence

Instrument existing reads/handlers for:

- SN-request received, SN-response sent, ranging-request received and
  registration-response sent interrupts, separately from software PLOAM sends.
- Downstream FIFO arrival/drain/full/overflow and upstream queued/sent/overflow.
- BWmap/grant errors, invalid profile grants, late-start/overrun/other TX errors.
- Auto-reply configuration, response timing, hardware/software activation state,
  installed profile validity/version/length and local identity readback-match
  booleans. Never export identity/register key contents.
- Controller/PHY TX request, actual gate state and time gated off, including
  short interruptions inside profile refresh/rebuild transactions.
- Existing controller sensor values before/after TX. Add TX power/bias only
  where a verified read-only accessor exists; report unavailable otherwise.
  A gate bit or averaged sensor value is not proof of a correctly timed burst.

Candidate register inventory from the imported AN7581 header: `0x5100`
(auto-reply), `0x5104` (state), `0x5108` (response timing), `0x511c` and
`0x5120/0x5124` (profile status/length), `0x5300/0x5308` (FIFO status),
`0x5920` (invalid-profile grants), `0x5950/0x5954` (RX/TX PLOAM counters),
and `0x5984` (ACK counter). Audit read/clear/latch semantics and generation
ownership before exposing each. These are candidates, not permission for an
unrestricted register dump. Capture W1C/error status through its current owner.

### 3.4 Recovery/event evidence

Log optical-controller LOS, PHY LOS, PSync, PCS/SOF/FEC/HEC counters, raw
clock/CDR/FLL results, forced-state fields, calibration completion, controller
RX output, supply/temperature and existing MCU status. No raw frequency word
is converted into MHz or called CDR lock without a verified interpretation.

Record the complete event chain: source edge, ISR acknowledgement, worker
enqueue/dequeue, delivered LOS/ready event, recovery eligibility/skip reason,
chosen action, every checked phase boundary, result and first resumed frame.
Capture states before action and after 0.25, 1, 5, 15 and 30 seconds where
practical; immediate boundaries use cached/owned observations. User confirmation
times and observed optical edges remain separate timestamps.

## 4. Tests and controls packed into the same image

All IDs below are planned. The release manifest must distinguish implemented,
software-validated, selectable, run, skipped and unsupported cases.

| ID | Test / mode | Main hypothesis | Evidence or decision |
|---|---|---|---|
| B01 | Identity, MCU/DSD, build/runtime and idle preflight | Known inputs / reproducibility | Exact hashes, default-versus-override registration provenance, no private payload output. |
| B02 | Three ordinary RX startup/reload controls | Preserve the working fix | 30 synchronized advancing-frame samples per run and independent cleanup. |
| B03 | Connected RX soak | Steady-state optical/diagnostic health | 180 samples, error deltas, valid pair ages, event history. |
| A01 | Normal activation with default reset policy | H1–H4 | 240 s wall-clock window, or advance after stable O5; retain accepted/rejected profiles, grants and responses. |
| A02 | Matched activation with reduced diagnostic overhead | H7 | Same identity/mode/settings; counters remain active, detailed event sampling reduced. Compare discovery milestones, rates and latencies. |
| A03 | Conditional longer SN-send reset threshold | H4 | Only when A01 proves that reset path fired: compare fixed allowed thresholds 20 and 40 with the same bounded observation window. Keep LOS, emergency, fault and ranging protections. |
| A04 | Shadow profile decoder and repeated-install accounting | H1/H7 | Run observationally in A01/A02. Identify byte-layout disagreement and unnecessary identical-profile reprogramming without changing wire acceptance. |
| R00 | Passive RX disconnect/reconnect baseline | H5/H6 | Connected baseline, at least 15 confirmed dark samples, then 60 s of bright observation with no intervention. Retain the failed state. |
| R01 | Checked PMA out/in recovery | H6 | Existing bounded reference recipe; controller remains initialized, TX inhibited. |
| R02 | PMA out/in plus known PLL restoration | H6 | Existing checked clock recipe; compare against R01 from a fresh reproduced failure for an independent result. |
| R03 | OEM clock/digital-reset ordering | H6 | Existing audited OEM recipe, including its verified masks/delays; capture every boundary. |
| R04 | Fresh receiver calibration | H6 | Existing supported calibration/gain-order recipe; report calibration result and recovered frames separately. |
| R05 | Full PHY initialization, controller retained | H6 | Reinitialize through an owned/drained lifecycle, record actual reset scope; distinguish from R01–R04. |
| R06 | Controller initialization, PHY settings retained | H6 | Quiesce PHY/MAC users, reload the same MCU/DSD through the controller owner, restore approved output state, then resume. Requires an implemented lease-safe API; never echo initialize behind live users. |
| R07 | Full stack reinitialization | Positive recovery control | Known successful control; capture differences against narrower actions, 30 healthy samples. |
| R08 | Independently repeat the successful recovery | H6 / causality | Fresh normal baseline, new physical outage, winning action alone, 30 healthy samples; repeat twice when practical. |
| R09 | Normal activation-mode disconnect/reconnect | H5 | Exercise normal callback dispatch with TX policy visible. Can run in O2; if O5 was reached, also verify new ranging/authentication/service after reconnect. |
| R10 | Initialization in darkness, then connect | Initialization versus reacquisition | Capture absence of light during initialization and all skipped/failed calibration branches; resume through a bounded supported path once light appears. |
| R11 | Short versus longer outage | H5/H6, conditional | Run if recovery depends on dark duration. Use actual LOS duration and recorded user actions, not a shell sleep as proof of disconnection. |
| D01 | O5 and ranging stability | H8 | Stable assigned ONU, applied ranging, authenticated state and no repeated resets. |
| D02 | OMCI provisioning and unsupported-operation capture | H8 | Per-class/opcode/result trace, MIB changes, exact rule-install rejection reason, GEM/TCONT configuration, operational status. |
| D03 | Data-key and forwarding evidence | H8 | Key-valid/index/epoch and encryption-enable state, direction-specific GEM/DMA/packet counters; distinguish keys installed from encrypted traffic actually attributed. |
| D04 | DHCPv4, DHCPv6/PD, routes, DNS and traffic | H8 | Bind traffic to PON; distinguish IPv6 address, delegated prefix and usable source/route. Capture DHCP negotiation failures. PD-only success is not an IPv6 failure by itself. |
| D05 | MTU and optional throughput | H8 | Interface-bound checks, HTTP result/bytes, MTU probes; interpret blocked ICMP separately. iperf only with a reachable supplied endpoint. |
| D06 | Loaded service soak and teardown | Regression / separate shutdown issue | Observe error/queue/rule/key counters under traffic, then preserve final evidence and verify owned teardown. |

### Recovery ordering and causal attribution

One reproduced failure may be used for a **discovery ladder** of R01, R02,
R03, R04, R05, R06 and finally R07. Stop applying actions as soon as sustained
frames return. Earlier actions may affect later ones: label that result
`recovered_after_sequence`, never an independent success for the last action.
Use R08 to repeat the candidate alone from a new failure. Include every recipe
in the image, but do not execute unrelated analog variations after recovery.

If R05 works independently, prioritize PHY state; if R06 works independently,
prioritize controller/output state; if only R07 works, investigate lifecycle
ordering and interaction. A narrow action that fails twice with valid captures
does not exclude every possible configuration of that subsystem.

Keep passive RX and recovery-only modes under TX inhibit. Normal activation
and its recovery use the already authorized grant-controlled TX path. Before
any state-changing recovery transaction, close TX and drain its owners, record
the failed state, then apply the selected scope. Policies are chosen at session
creation; tests do not silently turn an RX-only session into activation.

## 5. One collector that learns from failures

Replace the current linear stop-on-any-failure rule with a dependency-aware
case runner. Proposed CLI supports full suite, group/case selection and resume
from a host checkpoint. Resume must verify the exact running image, runtime
hashes, boot/session generation, private inputs and idle/owned device state.
Resuming after a reboot begins a new hardware generation; it cannot claim to
continue the same preserved failed receiver state.

Outcomes must be separate:

- **Valid negative result:** O5 timeout, no passive reacquisition, unsupported
  required OMCI operation. Capture a bounded aftermath and final history, then
  continue independent tests after cleanup. Mark dependent service tests blocked.
- **Evidence problem:** missing records, ring overflow, stale/mixed generation,
  physical confirmation absent or diagnostic timeout. Retain partial evidence;
  do not call the hypothesis false. Continue only after re-established preflight.
- **Containment/cleanup failure:** TX-policy mismatch, failed hardware access,
  stuck worker/unload or ownership loss. Stop writes to that session, save
  available cached/serial evidence and retain dependencies. Do not keep resetting
  or automatically reboot away the evidence.

On a functional failure, preserve the diagnostic state before resetting it:
capture a 30-second aftermath where useful, freeze/export relevant ring history,
save the failed snapshot, and record the chosen next action. Physical captures
must allow a recovery stage after the passive timeout without unloading first.

Use wall-clock deadlines plus minimum valid sample counts, not a sample count
mislabelled as seconds. Synchronization must persist with advancing frames;
LOS edges, counter wraps/resets and generation changes get explicit treatment.
No synthetic loopback counters can satisfy a live-fiber acceptance test.

Suggested default order: B01–B03, A01, independent RX physical/recovery group,
R09, then A02/A03 when eligible. If O5 is reached in any activation run, perform
D01–D06 immediately and preserve that success before later disruptive tests.
R10/R11 and independent alternative recovery recipes remain selectable in the
same image. Budget roughly 25–45 minutes for the main branch without extended
service/throughput testing, depending on recovery steps and user response;
record exact executed/skipped IDs rather than promising every branch ran.

Minimize manual work: one outage for passive plus discovery ladder, a new outage
for independent confirmation of the candidate, and one for normal-mode
recovery. Darkness-at-initialization or duration comparison needs additional
prompted controls only when selected. Hardware may not reproduce the failure
on every cycle; record `not_reproduced`, not a recovery success.

The single output bundle contains the case manifest, compact verdict/hypothesis
table, continuously drained events, full snapshots, counter deltas, serial
interval, physical confirmations, before/after interventions, source/build and
runtime hashes, and first failure per stage. Private input payloads remain
separate; diagnostics use local-match booleans and never export credential/key
material. Avoid repeated long zero arrays in the user-facing summary.

## 6. Implementation and release checks

| Area | Main locations / work |
|---|---|
| Protocol and MAC telemetry | `gpon/gpon.c`, `gpon/gpon_ploam.c` via preserved Q1000K patches; `q1000k_protocol.c`, `q1000k_omci_backend.c`, `q1000k_mac_cold.c` and runtime status. Instrument applied/compiled Q1000K branches, not unreachable vendor branches. |
| PHY recovery and counters | `xpon_phy_10g/src/q1000k_phy.c`, `q1000k_phy_pma.c`, `q1000k_phy_probe.c` and checked PHY API. Separate passive sampling, controlled recovery and normal dispatch. |
| Controller state | `q1000k-pon-control/src/driver.c` and `en7573.c`; owner-safe recovery, read-only sensor/state export, preserved MCU/DSD provenance. |
| Collection | `q1000k-xgspon-validation/files/validate` and `scripts/q1000k/activation-collect.py`; first-failure dump, case outcomes/dependencies, preservation of failed state, phase-specific controls and resume. |
| Artifact | Existing activation profile and separate XGS builder; one FIT plus embedded exact runtime manifest, collector, test-map and build/inspection metadata. |

Before building, check offline wire-byte decoding against the actual production
headers, local/broadcast/other-ONU decisions, malformed/MIC-failed messages,
accepted/rejected profile accounting, hardware-generated SN interrupt accounting,
reset thresholds and first-cause retention. Test ring wrap/drop handling and
snapshot reads after a fault. Do not substitute a hand-written struct with the
same intended layout for testing the actual wire decoder.

Exercise owned recovery start/stop/error unwinds, failed writes, absent fiber,
TX inhibit, concurrent snapshot/recovery, generation changes and interrupted
physical stages. Use the existing UML/lock-checking path for lifecycle/ordering
changes. Host runner tests must show that an O5 timeout still permits independent
RX tests, that missing evidence is not a negative finding, that a recovery ladder
cannot masquerade as an independent test, and that ownership/cleanup faults stop
dependent writes. Tests use modeled hardware; hardware acceptance remains separate.

Build once after required hooks and selected recovery scopes are present.
Inspect the actual embedded DT, modules/helpers, schema versions, counters,
supported action IDs, required userspace tools and FIT/RAM headroom. Generate
the portable collector from that exact artifact. Archive the test-capability
manifest; do not label a planned or stubbed hook as coverage.

## 7. Completion criteria for this bench design

A failed registration run should identify the furthest **observed** milestone:
profile received/installed → discovery request → hardware response → matching
assignment → ranging → O5 → OMCI rules → key/data path → IP/traffic. For each
missing transition, it must report whether the measurement is zero, unavailable
or invalid. Firmware-only evidence may still be unable to prove optical burst
quality or OLT rejection; state that remaining external-measurement boundary.

A failed reconnect should distinguish absent handler/action, failed attempted
action, unusable evidence and successful recovery. The artifact must retain
the pre-recovery state and say which isolated action was independently verified.
The useful deliverable is a narrowed cause or a validated recovery, even when
full subscriber service is not reached during this bench session.
