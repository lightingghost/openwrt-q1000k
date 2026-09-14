# Experimental Q1000K supervisor

This optional package provides `/etc/init.d/q1000k-xgspon` and
`/usr/libexec/q1000k-xgspon-run`. It is marked `BROKEN`, is disabled in UCI
by default, and is not selected by the diagnostics configuration. Board PON
resources remain disabled. It has been tested with simulated modules and
controller files only. It is not evidence of working optical service.

The current development session permits only read-only device access and
never flashing. The activation interface below documents future acceptance
work; it must not be executed under that restriction.

## Configuration and commands

The existing `/etc/config/q1000k-xgspon` contains:

- `identity.serial` and `identity.wan_mac`: empty values use validated factory
  identity. Overrides have the same validation as the diagnostics backend.
- `identity.registration_id`: an explicit 36-byte registration ID encoded as
  exactly 72 hexadecimal digits. The supervisor never invents this credential.
- `service.enabled`: defaults to `0`; must explicitly be `1` for startup.
- `service.lower`: the native PON lower interface, already administratively
  up. The native driver additionally validates its hardware role at attach.
  The supervisor does not configure netifd, change LAN devices, or bring up
  the named device.

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
