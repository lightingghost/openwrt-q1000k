# Q1000K optical PHY integration

All evidence below comes from local source inspection and disassembly of the
cached QKX001-06.00.44.00 firmware. Neither the kernel nor extracted OEM
modules were executed. The Q1000K was not accessed for these changes.

## IRQ evidence

The decompressed ARM64 kernel has SHA256
`5588ee2a6c54faf57e32ab6504bad5c7468580245294ae48f39cc67f6ce393ca`.
Its embedded kallsyms were recovered with
[vmlinux-to-elf](https://github.com/marin-m/vmlinux-to-elf).

`get_pon_phy_irq` at `ffffffc0100d88c0` loads the pointer at
`ffffffc010c6c8a0` and returns the word at structure offset 72.
`en7581_serdes_common_phy_probe` at `ffffffc0100d8a08` populates that same
field with `platform_get_irq(pdev, 0)` at `ffffffc0100d8d1c`.
The OEM `serdes_common_phy@1fa5a000` DT node supplies GIC SPI **43**, level
high. This is not MAC SPI **42**, dying-gasp SPI **34**, or the Ethernet
PCS interrupt **66**. The imported generic PHY provider excludes the IRQ
lookup on EN7581; exposing that provider's zero-filled field was incorrect.

The OEM `xpon_int.ko` independently requests MAC IRQ 0 and dispatches the
MAC ECNT hook with source `XPON_INT_MODULE`. It does not dispatch PHY events.
The native PHY and MAC therefore need separate IRQ lifecycles.

## Optical resource provider

Vendor patch 024 selects `an7581_pon_phy.c` only for AN7581. AN7583 retains
its original provider. The disabled Q1000K PHY node supplies:

| Name | Physical address | Size |
| --- | --- | --- |
| digital | `1faf0000` | `1fff`, matching the OEM digital window |
| ana | `1fa8a000` | `1000`, matching the native PON PCS resource |
| pma | `1fa8b000` | `1000`, matching the native PON PCS resource |

Probe validates all addresses and sizes, claims the named resources, obtains
IRQ 43 by name, then publishes the provider. It writes no hardware registers.
Manual bind/unbind attributes are suppressed because legacy callers borrow
the device pointer. Module symbol dependencies pin the resource provider.
Removal excludes active register access before devres frees the mappings.

Only aligned words entirely inside these windows are accepted. Physical
addresses and their exact MIPS KSEG1 aliases are supported; unrelated aliases,
FPGA TX-off, copper XFI, old analog windows and SCU addresses are rejected.
The legacy generic `Get_Base` mapper is not used by this provider.

Reads and writes serialize with removal. All writers, including full writes
and bitfield updates, share the same lock. Bitfield updates validate bit and
value ranges and hold the lock over the complete read/modify/write. Full-word
writes do not read first, preserving W1C semantics. Legacy access failures
latch a status error, checked before PHY initialization can report complete.
MMIO access alone cannot report a hardware bus failure; a successful accessor
is not proof of analog readiness or calibration.

The production accessor fixture checks window edges and aliases, absence and
removal, correct IRQ publication, every bitfield width, unrelated-bit
preservation, full-write semantics, sticky errors, and concurrent RMW from
four threads. UBSan is enabled. The board remains disabled pending coordinated
reset, clock, controller and MAC startup. This resource change alone does not
make optical operation available.

The AArch64 Linux 6.18.44 vendor package passes modpost and packaging as r22
(334,582 bytes). The updated board DT compiles with the kernel DTC. All 32
PON host tests pass, including the new provider test.

## Typed PHY lifecycle

Patch 025 selects a separate Q1000K lifecycle in `q1000k_phy.c`. Module load
validates the provider and AN7581 ID, allocates software state and initializes
its timer and locks. It does not change IOMUX, initialize analog hardware,
create debug proc writers, register a raw PHY hook, or publish legacy callback
pointers. Direct exported functions pin `phy_10g` while the MAC uses it.

Configuration, start, stop and public API calls are process-context operations.
They reject interrupt/atomic contexts and observable RCU nesting. Without
lockdep, `rcu_read_lock_held()` is a constant true stub and is not used as a
runtime prohibition; PREEMPT_RCU nesting is checked separately. Callers still
must honor the process-context contract on builds without RCU tracking.
Lifecycle and callback mutexes serialize operations; nested calls receive
`-EBUSY` instead of waiting on their own callback. A caller receiving `-EBUSY`
from stop has not completed shutdown and must retry from its owning worker.

Configuration accepts XGS-PON only, requires WAN selection 10, and requests
TX disabled. It does not launch the legacy background retry thread on failure.
Start requires successful configuration, masks all optical IRQ sources and
clears pending interrupts before requesting IRQ 43. Ownership is recorded only
after IRQ acquisition succeeds. It then enables and verifies the digital IRQ
mask. Failed mask/readback or subsequent callback errors latch a fault and
close callback admission. Failed IRQ acquisition leaves the source masked and
can be retried.

The threaded IRQ checks the PHY's own enabled digital/rogue pending bits
before dispatching. The legacy unconditional interrupt counter and 50 ms busy
wait are bypassed. Timer callbacks only queue work; polling and IRQ callbacks
serialize in process context. Stop closes callback admission, masks sources,
drains IRQ callbacks, deletes the timer, cancels polling work, deletes any
racing rearm and masks again after callbacks finish. Unload releases software
state only after this drain. None of these operations establishes optical TX
or MAC/FE drain.

MAC wrappers return the typed operation's actual error, including when the
PHY is not configured. GPON-family initialization now requests the XGS-PON
profile on Q1000K and unwinds crypto if PHY configuration fails. Existing
internal PHY tuning, full SCU/reset calls and legacy API bodies still need the
controller/reset ownership audit; the typed wrapper is not proof that those
hardware operations are complete or individually validated. Hardware gates
remain in place.

All 33 PON host tests pass. The lifecycle fixture injects failures at each
startup mask write/read, IRQ allocation, clear, mode setup, polling and stop;
it checks reentry, sticky failure and retry ownership. The UML fixture uses
real mutexes, RCU, timers, workqueues and kthreads with synthetic registers
and IRQ acquisition. It passes 50 cycles plus unload while a polling callback
and an IRQ thread are blocked using PHY state, with no runtime kernel
warnings. The AArch64 r23 package builds and packages successfully (341,589
bytes), and its prepared sources match the tested source and patches.

## Reset-controller prerequisite

The imported PCIe reset polarity patch (`609-02`) used `val |= ...` on an
uninitialized local in `en7523_reset_update`. The regmap conversion also
ignored errors from update and status reads. Kernel patch `9999f` assigns the
reset value directly and returns regmap errors from both operations. It keeps
the target-bit mask and the inverted PCIC polarity, preserving unrelated bits.

The production reset callbacks pass host tests across all 32 bit positions in
three banks, both polarities, several initial bit patterns, and read/update
errors. The Linux 6.18.44 AArch64 kernel builds successfully with the patch;
the prepared clock source matches the tested source. No reset was performed
on the Q1000K.

## Exclusive reset and callback ownership

Patch 026 replaces the selected XGS-PON top reset with a checked sequence.
It gates the digital clock, changes only WAN selection bits 7:0 to 17,
checks the analog clock gates, pulses the exclusively acquired
`EN7581_XPON_PHY_RST` through the kernel reset controller, restores selection
10, then checks the digital reset pulse. Reset failure leaves the clocks
gated. Reset-provider calls execute without an IRQ-disabled register lock;
register accesses are rejected while a pulse is in progress. Assertion,
deassertion and status failures are sticky. The disabled PHY DT node now
names this reset explicitly.

The shared-SCU API updates only the WAN selector or PON PBUS access bit,
checks readback and propagates regmap errors. It never restores an old full
SCU word. Other reset consumers keep ownership of their bits.

The lifecycle records the task owning its callback mutex. Internal GET/SET
helpers, the selected XGS IRQ/poll callbacks and reset helpers require that
owner in process context. Their old nested IRQ spinlocks are bypassed on
Q1000K, including the XGS handler's mismatched IRQ-save/unlock pair. PMA reset
now propagates TX-disable, reset and restore errors and does not restore TX
after a failed reset. Controller wiring and remaining raw API bodies are
separate integration work; this checkpoint does not enable hardware.

All 36 PON host tests pass with UBSan, including every top-reset transport
failure, ignored clock writes, SCU field preservation and callback ownership.
The UML PHY test passes its 50-cycle and concurrent teardown cases. Vendor
r24 builds against Linux 6.18.44 and packages successfully (350,748 bytes).
No reset or other write was executed on the Q1000K.

## EN7573 and board wiring

Patch 027 connects the PHY to the controller's exclusive kernel-consumer API.
Configuration acquires the controller only after verified firmware/calibration
initialization, checks it, selects the native pinctrl `pon` group explicitly,
and enables only the PON PBUS access bit. Probe acquires the named pin state
without selecting it. This replaces the selected backend's full IOMUX/PBUS
writes and reference-board LED/GPIO changes. The disabled board node names
only the explicit `pon` state, so the driver core cannot select a default
state before controller ownership.

The OEM boot log selects profile 82, vendor ECONET / EN7572, for its EN7573
controllers. The imported table agrees with the three observed values:
SFP valid level `0x9`, PMA setting 0 `0x10001`, PMA setting 1 `0x1010100`.
The Q1000K path writes and verifies these values after checking the owned
controller. It does not probe the controller address through the generic SFP
I2C path or substitute a different transceiver profile.

TX switching now reaches the EN7573 control register through its owner.
Configuration and IRQ startup leave TX disabled. A separate typed operation
requires an active, configured PHY before enabling it; temporary PMA-reset
suppression preserves the desired state and restores it only while authorized.
Stop revokes that authorization and disables TX. Poll/IRQ callbacks check
controller state; faults close callback admission and attempt TX disable.
Module exit drops the controller reference only after callbacks drain.

All 37 PON host tests pass. Added cases cover missing-controller retry,
pinctrl/PBUS failures, profile write/read errors, startup authorization,
PMA TX suppression/restore and stop. The UML callback test passes with the
connected lifecycle. The board DT compiles. Vendor r25 builds and its prepared
sources match the tested files. No hardware access was performed. MAC startup,
physical drain and OMCI coordination still must use these typed interfaces
before the disabled board gates can be removed.

## Observed FEC telemetry

Vendor r59 connects the checked external TX-FEC query to the OMCI provider.
The query reads `EN7581_XGPON_PHY_DBG_TX_FEC_STA`; it does not infer FEC from a
configured burst profile. Sampling holds the protocol owner and requires a
cold-started, started, active O5 session with no pending reset. PHY/controller
faults, invalid boolean results and inactive sessions leave output unchanged
and return an error. Only `OMCI_TELEMETRY_F_FEC_UPSTREAM` is published.

Downstream FEC configuration is not a measurement, so the corresponding valid
bit remains clear. Temperature, voltage, bias and optical powers also remain
unavailable pending a calibrated controller interface. This path does not
select a debug probe, clear counters or issue a generic SFP sensor read.

Core r13 clears all telemetry validity when a provider reports failure, even
if the provider partially populated its output. The UML fixture parses the
real generated netlink attributes, checking success, missing callbacks,
`-EIO` and `-ENODATA`, and absence of every unvalidated field. Host tests
exercise the production provider and each session/query rejection. Matching
AN7581 packages build; no Q1000K connection or firmware flashing occurred.

## Legacy fault admission and analog phase errors

Vendor r60 closes legacy register admission after a latched provider fault.
The check is inside the same MMIO lock as the read or write, covering physical
and KSEG1 addresses, full legacy writes and masked/full-width field updates.
Explicit checked reads/full writes remain available for lifecycle containment;
a successful containment write does not clear the original fault. The host
provider tests verify that no further legacy MMIO occurs in all three windows.

Patch 050 routes AN7581 PMA mode initialization through a process-context
Q1000K helper. It preserves the imported `xpon_init` and `fiber_plug_reset`
sequences, including first calibration followed by power saving when LOS is
asserted. Provider/controller checks separate those phases. A checked LOS
read rejects access errors and all-ones status instead of interpreting an
error as signal present. The existing 350 ms settling delay sleeps; the
no-signal path preserves the two one-millisecond waits. Initialized state is
published only after the final health check.

The selected `xpon_pma_init` caller now propagates parameter/mode errors and
checks final tuning status instead of unconditionally returning success. Its
error reaches `phy_mode_config` and the lifecycle's TX-disable/fault handling.
Tests exercise both LOS paths, every health/phase/delay failure, ownership
rejection and the actual prepared caller. The imported analog tuning values
are unchanged. Their electrical correctness, calibration convergence and
optical timing still require hardware acceptance; these local tests cannot
prove them. No device access occurred.

Validation for r60: all 79 PON/WAN host tests pass against the final prepared
sources, including both PMA tests and the locked MMIO fixture. The PHY UML
callback/lifetime suite and matching AN7581 core/vendor package build pass.
