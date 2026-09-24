# Experimental Q1000K supervisor

This optional package provides `/etc/init.d/q1000k-xgspon` and
`/usr/libexec/q1000k-xgspon-run`. It is marked `BROKEN` and is selected by
the LuCI package. Automatic diagnostics load the controller at boot with TX
disabled; subscriber registration remains explicitly enabled. The board must
provide enabled controller resources. Module presence does not establish
working optical service.

RAM monitoring waits for verified OEM firmware and a unit-specific 513-byte
calibration record at `/lib/firmware/airoha/q1000k/xgspon-calibration.bin`.
NAND remains disabled; no private inputs are embedded in the public image.
Only the controller is loaded, with no TX permission, PHY, MAC or registration.
Status RPCs remain read-only. The monitor uses the same exclusive ownership and
cleanup rules as full startup and reports `waiting_firmware`,
`waiting_calibration` or `monitoring`. A fault stops it without respawn.

The explicit bench launchers and collector call `stop_monitor` before claiming
hardware. This releases only a monitor owned by this service, and leaves a full
registration service alone. Diagnostics can be resumed with
`/etc/init.d/q1000k-xgspon start` after a completed bench session.

## Configuration and commands

The existing `/etc/config/q1000k-xgspon` contains:

- `identity.serial` and `identity.wan_mac`: empty values use validated factory
  identity. Overrides have the same validation as the diagnostics backend.
- `identity.registration_id`: an explicit 1–36-byte registration ID encoded as
  pairs of hexadecimal digits, zero-padded on the right to 36 bytes. The supervisor never invents this credential.
- `identity.equipment_id`: optional printable ASCII text, at most 20 bytes.
- `identity.omci_version`: optional printable ASCII text, at most 14 bytes.
  The core applies this to ONU-G version and both software-image versions.
  Empty equipment/version values retain the native Q1000K/OpenWrt defaults;
  the gateway model alone does not establish appropriate override values.
- Additional vendor, hardware, separate software A/B, Circuit Pack sync, bank
  metadata, logical credentials, native MIB profile and VLAN-0 settings are
  documented in the [OMCI CLI contract](../q1000k-omci-tools/README.md).
  The separate version fields override the legacy combined version.
- `service.monitor`: defaults to `1`; starts optical diagnostics at boot when
  full registration is disabled. Exposed in LuCI Settings.
- `service.enabled`: defaults to `0`; must explicitly be `1` for full startup.
  Exposed as “Start PON Internet service at boot” in LuCI Settings.
- `service.lower`: the native PON lower interface, already administratively
  up. The native driver additionally validates its hardware role at attach.
  The ordinary profile does not configure netifd or bring up the named device.
- `service.continuous_bench`: private activation RAM profile only. Requires
  the image marker `/etc/q1000k-private-autostart` and activation DT, and
  restricts the lower device to `ponraw`. Public images leave this off.

With this package installed, `q1000k-xgspon start|stop|restart|reload` delegates
to the init service. Changes take effect only after an explicit lifecycle
command; there is no UCI reload trigger. `q1000k-xgspon status` exposes the
last supervisor report separately from optical readiness. A saved `running`
stage may survive abrupt process termination and does not prove liveness.

## Ownership and failure behavior

Preflight verifies the exact board, factory calibration availability, identity,
firmware hashes, registration ID, lower link state, and absence of all known
PON modules. An exclusive runtime lock prevents competing supervisors.

Startup loads the standalone controller, initializes and verifies MD32 with
TX disabled, then loads the BSP, PHY, generic PON/OMCI core and MAC in dependency
order. Only the final MAC module receives immutable identity and lower-device
parameters. Registration IDs are never written to status or routine logs.
Equipment and version are validated before any module load, hex-encoded for
module-argument transport and independently checked by the MAC. Spaces and
printable punctuation are preserved; controls, non-ASCII and oversized values
are rejected. The MAC caches supplied values and applies them to the OMCI core
before startup, so the initial MIB contains the configured identity. These
fields do not change the PON serial or registration credential.

The process polls checked controller status, the MAC's cached protocol error
and the read-only OMCI command. Malformed samples, missing providers, protocol
faults and an uncertain OMCI rollback terminate supervision. Unsupported or
incomplete service provisioning alone does not cause a power cycle. Kernel
fault containment closes admission immediately; this poll is not its substitute.

Stop/failure removes only modules loaded by this instance, in reverse order,
with controller power-off before its removal. A failed unload retains that
module and its dependencies, records `cleanup_failed`, and returns failure.
An existing module is never adopted or unloaded by a new invocation. A later
start consequently refuses a partially retained stack. There is no automatic
respawn or automatic restart following a hardware fault.

The runtime report is written atomically with mode 0600 to
`/var/run/q1000k-xgspon/status.json`. Stages identify the failed step; `error`
is the supervisor's exit result, not a hardware errno. Detailed controller,
protocol and OMCI diagnostics remain available through their read-only APIs.

## Private continuous-service RAM image

`scripts/q1000k/bench-build.py --profile activation --working-tree
--private-inputs INPUTS.tar --private-identity IDENTITY.json --output OUTPUT`
explicitly embeds the approved firmware, per-unit calibration and validated
subscriber identity. The input archive uses the existing activation collector's
size/hash checks. Identity values become the actual UCI identity section, so
LuCI displays and edits the same settings read by the service. An omitted
registration value follows the collector's explicit 36-byte zero-default
policy; it is recorded in the private manifest. No private values enter source
files, the source snapshot or normal build output.

The generated `zz-q1000k-private-autostart` UCI default runs after the ordinary
bench and WAN defaults. It keeps both copper ports on LAN at 192.168.255.1/24,
enables DHCPv4, SLAAC/RA and DHCPv6, and preserves the standard WAN firewall
zone, IPv4 masquerading and LAN-to-WAN forwarding. IPv6 uses DHCPv6-PD without
IA_NA: netifd allocates /64 hint 0 to LAN and hint f to the router from the
current provider prefix. No subscriber prefix or public source address is
hardcoded, and IPv6 is routed without NAT66 or neighbor proxy workarounds.

The private service grants the activation controller `validation_tx=1` for
its lifetime, runs detect/initialize, opens `ponraw` if needed, and loads the
last Internet-tested bench module recipe. `rx_bench=0` retains the normal
driver's indefinite LOS and frame-sync polling. Optical registration remains
gated by the driver; detecting light alone does not grant TX permission to a
diagnostics-only monitor. Three healthy, provisioned O5 samples start the WAN
clients with direct netifd `up` calls. These avoid the `ifup` wrapper's forced
down/up cycle. Ordinary signal loss keeps the modules and network state;
provisioned recovery requests DHCP/DHCPv6 renewal. Hard protocol/controller
faults still stop the service and require diagnosis before an explicit restart.

Use `scripts/q1000k/live-collect.py` for observations and bounded connectivity
probes while the service runs. It performs no remote cleanup. The finite
activation/RX launchers require exclusive ownership and refuse a running full
service; stop the service explicitly only for an intentionally disruptive
experiment. Stop/fault cleanup releases this owner's WAN clients and modules,
and closes the lower device only if this owner opened it.

Private ITBs and intermediate root filesystems contain subscriber inputs.
Keep them private. RAM edits revert to the embedded values on another RAM boot;
this workflow does not write NAND. Host fixtures verify startup/recovery and UCI
ordering; the actual cold boot, LAN Internet and native PD behavior still need
hardware acceptance on each new candidate image.
