# Sirherobrine23 PON comparison for Q1000K

Audited 2026-09-16 on `q1000k-xgspon`. This is a source comparison and an
offline check against the unit's NAND inputs. No new optical hardware results
are claimed by the audit itself. The requested follow-up is now implemented in
the [58-case consolidated bench](XGSPON-REPEAT-RX.q1000k.md).

## Conclusion

The work provides useful evidence for **startup ordering and recovery while
light is present**, but the inspected code does not implement a complete
AN7581 XGS-PON receiver. Its working GPON examples cannot establish that its
initialization fixes apply unchanged to Q1000K.

The most useful additional experiment is a **bounded repeated acquisition
case**, using Q1000K/OEM-backed XGS reset operations. Our existing cases perform
one intervention per module lifetime; the reference retries recovery every
five seconds. The new `oem-reset-repeat` case compares up to six identical OEM
acquisitions against the existing zero/one-attempt controls. All 57 previously
prepared cases remain in the consolidated replacement image.

## Exactly what was examined

| Source | Pinned revision | Scope |
|---|---|---|
| User's OpenWrt `airoha_en7523` branch | `1cf599c745d65a83f6571e1dfbc7f5aeca3198ee` | Reconstructed selected PON files by applying its 337 numbered `930-*` kernel patches in order |
| Kernel default `airoha_en7523_all` branch | `2e2cf91fe84467d77649efebd99a28284f2124b3` | Read the MAC, PHY, optical controller, calibration loader and bindings |
| Kernel alternate `airoha_en7523` branch | `f6cc9b8abb6323f1f90b6d12bd7387bc39919026` | Tree inventory contains no Airoha PON MAC/PHY/controller implementation |

These are related implementations, not independent demonstrations of XGS
success. The three EN7572 C files reconstructed from OpenWrt are byte-identical
to the default kernel versions. The MAC, PHY and generic controller bridge
have a few differences, including additional TX power/rearm experiments in
OpenWrt. Those differences were inspected separately.

