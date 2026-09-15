# Q1000K controller / electrical RX audit v2

Date: 2026-09-15. Sources: current q1000k-xgspon tree; local QKX001-06.00.44.00 `en7572.ko.dis`, `phy_10g.ko.dis`, `xponconfig`; pinned public EN7572 LDDLA source `950199a8de6b75e76906a7c1b39b7a9a3e2913f9`, checked out in `/tmp/q1000k-lddla-controller-audit`.

## Highest-value new electrical frontend experiment

The pinned source's `v2/lddla/en7572_cmd.c` has an explicitly named `SetRxPreEmphasis()` and `GetRxPreEmphasis()` (lines 2027–2068). This supplies a documented controller **RX electrical output** experiment: change the limiting amplifier output swing/pre-emphasis feeding the AN7581 receiver. It is separate from optical laser TX.

Its `RX_PE_LUT` at lines 55–119 maps DC swing (mV), boost (dB), and five register fields. `BitWrite()` sends little-endian 32-bit RMW words to I2C A2 / 0x51. Exact fields:

| register | field | mask | LUT column |
|---|---|---|---|
| 0x0114 | [13:8] | 0x00003f00 | 2 |
| 0x0114 | [20:16] | 0x001f0000 | 3 |
| 0x0114 | [29:24] | 0x3f000000 | 4 |
| 0x0114 | [3] | 0x00000008 | 5 |
| 0x0110 | [6] | 0x00000040 | 6 |

Full 0x114 mask = 0x3f1f3f08. Retain source order: four individual masked writes at 0x114, then bit6 at 0x110. Check TX inhibited and disabled before, between, and after writes. Save original two words before first mutation, restore only masks in reverse order after each trial; immediate readback each write, stop/contain on mismatch/error. No arbitrary register interface.

Three bounded source-table candidates, which span useful behavior without a blind 63-setting sweep:

| name | swing | boost | field tuple (13:8,20:16,29:24,3,110:6) | combined 114 masked value | 110[6] |
|---|---:|---:|---|---|---|
| frontend-400-flat | 400 mV | 0 dB | 20,0,20,0,1 | 0x14001400 | 0x40 |
| frontend-600-flat | 600 mV | 0 dB | 30,0,30,0,1 | 0x1e001e00 | 0x40 |
| frontend-600-boost | 600 mV | 2 dB | 50,8,54,1,0 | 0x36083208 | 0x00 |

Record baseline words and decoded match before setting candidates. A candidate that improves actual frame sync/counters is useful evidence; successful readback alone says only that settings applied. The OEM binary does not include the optional RXPE diagnostic, so this is a family-source experiment, not a known OEM Q1000K default. Never copy its `i<64` loop over 63 elements.

## Current versus OEM controller deltas

| area | native current | local OEM evidence | bench action |
|---|---|---|---|
| MD32 cfg/address/control bank | 0x51; data ports 0x50 | `Write_data_MD32`: PM cfg/address at 0x2ac–0x2e8, DM cfg/address 0x388–0x3c4; all use w2=0x50. `init_module` stop/start 0xad8–0xaec and 0xbbc–0xbd0 also A0 | Explicit complete OEM-A0 loader/start profile, retaining identical PM/DM/cal bytes and TX-disable policy. Do not rewrite baseline silently |
| Ordinary control RMW | 0x51 | `writeByBit` 0x1de0/0x1e90 uses A2 | Keep A2 for APD/OCP/reset/TX/analog fields |
| MCU stop/reset ordering | stop MCU, clear OCP 160[30], APD 15c[8], 100ms, 200[31:30]=0 then 3 | `init_module` 0xac8–0xb74 same sequence | Already implemented; audit readback and explicit bank choice |
| PM/DM capacities | 16KiB PM, 4KiB DM, zero pad; cal512 at DM600 | OEM same 4096/1024 word loops; BOB 128 words at DM600 | Preserve current full verification and hashes |
| BOB vendor text | preserve unit bytes | `Write_Vend_data` patches 20..35 to ECONET and 40..55 to EN7572 | No need to alter: qphy_board_profile explicitly chooses OEM profile82 / three exact PHY words independently of SFP vendor strings |
| Alarm clear before MCU start | omitted | `ClearWarningAlarmFlag` zeros alarm/warning table words | Not a demonstrated RX gate; capture status, avoid erasing evidence merely to mimic OEM |
| Post-PHY controller tweak | omitted | xponconfig XGS branch writes A2 0x110[8]=1 after xpon_10g.ko | Record raw 0x110. Bit semantics unresolved; not justified as receive-only just because neighboring bit6 is RXPE |
| Electrical RX output shaping | no explicit frontend probe | family `SetRxPreEmphasis` provides exact masks/table | Add three bounded source-table candidates above |
| Optical sensor health | power + 7 words | OEM has raw RSSI, live APD voltage, overcurrent status, temperature | Add reads below to distinguish sensor responsiveness / OCP / analog bias from high-speed receive |

