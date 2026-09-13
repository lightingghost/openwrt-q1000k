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
