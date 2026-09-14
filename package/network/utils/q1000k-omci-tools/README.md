# q1000k-omci command

`q1000k-omci-tools` installs `/usr/sbin/q1000k-omci`, a userspace management
client for the `kmod-q1000k-omci` kernel core. It uses Generic Netlink and
libmnl. It does not replace the in-kernel OMCI agent, bind its raw packet
observer, load modules or initialize optics. The package remains optional
and marked BROKEN with the experimental PON stack.

Examples on an already initialized matching build:

```sh
q1000k-omci -i pon status
q1000k-omci -i pon mib
q1000k-omci -i pon mib 277 0x8003
q1000k-omci -i pon get serial
q1000k-omci -i pon get olt-profile
q1000k-omci -i pon set olt-profile auto
q1000k-omci -i pon set version Q1000K-test
```

These are command examples, not instructions to run them during the current
read-only hardware work. No command was installed or executed on the Q1000K.
Local tests use a disposable UML kernel and synthetic `pon-test` interface.

Use `--help` for all keys. `-d ID` selects a core device ID; the default is ID 0.
`-i INTERFACE` selects its actual interface index and never falls back to ID 0.
Only one selector is accepted. IDs accept decimal or `0x` hex. Profiles accept
`generic`, `auto`, `nokia`, `dasan`, `huawei`, `fiberhome`, `zte`; the forced
profile also accepts `none`. ONU types accept their UAPI names, such as `sfu`.
Serial/vendor reads are available, but changing registration identity belongs
to the module loader. Password/key material has no CLI accessor.

Output is JSON; `--json` is accepted explicitly. Unavailable telemetry is
`null`, and 64-bit counters are decimal strings to retain precision in LuCI.
`authenticated` describes core session admission. `agent_operational` records
agent exchanges; neither that flag nor a nonzero `service_rules` count proves
optical Internet service. `service_rules` counts configured rules, which may
still be dormant awaiting PLOAM allocation. `service_error` reports a retained
physical fault or the latest reconciliation error. No key or epoch value is
exposed in status.

`mib` walks the live MIB with bounded, monotonically advancing cursors. It is
an inspection operation, not an atomic backup/export or a restart image.
Output is buffered until the operation succeeds, so errors do not leave a
partial JSON array. Configuration writes go through kernel validation and
return success only after the kernel acknowledges them. They are runtime
settings, not persistent UCI writes. The command does not offer raw PDU TX,
MIB reset/delete, optical start, firmware or flash operations.

The client checks the Generic Netlink family version, response command,
sequence, kernel sender, scalar lengths and malformed/duplicate attributes.
Socket receives time out after three seconds. Errors produce a negative errno
JSON object, a stderr explanation and a nonzero exit status.

Validation: `test_pon_omci_cli.py` compiles the actual client parser with UBSan
and tests malformed replies, integer bounds, named settings, identity write
rejection and JSON escaping. It needs the staged libmnl header and a host
libmnl runtime. For the real userspace/kernel boundary, compile this source
for the host and set `Q1000K_OMCI_CLI` to that absolute executable path when
running `tests/q1000k/run_omci_core_uml.sh`. The runner creates only a synthetic
interface inside UML, tests status/MIB/get/set/errors, then unregisters it.
