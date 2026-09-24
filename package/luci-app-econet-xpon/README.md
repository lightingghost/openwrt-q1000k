# Q1000K XGS-PON LuCI interface

The status page refreshes every five seconds through the read-only
`econet-xpon.status` RPC. Configuration is stored in the `identity` section of
`/etc/config/q1000k-xgspon`; saving does not restart the PON stack. Both the
service and bench launchers validate these options through `common.sh`.

The menu is named **Settings**. Versioned view entry points (`status-v3.js` and
`settings-v3.js`, symlinks to the canonical source) avoid cached older two-field
forms when the base LuCI version stays unchanged. Image inspection compares
both packaged views, the menu and startup scripts to the selected source.

The companion supervisor automatically loads the optical controller at boot.
It waits for verified OEM firmware and unit calibration, then initializes
TX-disabled diagnostics. On a NAND-disabled RAM image the factory identity
remains unavailable; calibration can be staged separately without claiming a
factory identity. Missing inputs and the monitor state appear above the status
tables. See the [startup contract](../network/utils/q1000k-xgspon-service/README.md).

## Reference coverage

The September 2026 UI comparison uses the user-supplied AT&T fiber-status
screenshots and the [8311 configuration controller at
7d89440c7d9e1f209140910bb039f5d5a24dfbed](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/7d89440c7d9e1f209140910bb039f5d5a24dfbed/files/basic/usr/lib/lua/luci/controller/8311.lua).
The references describe another device; their example identities, thresholds,
calibration limits and firmware-specific flags are not Q1000K defaults.

| Reference feature | Q1000K implementation |
| --- | --- |
| Temperature, Vcc, TX bias, TX/RX power | Passive EN7573 `transmitter_status` readings, with per-field validity and units: °C, V, mA, dBm/nW. OMCI telemetry supplies a fallback when no controller sample exists. |
| Low/high alarm and warning thresholds/states | Omitted from the UI because the driver has no verified threshold/alarm interface. No guessed limits or inferred clear alarms. |
| LOS, TX disable, registration | Existing controller GPIO/state and OMCI status. Registration O5 is not proof of subscriber service. |
| Operational WAN status | PON netdevice carrier, explicitly labeled as link state. It does not establish Internet reachability. Missing carrier remains unknown. |
| Last link change | Unavailable: the backend exposes no timestamp. Poll uptime is labeled separately. |
| Optical assembly identity, wavelength, link lengths | Listed as unavailable. The factory ONT identity is not substituted for the optical assembly identity. |
| SFP options, DMC/EOC capabilities, rate-select pins, SFF revision | Listed as not applicable to the integrated optics. The driver does not expose a standards-compliant SFP identification EEPROM. |
| ONT serial, vendor, equipment, hardware and A/B software versions | Validated UCI fields, applied before the first OMCI MIB is populated. |
| Circuit Pack version synchronization; active/committed banks | Advertised OMCI attributes only. No flash-bank writes or boot selection. |
| Registration ID, LOID and logical password | Independent credential fields with their existing lengths and redaction rules. |
| OMCC version | `omcc_version`, hexadecimal `0x80`–`0xBF`; empty keeps the native value. Changes presentation, not supported protocol features. |
| PON slot | `pon_slot`, decimal 1–254 except 128 (reserved optical slot). Default 1. Applies to PPTP UNI, UNI-G, Dot1X, Cardholder/Circuit Pack and profile-specific UNI objects; full-entity data-port mapping uses the configured slot. |
| IP-host MAC, hostname, domain | `iphost_mac`, `iphost_hostname`, `iphost_domain`. Optional identity-only ME 134, instance 0, attributes 2, 14 and 15. Hostname/domain are at most 25 printable ASCII bytes. An omitted MAC falls back to the selected WAN MAC if another IP-host field is set. All empty omits the instance. Routing/DHCP attributes and writes are not acknowledged. |
| MIB file | Omitted from the UI; there is only one supported native PPTP profile. |
| OMCI interoperability mask | Omitted from the UI. The WAS-110 bits have no equivalent numeric ABI here. Native `olt_profile` choices are automatic, generic, Nokia/ALCL, DASAN, Huawei, FiberHome and ZTE. |
| VLAN fix | Existing native VLAN-0 normalization; no TV/voice VLAN remapping. |

The RPC does not run raw I²C commands, select an ADC channel, clear alarms,
trigger a measurement, or change TX. A successful bus read does not establish
the age of the MCU's published sensor value or external connector power.
Missing, malformed and failed samples remain unknown, including after refresh.
Settings use the generic 8311 descriptions without device or ISP examples.
Module information and PON/OMCI details are expanded sections, not disclosures.

## Startup and validation

The Q1000K provider retains an immutable initial identity for its registration
lifetime. The OMCI core copies it before creating its initial MIB, including
the optional IP-host instance and selected Ethernet slot. No live topology
mutation is performed by the configuration page. Rebuild the provider and OMCI
core together: their internal provider contract changed.

Relevant checks:

```sh
PYTHONPATH=tests/q1000k python3 -m unittest \
  test_xgspon test_pon_config test_pon_identity \
  test_pon_omci_backend test_pon_services
node tests/q1000k/test_xgspon_views.cjs
bash tests/q1000k/run_omci_core_uml.sh
```

Fixtures cover partial/invalid sensors, negative temperatures, zero readings,
refresh failure, shared CLI/LuCI validation, startup identity, alternate UNI
slots, and the ME 134 identity encoding. The UML test exercises the complete
OMCI core with lockdep; these checks do not establish hardware or OLT acceptance
of the new settings. Browser layout checks use synthetic data, not a router.
