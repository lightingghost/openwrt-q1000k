# Q1000K EN7573 control driver

This optional, manually loaded driver handles the two EN7573AN paths on the
AN7581SIT Q1000K. It supports detection and XGS-PON MD32 bring-up with the
TX-disable control asserted. It does not implement the SoC PON PHY/MAC,
registration, OMCI, service provisioning or automatic startup.

The implementation uses Linux I2C, GPIO descriptors and firmware APIs. The
wire protocol was derived from the OEM board sequence and the
[EN757x reference at 950199a](https://github.com/Sirherobrine23/airoha_xpon_en757x/tree/950199a8de6b75e76906a7c1b39b7a9a3e2913f9/v2/lddla).
The proprietary loader was not imported or linked. No OEM blobs are included.

## Board binding

The `quantum,q1000k-pon-control` I2C child is at address `0x51` on the
AN7581 I2C controller. Probe also reserves memory address `0x50`; another
client using that address makes probe fail. The driver requires the
`quantum,q1000k-ubi` machine compatible and these GPIO properties:

| Property | GPIO controller offset | Polarity / function |
| --- | ---: | --- |
| `gpon-enable-gpios` | 9 | Active low, GPON power enable |
| `xgspon-enable-gpios` | 8 | Active low, XGS-PON power enable |
| `select-gpios` | 45, 46 | Both low: GPON; both high: XGS-PON |
| `gpon-los-gpios` | 10 | Active high LOS; OEM mapping, GPON initialization untested |
| `xgspon-los-gpios` | 11 | Active high LOS; disconnected-fiber state tested |

The board DTS assigns these six pins to the GPIO function. The AN7581
pinctrl patch adds the missing force-GPIO functions and preloads output
latches before enabling output. Global sysfs GPIO numbers are not used.
The additional I2C-master selection bit at SCU `0x214[13]` is unnecessary
for this board and is not changed.

## Sysfs interface (development ABI, version 1)

Discover the single `*-0051` child under
`/sys/bus/i2c/drivers/q1000k-pon-control/`; do not assume adapter number 0.

- `status` is JSON. Detection values are null until checked, then record
  the last ID probe and its boot-relative `checked_uptime`. They are not
  continuous presence checks. `mode` is `off`, `gpon`, `xgspon` or `unknown`
  if an enable write failed. `md32_enabled` reads the MCU enable bit; it is
  not a firmware heartbeat. `firmware_verified` means the exact OEM input
  hashes and complete memory readback passed in this initialization.
  `tx_disabled` is true after power-off or a successful initialized-state
  check; it is null when the state cannot be established. `los` is only
  sampled after initialization. Reading status never powers, selects, resets
  or writes controller registers. Failed register reads report unknown MCU/TX
  values and `last_error` as negative Linux errno. Unexpected MCU/TX bits are
  reported as sampled, with `last_error = -EIO`. An operator must explicitly
  request `off` to shut down; polling is not a protection mechanism.
  `stage` identifies the
  operation stage; it does not imply optical service readiness.
- `calibration` accepts one offset-zero write of exactly 513 bytes. The
  factory reader provides this unit's record. The first 512 bytes must not
  be entirely zero or erased. The full record is retained privately in
  RAM; its first 512 bytes are loaded at DM offset `0x600`. The final byte
  is not part of the controller's 512-byte calibration payload. No data
  is written back to factory storage or replaced with vendor strings.
- Root-only `operation` accepts `detect`, `initialize` or `off`.
  `detect` powers one path at a time, reads `0x51:0x0408`, expects family ID
  `0x1388`, then powers both off. A failed read retains its errno.
  `initialize` requires calibration and the exact firmware pair documented
  in [the checkpoint](../../../target/linux/airoha/XGSPON-STATUS.q1000k.md).
  Firmware is verified before powering a path. Both enables are inactive
  while selectors change. The loader holds TX-disable, halts/resets MD32,
  writes zero-padded PM (16 KiB), DM (4 KiB) and the calibration payload,
  and verifies every word before setting MCU enable. Errors power off.
  Repeated initialization/detection while initialized returns `EBUSY`;
  use `off` first. Removal and system shutdown power both paths off.

Control and memory-address registers use I2C `0x51`. Only the PM/DM data
ports use `0x50`. Register pointers are big endian; data words are little
endian. Bulk writes auto-increment; readback explicitly sets every address.
Short transfers and readback mismatches fail immediately. TX-disable is
register `0x3e0` bit 9, and MCU enable is `0x3018` bit 0.

For bench use with the fiber disconnected, a kernel/DT built from this
branch, matching modules and the verified OEM files installed locally:

```sh
modprobe q1000k-pon-control
q1000k-xgspon detect
q1000k-xgspon initialize
q1000k-xgspon status
q1000k-xgspon off
```

These commands do not enable an optical Internet service. The live tests
used a temporary DT-less harness on the older running image, then removed
both modules and restored the saved GPIO/mux bits. The production DT has
been compiled but has not yet been boot-tested. Cold boot, complete OEM
analog tuning/alarm behavior, fiber-present LOS, long-running MCU health
and integration with the SoC PHY remain acceptance work. The register
checks are samples, not an independent hardware laser interlock.

## Host verification

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I package/kernel/q1000k-pon-control/src \
  tests/q1000k/test_en7573.c package/kernel/q1000k-pon-control/src/en7573.c \
  -o /tmp/q1000k-test-en7573
/tmp/q1000k-test-en7573
```

The test executes the production loader with synthetic firmware and an I2C
memory model. It checks address spaces, byte order, exact calibration,
padding, readback rejection, no MCU enable before successful verification,
invalid input rejection and immediate error propagation at every transfer.
State sampling is tested with no write/delay callbacks, all four MCU/TX bit
combinations and failures of either register read; stale values become unknown.

## Exclusive kernel consumer

The exported `q1000k_pon_get/check/set_tx/get_los/put` API requires process
context. Acquisition requires this unit's calibration and verified controller
firmware already initialized in XGS mode with TX disabled. One consumer holds
an exclusive reference; sysfs operation and calibration writes return busy
while it is held. References survive I2C-device removal, after which operations
return `-ENODEV` without accessing released GPIO or I2C resources. Release
disables TX and drops the reference; a context error leaves the reference held
so the caller can retry from process context.

TX changes preserve unrelated control bits and check readback. Controller
state is checked against the expected TX state, including MCU enable.
Transport failures and inconsistent state latch a fault and attempt power-off
containment. A failed power-off remains an error, never a successful optical
stop. Recovery requires a fresh verified initialization while no consumer
holds the controller. No module autoload or board activation is added.

The production consumer fixture covers exclusive acquisition, release,
removal with an outstanding reference, context rejection, state mismatch,
LOS/I2C failures and failed containment. The EN7573 fixture still passes all
15,388 firmware-loader I2C failure points, plus TX field preservation and
control read/write/readback failures. The Linux 6.18.44 AArch64 r3 package
builds (8,711 bytes) and installs its development header and exported symbol
file. These are local tests; the controller was not accessed on hardware.
