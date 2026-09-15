# http-uboot findings from Q1000K XGS-PON bring-up

Recorded 2026-09-14 on `q1000k-xgspon`. This is a follow-up note, not a
bootloader change or a request to flash. The local `http-uboot-q1000k` checkout
is at `527bdbe3b0b9b686fdb5d9253f0ff7594180c869` and has unrelated uncommitted
work. That work has not been modified. The exact running bootloader binary
has not been matched to this commit.

## PON WAN selector written during copper Ethernet initialization

**Confirmed observation:** the RAM bench's MAC resource provider reads
SCU WAN selector `0x12`, top-level PON MAC reset deasserted, and local MAC reset
and stop words both zero. The first MPI RX stop request reads back zero;
CPU/DMA pause has completed but no FE channel has been retired. See
`build-artifacts/q1000k-xgspon/bench-ef676230c8/stack-r66-01/` for the capture.

**Source evidence:** in the local bootloader's `drivers/net/airoha_eth.c`,
`airoha_hw_init()` at lines 6806–6810 writes both the Ethernet XSI selector
and `SCU_WAN_CONF[7:0] = 0x12` whenever `!eth->gdm4_usb_hsgmii`. The symbol is
`SCU_WAN_SEL_USXGMII` at line 68. This condition does not require a PON PCS
consumer. The separate bootloader `drivers/net/airoha/pcs-airoha-common.c`
and Linux `drivers/net/pcs/airoha/pcs-airoha-common.c` distinguish the two:
Ethernet PCS setup updates the Ethernet XSI selector, while PON PCS setup
updates the PON XSI and WAN selectors. Q1000K's 10 GbE copper port uses
`eth_pcs`/GDM4; its optical port uses the separate PON path/GDM2.

**Proposed bootloader follow-up:** audit and restrict the legacy WAN selector
write to a configuration that actually uses the PON lane for Ethernet.
Copper-only HTTP recovery should preserve the optical lane's selector and
reset state unless a documented board requirement needs otherwise. The
current write is a likely unnecessary side effect for Q1000K, supported by
the separate PCS implementations; validate this with both copper ports before
calling a modified bootloader correct. Do not simply replace `0x12` with
XGS-PON `0x0a` in the bootloader.

**Linux responsibility:** tolerate already deployed bootloaders. The PON
lifecycle must own the transition to XGS-PON after acquiring the native port,
pausing CPU/DMA admission and verifying controller TX is off. It must not
assume OEM firmware left the lane in XGS-PON mode. Loading EN7573 firmware,
calibration, optical activation, MAC/PHY reset sequencing and OMCI remain
Linux responsibilities. A bootloader change alone would mask this missing
cold-start handling.

**Validation for a future bootloader change:** RAM-load a candidate using the
existing two-stage chain, exercise HTTP/TFTP and both copper LAN ports, then
boot a NAND-disabled, TX-inhibited Linux bench. Compare WAN/XSI/reset state
and confirm the Linux PON initialization owns its own setup. Do not modify
flash, persistent environment or factory calibration for this investigation.

## Bench boot arguments and panic timeout

The f885e80846 FIT contains DT `panic=0`, but the running command line is
`console=ttyS0,115200 earlycon root=/dev/ram0 rdinit=/init`. Boot argument
replacement therefore prevents relying on the FIT's DT value alone. OpenWrt
also subsequently sets `kernel.panic=3` from `10-default.conf`.

This is not established as a bootloader bug: supplying boot arguments is
normal bootloader behavior. A future bench-specific RAM boot command can
include `panic=0` explicitly, without saving environment changes. Bench r4
also installs a late userspace sysctl override and the test helper verifies
the live value. Those Linux checks are still necessary even if the bootloader
command is improved. Hardware watchdog behavior is a separate question.

## Keep the bootloader's scope narrow

No evidence currently justifies having http-uboot initialize the optical
controller, read/apply optical calibration, train the PHY, register an ONU,
or enable laser transmission. Its role in these tests is RAM loading and a
predictable peripheral handoff. Any further bootloader issues found during
bring-up should be appended here with observed state, source location,
proposed ownership and a separate validation plan.
