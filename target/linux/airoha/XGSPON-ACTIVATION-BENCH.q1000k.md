# Q1000K consolidated RX, activation and WAN validation bench

The user authorized one image containing the five next stages, including optical
TX when needed. Work stays on `q1000k-xgspon`. This image remains RAM-only,
with NAND disabled and management at 192.168.255.1.

## Implementation and acceptance plan

1. Integrate checked OEM controller `0x110[8]` after PHY/MAC hardware startup,
   before protocol activation/TX. Preserve the older receive-only experimental
   image's original comparison behavior. Validate initialization and unwind.
2. Export coherent read-only PHY status plus diagnostics under one callback
   lock, usable in receive-only and activation modes. Collect startup, repeated
   lifecycle, prompted dark/reconnect and sustained RX tests.
3. Build a separate activation-validation DT/profile. Controller loading defaults
   to a cached TX inhibit; an explicit immutable parameter in a new controller
   session permits TX only on this DT. Keep all boot services inactive. Collect
   normal protocol/O5 progress with user-provided private subscriber identity.
4. Collect OMCI status/MIB, service rules, assigned ONU/GEM state, protocol/key
   errors and hardware packet counters; distinguish operational registration
   from provisioned service and demonstrated encrypted traffic.
5. Include inactive DHCP/DHCPv6 WAN configuration, explicit WAN validation,
   bound-interface connectivity/MTU checks, throughput hooks and sustained
   traffic/recovery observations. Restore interfaces/configuration and stop
   only resources owned by the collection session.

One portable host collector will pin the image/runtime, stage verified OEM/DSD
inputs plus optional private subscriber configuration, run the stages, preserve
serial/status evidence and produce an archive. Missing identity permits RX work
but cannot count activation as tested. Physical controls require actual operator
confirmation; unattended omission is recorded as not run.

## Validation required before delivery

- Controller policy/startup and failure-injection tests.
- Pipeline ordering and coherent snapshot/lifecycle tests, including normal TX.
- Inert-device collector/helper tests for phase gates, errors, timeouts,
  credentials, interface ownership and cleanup.
- Existing PON/status tests and applicable Linux UML lifecycle/concurrency runs.
- Complete pinned build, FIT/DT/rootfs inspection, runtime checksums and portable
  collector verification. The artifact must retain all five stages in one image.

Hardware acceptance will be reported separately after the new image is booted.

## Collector stages and hypotheses

| Step | Collector section | What it tests | Evidence / acceptance |
| --- | --- | --- | --- |
| 1 | `rx-startup` | The missing OEM `0x110[8]` operation was the receiver startup defect | Checked write/readback before TX; controller owns the saved original; 30 coherent RX samples end with at least five advancing, synchronized samples |
| 2 | `rx-repeat-1`, `rx-repeat-2` | The fix survives full driver/controller teardown and restart | Two fresh controller lifetimes, the same OEM pair and same-unit DSD, independent RX captures and reverse-order cleanup |
| 2 | `rx-soak` | Synchronization, clock and analog readings remain stable | 180 samples by default; `--soak 300` / `600` extend it; power, PCS/FEC/HEC/LOF counters and clock/analog diagnostics retained |
| 2 | `rx-reconnect` | Recovery works after real light loss without a firmware replacement | Live 300-sample RX-only window, operator-confirmed disconnection, 15 dark LOS samples, confirmed reconnection and at least 15 recovered sync samples; `--skip-physical` records this as omitted |
| 3 | `activation` | Normal upstream bursts, OLT discovery/ranging, assignment and authentication work | Fresh `validation_tx=1` controller session, explicit subscriber identity, normal protocol/interrupts; five consecutive authenticated O5 samples within 180 observations |
| 4 | `provisioning` | OMCI can build service rules after registration | Five operational/authenticated O5 samples with MIB objects, installed rules and no service error; MIB dump and PLOAM/MIC errors retained |
| 4 | Security fields in MAC status | Data key exchange and selection complete | Published key-valid bitmap, selected TX key index, pending exchange and epoch, captured with traffic evidence; no key or registration bytes exported |
| 5 | `wan` | Provisioned GEM/VLAN service carries DHCP and DHCPv6 | Explicitly start the two otherwise inactive WAN interfaces; observe leases, IPv6 address/prefix, routes and interface addresses for 180 samples |
| 5 | Traffic probes, optional throughput, `traffic-soak` | Packets traverse the optical interface in both directions, including 1500-byte MTU | IPv4/IPv6 pings, do-not-fragment MTU probes and HTTPS bound to `pon`; optional 20-second iperf3 upload/download to a supplied server; final 30-sample RX/OMCI soak |
| Every session | Coherent snapshot and cleanup | Long PHY operations cannot tear the status/diagnostic pair; shutdown restores owned state | Both RX records captured under one PHY callback lock; retain 1500-ms age rejection and invalid records; disable TX before analog restore, unload owned modules and release the lower interface |

### Interpretation limits

This build instruments and tests key installation and traffic separately. The
current interface does not expose per-encrypted-GEM packet attribution, so the
report intentionally keeps `encrypted_traffic_proven=false`; installed keys alone
are not proof of decrypted user traffic. Likewise, O5 alone is not WAN acceptance.
Per-family DHCP, connectivity, MTU and throughput results remain separate. ICMP
or public-endpoint failure alone does not establish the source of a PON fault.

The shutdown issue remains separate from the original no-frame defect. Every
session exercises normal cleanup; tests stop and preserve dependencies if module
unload fails. The runner uses no legacy invasive acquisition probes once the
automatic startup fix is enabled. Those remain available in the same image's
older RX helper for explicitly selected follow-up diagnosis.

## Private inputs and running the portable script

Boot the generated activation **initramfs bench FIT into RAM** using the existing
bench boot procedure. This profile produces no sysupgrade or bootloader image.
Keep the serial capture writing to `/tmp/serial_output.log` on the host, connect
the fiber and use the existing verified `q1000k-inputs.tar` (OEM MCU pair plus
513-byte same-unit DSD slice).

Copy `activation-identity.example.json` to a private file and fill the subscriber
ONT's actual serial, WAN MAC and registration ID. Registration ID is a string of
1–36 hexadecimal bytes and is zero-padded on the right by the driver launcher;
use the line's actual setting. Other fields are optional presentation overrides.
Do not substitute the Q1000K factory identity unless it is the provisioned line
identity. The collector performs no factory fallback and never commits identity
to UCI or NAND. It stages the input only in this RAM boot.

The generated collector needs only Python 3 and OpenSSH on the host:

```sh
python3 q1000k-activation-collect.py --dry-run
python3 q1000k-activation-collect.py \
  --inputs /path/to/q1000k-inputs.tar \
  --identity /path/to/private-subscriber.json \
  --serial-log /tmp/serial_output.log \
  --output /path/to/new-capture
```

Run it in a terminal and answer its disconnect/reconnect prompts. Add
`--skip-physical` for a fully unattended connected-fiber run; that physical test
is then marked omitted. Add `--iperf-server NUMERIC_IP` for a server reachable
through the provisioned WAN. Without a server, throughput is marked not run.
Without `--identity`, or with `--rx-only`, all RX stages run but activation,
provisioning and WAN remain untested. No additional firmware image is needed
when the private identity becomes available.

The output directory and adjacent `.tar.gz` contain phase logs, coherent raw
observations, MIB/security/network evidence, serial output, per-stage JSON results
and checksums. The firmware/calibration/identity input archive is excluded.
Known subscriber credential representations are redacted from captured text;
network addresses and ordinary optical diagnostics remain in the evidence.
