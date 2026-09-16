# Q1000K activation and reconnect results — 2026-09-16

**Follow-up analysis:** the [next consolidated bench plan](XGSPON-NEXT-BENCH-PLAN.q1000k.md)
compares all seven captures. The OCP control change also occurs in healthy
sessions before any outage; it is not specific to reconnect failure. The
physical test used passive RX mode with normal recovery callbacks suppressed,
so it does not establish whether normal activation-mode reconnect works.
Activation published profile-derived keys as valid in 176/180 samples,
supporting capture of successful profile processing and hardware discovery
responses alongside rejected messages. Raw results below remain unchanged.

## Conclusions

1. A separately supplied registration ID is not required by the 8311 AT&T BGW320 recipe. The previous prerequisite was our host/launcher policy. The collector now supplies 36 zero bytes when the field is empty or omitted, matching 8311's default, while preserving an explicit optional override.
2. Fresh RX startup is reproducible on the consolidated image. Three 30-sample startup/reload captures and a 180-sample soak all retained synchronization with TX inhibited.
3. Normal TX activation did not reach O5. The 180-sample capture retained frame synchronization but stayed in O2/discovery with a brief O1 transition, no assigned ONU ID, no operational OMCI service and no WAN testing.
4. Passive fiber reconnect failed. Light returned, but frames did not resume. A complete normal stack restart on the same RAM boot immediately restored reception and passed another 30-sample capture.
5. Every session cleaned up successfully. The shutdown fault did not reproduce in these runs. This is not a test of teardown under provisioned subscriber traffic.

## Registration ID correction and identity provenance

