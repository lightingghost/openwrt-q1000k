# q1000k-omci command

`q1000k-omci-tools` provides the CLI for the Q1000K kernel OMCI agent. It does
not load optical modules, start another OMCI daemon, transmit raw PDUs, change
bootloader variables or perform firmware operations. The package remains
experimental/BROKEN. Runtime commands require matching Generic Netlink v16.

## Staged identity (also available in LuCI)

These commands work with the stack unloaded. They modify only the
`q1000k-xgspon.identity` UCI section, using the same validation and parameter
builder as the supervisor and RAM bench. Each set/clear commits this UCI
package; it does not activate PON. In the RAM image the configuration is
volatile. On an installed system it has normal UCI persistence.

```sh
q1000k-omci config list
q1000k-omci config get hardware_version
q1000k-omci config set equipment_id iONT320500X
q1000k-omci config set hardware_version BGW320-500_2.1
q1000k-omci config set sync_circuit_pack 1
q1000k-omci config clear active_bank
q1000k-omci config validate
```

`list` redacts registration and logical credentials. An explicit `get KEY`
returns that configured value, including secrets. `validate` checks syntax
of all fields; it does not establish completeness, provisioning or optical
readiness. Empty optional fields restore native defaults on next startup.
Serial and WAN MAC default to the factory identity in the normal supervisor.
Registration must be explicitly configured before that supervisor can start.

| UCI/CLI key | Accepted value and effect |
| --- | --- |
| `serial` | 4 ASCII vendor letters/digits + 8 hex digits; PLOAM and OMCI serial |
| `vendor_id` | 4 ASCII letters/digits; OMCI vendor, derived from serial if empty; independent of PLOAM serial |
| `equipment_id` | Up to 20 printable ASCII bytes; ONU2-G and equipment MEs |
| `hardware_version` | Up to 14 printable ASCII bytes; ONU-G version |
| `sync_circuit_pack` | `1` follows hardware version; `0` retains native `OpenWrt` Circuit Pack version; native default `1` |
| `software_version_a`, `software_version_b` | Independent 14-byte Software Image ME7 versions, instances 0 and 1 |
| `active_bank`, `committed_bank` | Empty means native A; `0`=A, `1`=B; advertised ME7 metadata only, no boot or flash effect |
| `registration_id` | 1–36 bytes as 2–72 hex digits, padded with zeros on the right to 36 bytes; empty remains unconfigured |
| `logical_onu_id` | Up to 24 printable ASCII bytes, ONU-G attribute 10 |
| `logical_password` | Up to 12 printable ASCII bytes, ONU-G attribute 11; separate from registration ID |
| `wan_mac` | Nonzero unicast Ethernet address |
| `mib_profile` | `native-pptp`: one physical Ethernet UNI in SFU mode; no foreign platform MIB file |
| `fix_vlans` | `1` enables subscriber VLAN-0 normalization; `0` keeps strict OLT rule matching |
| `omci_version` | Legacy combined 14-byte fallback; separate hardware and software fields take precedence |

For a registration password occupying the final 12 bytes, supply the complete
36-byte registration value, including the leading padding. Do not use a
short password expecting it to be automatically moved to the end.

`fix_vlans=1` first tries the native OLT-provisioned class 84/171 rules. An
unmatched untagged upstream frame can then be classified as VLAN 0, PCP 0,
through those same rules, filters, queue selection and drop actions. Downstream,
a lone 802.1Q VLAN-0 tag is removed at the subscriber boundary. Nonzero VLANs,
double tags and explicit drops are preserved. This covers an untagged Internet
WAN with priority-tagged provisioning; it is not the full 8311 multi-service
TV/voice remapper and does not guess optical VLANs.

## Runtime inspection and configuration

```sh
q1000k-omci -i pon status
q1000k-omci -i pon mib
q1000k-omci -i pon mib 7 0
q1000k-omci -i pon get hardware-version
q1000k-omci -i pon get software0
q1000k-omci -i pon get vendor
```

Runtime `get/set` keys: `serial` (read only), `vendor`, `version` (legacy
combined), `hardware-version`, `equipment`, `software0`, `software1`,
`sync-circuit-pack`, `active-bank`, `committed-bank`, `logical-onu-id`,
`logical-password`, `enabled`, `onu-type`, `uni-count`, `olt-profile`,
`olt-profile-force`, `omcc-version`. Banks and booleans use 0/1. The Q1000K
provider pins SFU/one UNI; runtime attempts to select another physical model
are rejected. Runtime changes do not update UCI and disappear on reload.
Use staged configuration for the next cold startup and its initial MIB.

`-d ID` selects a core device ID (default 0); `-i INTERFACE` selects that
interface without falling back to ID 0. Only one selector is accepted.
Profiles accept `generic`, `auto`, `nokia`, `dasan`, `huawei`, `fiberhome`, `zte`;
forced profile also accepts `none`. IDs accept decimal or `0x` hexadecimal.

Runtime output is JSON. Unavailable telemetry is `null`; 64-bit counters are
decimal strings. `authenticated`, `agent_operational` and `service_rules` do
not prove Internet access. `service_error` retains physical/reconciliation
failures. Status has no credentials. MIB diagnostics redact logical credentials
and mark ONU-G `credentials_redacted:true`; OLT GET responses retain them.
`mib` is a bounded inspection, not an atomic restart image. Errors return a
negative errno JSON object, stderr explanation and nonzero exit status.

Host tests cover the actual CLI parser, UCI parser/commits in isolated temporary
directories, LuCI validators, hex parameter forwarding, byte boundaries,
credential redaction and malformed input. The disposable UML suite verifies
the real core, cold identity, MIB reset, wire serialization and netlink redaction.
For the runtime CLI/netlink boundary, compile the client for the host and set
`Q1000K_OMCI_CLI` when running `tests/q1000k/run_omci_core_uml.sh`.

RX optical telemetry is supplied by the active controller before registration,
including receive-only O1. `status` reports `rx_power_nw` and `rx_power_dbm`
(`10 * log10(nW / 1,000,000)`, rounded to two decimal places). Both are null when
no usable measurement is published; zero/saturated controller words do not
become a dBm value. This is controller-reported power, not independent meter
calibration. Reading status does not enable transmission or start the stack.