The associated [OpenWrt PR #20104](https://github.com/openwrt/openwrt/pull/20104)
describes working test use and remains a draft. Its listed examples and the
specific Nokia G-140W-F O5 claim concern GPON. O5 means completed PON activation;
it is stronger evidence than merely detecting optical power, but it concerns
a different SoC/optical frontend. The PR targets another branch name, so its
description was not substituted for inspection of the user's exact branch.

The audited public files, original patch authorship, SHA-256 hashes and the
offline compatibility results are retained in
`build-artifacts/q1000k-xgspon/sirherobrine-audit-20260916/manifest.json`.
No NAND, calibration contents or OEM binaries were copied into that snapshot.

## Support boundary: generic XGS names are not an XGS receive implementation

The [PHY mode setter](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/phy/airoha/phy-airoha-xpon.c#L561)
accepts GPON and EPON only. Its device matches are EN751221, EN7523 and EN7528;
there is no AN7581 XGS PHY match or corresponding 10G PMA initialization.
The [MAC binding](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/Documentation/devicetree/bindings/net/airoha,en7523-xpon.yaml#L86)
also limits the selected protocol to GPON/EPON.

The EN7572 optical-controller capability bitmap includes XGS-PON, but the
[shared frontend mode callback](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/airoha_lddla_core.c#L472)
rejects protocols other than GPON/EPON. Controller capability, generic xPON
enums, Ethernet support for a 10G SoC and an operational XGS receiver are
different claims.

## What they did differently and how it applies

| Finding | Reference behavior | Q1000K comparison | Consequence |
|---|---|---|---|
| MAC startup before synchronization | A trial that waited for PHY_READY was reverted after a tested EN7523/EN7571 design stayed in O1. PHY power-on is followed immediately by MAC preparation. | Our cold-start path already initializes the MAC and releases its transfer stops without waiting for RX synchronization. Registration execution and MAC interrupts remain held off. | Keep local startup dependencies in the audit, but the exact wait-before-MAC bug is absent. |
| Recovery after startup | When its digital PHY reports syncing and LOS is clear, delayed work pulses its PLL/counter reset every 5 seconds. | Our RX bench acknowledges/samples events and performs one selected intervention after ten eligible no-sync polls. It does not run that continuous vendor recovery worker. | New testable difference: bounded repeated XGS recovery within one module lifetime. |
| Configuration before reset release | EN7523 programs mode-dependent TDC, delimiter, transceiver and polarity values before its final PLL/counter reset. | Q1000K's prepared OEM cases test its own calibration, 7-bit versus 12-bit reset, L2D/eye/TDC and final-ready ordering. | Supports testing complete ordering; do not copy GPON register offsets into AN7581. |
| Signal-detect polarity | EN751221 previously inherited a polarity that falsely indicated LOS and prevented the conditional recovery worker. Board properties now select polarity before reset. | Q1000K's controller and PHY LOS indications both followed confirmed fiber removal/reconnection. | That specific permanent-false-LOS failure is strongly disfavored. It does not settle high-speed differential data polarity. |
| SoC clock/mux prerequisites | EN751221 needs ToD gates and WAN mux selection; the patch removes an inappropriate reset on that SoC. | Q1000K already runs JCPLL, TXPLL and digital/TX clock initialization while independently inhibiting the laser. Its exact OEM reset/clock cases are prepared. | Audit each AN7581 prerequisite against OEM evidence; this is not a license to write the EN751221 SCU values. |
| Controller startup | MCU hold, OCP/APD off, 100 ms delay, core reset, PM/DM/BOB loading and MCU release. | This basic sequence and BOB placement at DM `0x600` already exist, with complete memory readback in our loader. | No new missing basic loader stage was identified. |
| Extra controller calibration at probe | The generic EN7572 driver calls its adaptive TX-eye routine after MCU startup. | Our driver does not run that routine. It changes TX current, APC, TIA/PGA-labelled fields and loop/BEN controls; it is not a passive receiver-eye measurement. | Retain as an audit lead. Establish the receive relevance of each field before selecting an RX experiment. |
| Activation/datapath improvements | Upstream burst timing, laser power/rearm, GEM routing, QDMA and OMCI work enables registration and traffic. | Our failure precedes valid PHY framing and all seven observed PCS counters remain zero. | Useful later, but these do not yet explain our quiet physical receive path. |

Key primary references:

- [MAC start correction, including its reported test result](https://github.com/Sirherobrine23/openwrt/blob/1cf599c745d65a83f6571e1dfbc7f5aeca3198ee/target/linux/airoha/patches-6.18/930-92-airoha_en7523_all-net-airoha-start-GPON-MAC-before-PHY-synchronization.patch).
- [Conditional periodic recovery](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/phy/airoha/phy-airoha-xpon.c#L925).
- [Board polarity failure and fix](https://github.com/Sirherobrine23/openwrt/blob/1cf599c745d65a83f6571e1dfbc7f5aeca3198ee/target/linux/airoha/patches-6.18/930-279-airoha_en7523_all-phy-airoha-describe-the-transceiver-pin-conventions-.patch).
- [EN751221 ToD/mux/reset correction](https://github.com/Sirherobrine23/openwrt/blob/1cf599c745d65a83f6571e1dfbc7f5aeca3198ee/target/linux/airoha/patches-6.18/930-263-airoha_en7523_all-clk-en7523-Enable-xPON-ToD-clock-and-update-xPON-ini.patch).
- [Adaptive TX-eye writes](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_loop.c#L36).

### Checking our actual cold-start path

The applied Q1000K source calls `q1000k_omci_cold_start()` before the explicit
RX-bench skip of `q1000k_protocol_start()`. Cold start requests an O1 hardware
rebuild. That rebuild calls the MAC installation routines and
`q1000k_pipeline_activate_receive_only()`. Activation starts the PHY and then
calls `an7581_xpon_mac_stop(Q1000K_MAC_ALL_STOPS, false)` without a PHY_READY
condition. `q1000k_mac_cold_release()` releases the MAC's local reset beforehand.

Thus “registration is off” must not be described as “MAC initialization is
skipped.” Residual differences such as hardware activation state and event
side effects remain possible, but require a specific XGS dependency and a
controlled test. The imported GPON success does not identify that dependency.

## Direct DSD/firmware compatibility check

The actual unit record at NAND offset `0x412000` is the same 513-byte input used
by our bench: SHA-256
`f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c`.

| Input | Reference assumption | Actual Q1000K result |
|---|---|---|
| Calibration signature | Generic BOB importer requires an Airoha signature at byte `0x94` in one of two word orders. | That word is `0xffffffff`; neither order passes. The importer returns `-EINVAL`. |
| MCU PM | Request helper requires at least 16,384 bytes. | OEM file is 15,232 bytes; rejected unchanged. |
| MCU DM | Request helper requires at least 4,096 bytes. | OEM file is 56 bytes; rejected unchanged. |
| MCU transfer | Reference streams words without full memory verification. | Our loader pads to full capacity, overlays the 512 calibration bytes, and verifies all PM/DM bytes before starting the MCU. |

The [reference firmware length checks](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/optical/airoha/en7572_main.c#L203)
and generic signature checks are incompatible with supplying our original OEM
inputs directly. This is not evidence that the DSD record is corrupt. Our
board-specific OEM loader reads its raw calibration data without that generic
signature requirement. Nor does the reference supply evidence for byte-swapping
our DSD: a failed signature test cannot determine an alternative byte order.

The reference's adaptive TX-eye routine is also present in the NAND OEM
`en7572.ko`, but its direct calls in that module are through the API dispatcher
and debug command, not `init_module()`. Other modules may invoke the API;
their runtime call history has not been proved. Register names mentioning TIA
or PGA alone do not establish a downstream RX function in this TX-control
routine.

Neither reference EN7572 implementation writes the Q1000K OEM post-init
`0x110[8]` bit or supplies the `0x110/0x114` receive-output profiles. Our
`oem-post-init`, `oem-post-cal` and output-profile combinations remain more
direct Q1000K evidence. See the [deep receiver audit](XGSPON-DEEP-RX-AUDIT.q1000k.md).

## Does this change the TX-before-RX hypothesis?

It strengthens **local initialization coupling** as something to inspect,
not the theory that AT&T withholds downstream synchronization until this ONU
transmits. In [ITU-T G.9807.1, C.12.1.2–C.12.1.4](https://www.itu.int/epublications/ar/publication/itu-t-g-9807-1-2023-02-10-gigabit-capable-symmetric-passive-optical-network-xgs-pon),
the ONU first synchronizes and learns profiles with its transmitter off;
transmitting discovery responses follows that phase. This establishes protocol
ordering, not a measurement of the user's particular OLT.

Their MAC-start change alters several local conditions together and its end
result includes normal transmission. It does not isolate optical TX as the
cause of obtaining PHY_READY. Preserve the distinction between a local block
needing initialization and an OLT needing a prior message.

## Test priority and image budget

1. **Use the consolidated replacement described in the [repeat plan](XGSPON-REPEAT-RX.q1000k.md).** Prioritize
   `oem-post-init`, `oem-post-cal`, the fresh eye/calibration/full-reset cases
   and the missing passive reconnect control. These were prepared previously;
   this comparison supplies no new hardware outcome for them.
2. **Run the added `oem-reset-repeat` case in that same image.**
   Compare zero, one and a small fixed number of identical OEM-backed XGS
   acquisition attempts at recorded intervals in one module lifetime. Stop
   immediately on synchronization, loss of light or a controller/error guard.
   Record PCS counters before every intervention because a reset may clear
   them. Repeating module unload/load is not the same experiment.
3. **Retain a startup dependency audit.** Compare AN7581 OEM MAC activation,
   receive gates and clock/mux fields with the current cold-start image. Add
   a case only for an identified missing operation. Do not represent the
   existing generic MAC initialization as absent.
4. **Audit adaptive-controller fields before proposing writes.** Obtain OEM
   call-site/field evidence and determine which, if any, affect downstream
   reception. The whole TX-eye routine is not an RX experiment.

The audit itself required no image. The user subsequently requested the added
experiment in the new bench, so it is included in one consolidated replacement.
No community driver code was imported into Q1000K's runtime by this audit.