Both A0 and A2 MCU-enable views were already 1 on hardware (XGSPON-BENCH ~1227). This lowers confidence that bank choice alone explains the failure. An OEM-A0 variant tests alias assumptions; it is not a diagnosed fix. Native verified readback does not independently demonstrate electrical high-speed output.

## Concrete new read-only analog observations

All at A2 0x51 unless stated. Exact per-register widths matter; keep raw values in report.

| address/width | meaning / conversion | primary evidence |
|---|---|---|
| 0x110 / 4 LE | raw electrical-output control; bit6 RXPE; bit8 OEM-only tweak | public SetRxPreEmphasis; OEM xponconfig |
| 0x114 / 4 LE | electrical-output swing/pre-emphasis fields | public GetRxPreEmphasis |
| 0x0060 / 2 BE signed | temperature in 1/256 C | OEM bob_info 36f8 onwards; bosa_info 3d98 |
| 0x0062 / 2 BE | Vcc in 100uV units | OEM bob_info 37c4 onwards |
| 0x0083 / 1 | MCU_IDLE, record raw (not heartbeat) | pinned en7572.h |
| 0x00ae / 2 LE | live VAPD in 1/8 V | OEM bosa_info 3e0c, public bosa_info |
| 0x00f2 / 2 LE | raw RSSI_ADC, no units assumed | OEM ddmi_rx 20c8; pinned header RSSI_ADC |
| 0x00f4 / 1 | limiting-amplifier input impedance field, raw | pinned header LA_IN_IMP |
| 0x00f5 / 1 | limiting-amplifier drive impedance field, raw | pinned header LA_DRV_IMP |
| 0x00f6 / 2 LE | RSSI current: integer uA = raw >>5 | OEM bosa_info 3ea0; public header RSSI_CURRENT |
| 0x00f8 / 1 | AGC_DBG, raw | pinned header |
| 0x00f9 / 1 | CHKSUM_ERR, raw flag | pinned header |
| 0x00fa / 1 | controller LOS_STA | OEM bob_info 36ac; pinned header |
| 0x00fc / 1 | APD_HW_DBG, raw | pinned header |
| 0x00fd / 1 | APD_LUT_DBG, raw | pinned header |
| 0x00ff / 1 | RXP_DBG, raw | pinned header |
| 0x03e4 / 4 LE | bit8 OCP detected | OEM bosa_info 3eec–3f08 and text at .rodata.str1.8+b20 |

At minimum include 110,114,AE,F2,F6,3E4; raw debug bytes are secondary. No table dumps are necessary and no factory calibration contents need be exposed. The MCU's own state/debug fields are observations, not proof of liveness unless changing consistently with an external event.

## AN7581 high-speed receiver audit boundaries

The native adapted PHY already executes full imported `xpon_init`, `fiber_plug_reset(FIRST_PLUG_IN)`, RX preset/on, OS/pical/pdos/feos/sdcal, L2R/L2D and ready sequence. This differs from the GPON PR's initially omitted full receiver sequence. Do not claim we found the same omission.

Source `en7581_pma.c`: RX preset ~1528; RX on ~1590; OSCal ~1736; pical ~1767; pdos ~1813; feos ~1900; sdcal ~1945; calibration cleanup ~1979–1990. Complete audit should compare ordinary receiver-power force bits against calibration-only fields; many forces are intentional. E.g. final SD cleanup explicitly keeps force selectors 1 and sets oscal clock/reset/enable values 0, so blanket clearing every force bit is not equivalent to OEM.

Key readbacks beyond existing RX2ANA words: rx_fe_pwdb force 0xB894; rx_sigdet_pwdb/scan 0xB84C; rx_oscal_en 0xB840; rx_oscal_ckon 0xB83C; RX sequence force/disb pairs and RX_DISB_MODE1/2 calibration DAC routing. Exact PMA offsets should be taken from en7581_reg.h during implementation (names above are source anchors; numerical positions require recheck).

## Firmware integration recommendation

One firmware exposes fixed trial IDs, with collector running fresh baseline, each PHY-priority trial, each frontend candidate, OEM-A0 controller initialization, and selected combined best-evidence profile. Preserve per-trial input mode, before/applied/restored words, sensor data, CDR/FIFO/frequency/lock words, and real frame/FEC/LOF counters. Every mutation profile must restore or power off, and retain immutable optical TX inhibition and disabled registration. The output is one collection archive containing trial results plus cleanup evidence.

Software can test electrical output settings and initialization alternatives; it cannot prove actual optical modulation or an electrical eye is present without external measurement. All candidate settings remain experimental until measured on the Q1000K hardware.