The [8311 BGW320 guide](https://pon.wiki/guides/masquerade-as-the-att-inc-bgw320-500-505-with-the-was-110/) specifies serial, equipment and hardware/software presentation, circuit-pack synchronization and VLAN normalization, without a separate registration ID.

The [8311 implementation](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/master/files/common/lib/8311.sh#L170-L172) appends zeros and takes 36 bytes. When neither registration setting is supplied, this produces an all-zero field. The Q1000K driver's mandatory 36-byte module argument is a wire-format/API requirement; it does not establish an ISP-specific credential requirement.

The active capture used the private subscriber ONT serial and WAN MAC from the existing workspace inputs. It used the guide's BGW320-500 baseline: equipment `iONT320500X`, hardware `BGW320-500_2.1`, software A/B `BGW320_4.27.7`, circuit-pack synchronization enabled, `native-pptp`, and VLAN normalization enabled. These version strings are test settings from the guide, not versions newly read from the subscriber gateway. No subscriber identifiers are included here.

## Firmware and collector provenance

- Firmware revision: `280555a076afc30f0292b0f3d256d18521f29299`.
- Image SHA256: `da0a024e949e5c58350c3cc88827d7e89673c2d93dda1260c374ad4fad515305`.
- Host registration correction: `75449da75d70715d907cc2c23804b3bf903b676c`.
- Host physical-only selection: `78243c66c8157c44f2aa7193186b56b0b3005aaa`.
- Connected/activation collector SHA256: `3d254d51d1f247f0ea46eeeea9f9d3b1c7fde5e0f66adf0bdb43086313d6fbfb`.
- Current portable collector v2 SHA256: `856cb4dbe4cddc428b49ffa8797f881b9fdcfbf4df8ac428bf9c2a4ae611096a`.
- Same verified 513-byte unit DSD and OEM MCU PM/DM pair used in all sessions.
- All 15 pinned device-side files matched the release before/after sessions. No firmware rebuild, runtime module substitution, NAND write or bootloader change was performed.
- Serial captures are the appended intervals from `/tmp/serial_output.log`.

## Results table

| Capture | RX samples | Synchronized samples | Stage result | Cleanup |
|---|---:|---:|---|---|
| `rx-startup` | 30 | 30 | passed | passed |
| `rx-repeat-1` | 30 | 30 | passed | passed |
| `rx-repeat-2` | 30 | 30 | passed | passed |
| `rx-soak` | 180 | 180 | passed | passed |
| `activation` | 180 | 180 | failed | passed |
| `rx-reconnect` | 300 | 35 | failed | passed |
| `startup-after-reconnect` | 30 | 30 | passed | passed |

### Connected receiver and activation

Initial RX power was **-18.54 dBm**, with optical-controller LOS and PHY LOS both clear. Across the initial connected tests it stayed approximately -18.60 to -18.27 dBm. The 180-sample RX soak spans 208.982 seconds between first and last observations; its frame counter reached 1,672,346. Sampled LOF and uncorrectable FEC counters stayed zero in all four initial RX captures and the activation capture. Coherent receiver/diagnostic pair age was at most 2 ms.

The activation capture spans 215.540 seconds between first and last samples and its frame counter reached 1,724,819. Registration was enabled in all 180 samples, and the TX gate was enabled in 178 (disabled during two recovery samples). Gate state alone does not prove an upstream optical burst reached the OLT. Observed O-states were 1 and 2; ONU/GEM identifiers remained unassigned, authentication stayed false, and the service-rule count stayed zero. The 322 MIB objects are local initialized objects, not proof of OLT provisioning.

At the end, the PLOAM rejection counter was 651. The sampled last error was -95 (`EOPNOTSUPP`) in 178 observations, -22 (`EINVAL`) in one, and zero in the first. These are samples of the last-error field, not per-error message counts. `protocol_error` stayed zero. This points to PLOAM dispatch/validation as a priority, but does not prove every rejected message is relevant to this ONU or that these rejects caused the discovery failure.

The current parser can return `EOPNOTSUPP` for an unsupported message, missing handler, mismatched burst-profile line-rate or unsupported disable mode; downstream backend operations can also return errors. The legacy assignment-path `EOPNOTSUPP` in the source is inside a branch excluded by the Q1000K build, so it is not a live candidate. Status does not export the message ID or rejection stage, so selecting one as the confirmed cause would exceed the evidence. OMCI provisioning, DHCP, traffic and throughput remain untested because O5 was not reached.

### Physical control and fresh-start recovery

The host recorded both user confirmations, 36 qualifying dark samples and 0 synchronized samples after reconnect. The complete capture contains 300 observations. Both LOS indicators asserted and synchronization dropped during darkness. After light returned, both LOS indicators cleared and power returned near -18.3 dBm, but the frame counter stayed at 324,330. The bright, unsynchronized observation span was 241.735 seconds; 228.879 seconds were observed after the user's reconnect confirmation.

The controller's A2 `0x110` remained `0x01002d1f`, including the OEM startup bit, throughout the physical capture. Its `ocp_control` changed from `0x00000005` to `0x43000005`; PHY frequency/clock diagnostics also changed. Their causal significance is not established. LOF/FEC/HEC errors around deliberate loss of light should not be presented as clean connected-fiber error measurements.

After the failed passive capture completed and unloaded normally, a fresh controller/PHY/MAC initialization on the same boot restored sync immediately at **-18.27 dBm** and passed 30/30 synchronized samples with TX inhibited. This control reused the unchanged pinned collector, selecting only its existing 30-sample startup case; its small host wrapper is included in the evidence. It does not identify the minimum reset or initialization needed for reconnect recovery.

## Next tests to combine in the next bench update

1. Add nonsecret PLOAM telemetry: message ID, destination category, verification-versus-dispatch-versus-handler rejection stage, per-type accepted/rejected counts, decoded burst-profile rate/index/version, upstream serial-response/ranging counters and O-state transition reasons. Do not collect identities, registration payloads or key material. Preserve MIC verification.
2. Use those observations to distinguish irrelevant/unsupported broadcasts from a rejected required burst profile, missing upstream discovery response, assignment problem or ranging failure. Replay captured nonsecret protocol fixtures in parser tests before changing acceptance rules.
3. Implement and test one bounded receiver recovery action after confirmed LOS-clear plus persistent no-sync. Compare controller recovery (including the observed OCP state), PHY/CDR restart and normal full initialization independently; keep the working OEM startup bit and same calibration/firmware. A full restart is a proven control, not yet the minimal fix.
4. Re-run fresh startup, soak, dark/reconnect and activation in the same image. Retain registration's zero default unless line-specific evidence calls for an override. Proceed to OMCI, DHCP, MTU and traffic only once O5 and service rules are observed.

## Evidence

The bundle excludes private firmware/calibration/identity input payloads. Known subscriber serial/MAC representations were checked against the captured text. Native captures retain detailed diagnostics and status; the compact JSON summary is for navigation.

- [activation-hardware-20260916-bgw320-default](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/activation-hardware-20260916-bgw320-default/collection.json)
- [physical-hardware-20260916-bgw320-default](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/physical-hardware-20260916-bgw320-default/collection.json)
- [startup-after-reconnect-20260916](/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/startup-after-reconnect-20260916/collection.json)
