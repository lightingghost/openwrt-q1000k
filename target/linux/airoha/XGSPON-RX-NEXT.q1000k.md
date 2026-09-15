# Q1000K: remaining receive hypotheses and next bench

Prepared 2026-09-15 for `q1000k-xgspon`. Hardware execution of this new
collection is pending. The historical results are in
[the hypothesis log](XGSPON-RX-HYPOTHESES.q1000k.md) and
[the bench record](XGSPON-BENCH.q1000k.md).

## What the evidence supports

The previous image completed 13 receive cases and physical controls, 1,260
samples in total. Connected power was approximately -18.7 to -18.15 dBm;
both LOS indications, the power sensor and a PHY interrupt responded to
removing the fiber. Nevertheless, synchronization, frame/FEC counts and all
seven additional PCS counters remained zero. Gain, bit order, descrambling,
FEC encodings and the documented recovery/order variants were insufficient
fixes. This localizes the investigation before registration; ONT identity,
OMCI, DHCP and VLAN changes do not address the observed failure.

These results do not prove that every analog setting is correct, that the
SoC receives usable high-speed data, or that its recovered clock operates.
The checker finished and latched; its unchanged result in darkness cannot
serve as a continuous activity measurement. The previous live capture ended
before reconnection, leaving that transition unmeasured.

Offline reanalysis of all 15 saved captures confirms the 1,260-sample count.
In the live-dark control, `rx_analog2` changed from `0x21b` to `0x1b` for
the first dark sample (142), then returned to `0x21b` at sample 143 while
remaining dark. This is a one-sample FIFO-empty-field transient (2 → 0 → 2),
not a persistent light/data distinction. The FIFO-full field stayed zero.

## Reference PR

[OpenWrt PR #24577](https://github.com/openwrt/openwrt/pull/24577), reviewed
again at head `d7569c5e26551084e7643b0e83ecda9c31f49f11`, targets EN7528
with EN7571 and reports GPON service. Q1000K uses AN7581 and EN7573 with
XGS-PON. Its useful lesson is to verify the optical controller and the SoC
receive/CDR path separately. Its GPON register addresses, rate settings and
laser defaults cannot establish the correct Q1000K sequence. See
[the reuse review](XGSPON-PR24577.q1000k.md) for component provenance.

In the [pinned PHY source](https://github.com/AKoo7/openwrt/blob/d7569c5e26551084e7643b0e83ecda9c31f49f11/package/kernel/econet-xpon/src/phy/src/phy_init.c),
`en7571_optical_bringup()` is separate from `en7571_reg_init_full()` and
`gpon_do_lock()`; `gpon_mbi_start_probe()` comes after CDR lock. This supports
investigating reception before MAC delivery. Comments contain conflicting
clock/data and laser-servo descriptions, and `tx_laser_off` actually defaults
to zero despite safe-default comments. These are exploratory implementation
notes, not EN7573 register documentation.

The local AN7581 code comes from the preserved
[10G driver import](https://github.com/coolsnowwolf/lede/commit/f7fd86eaa58c29fed97da04ab219c74a835a9358).
The [separate MD32 loader reference](https://github.com/Sirherobrine23/airoha_xpon_en757x/tree/950199a8de6b75e76906a7c1b39b7a9a3e2913f9/v2/lddla)
and the same unit's OEM disassembly provide controller evidence. No examined
source establishes an independent EN7573 high-speed-output-ready indicator.

## Remaining hypotheses, ranked by useful next evidence

| Priority / hypothesis | Test and comparison | Evidence that narrows it; limitation |
| --- | --- | --- |
| 1. CDR/shared clock or receive state machine never becomes operational | Capture the raw TDC NCPO word and passive FIFO clock status alongside existing frequency monitors, force controls and RX2ANA words across connected, dark and live reconnected phases. Repeat a baseline after an independent cold RAM boot if the first capture stays quiet. | A reproducible change tied to light, or acquisition only after live insertion/cold boot, identifies a state/sequence dependency. No change is inconclusive; none of these raw words alone is an independent lock or calibrated-rate measurement. |
| 2. Controller output, electrical RX route or differential orientation is wrong | Compare the same board's OEM and bench receiver controls; establish the actual EN7573-to-AN7581 route from schematic or verified traces, then measure differential output and SoC input with suitable equipment. | Signal at controller output but absent at SoC input points to route/connection; present signal with failed recovery points toward input/clock/quality. Test points and a polarity-control bit are not established, so software cannot complete this test. |
| 3. Frontend calibration/equalization or optical quality is wrong | Decode the already captured RX DAC/offset fields; compare per phase and between cold boots. Compare OEM readings and a known-good board/optical path where available. | Stable sensor power does not establish a usable electrical eye or valid factory calibration. Existing gain trials only exclude those candidates as sufficient fixes. No calibration write or analog sweep is part of the collector. |
| 4. PCS is receiving unsuitable data/packing or reporting the wrong boundary | Correlate analog/clock changes with PSync, HEC, codeword and SOF/EOF counters, using existing full raw snapshots. | New error/codeword activity would move the fault boundary toward PCS configuration. Continuing zero counters leaves an earlier failure open; previously failed bit/FEC/descrambler trials do not eliminate combinations. |
| 5. Live reinsertion handling fails | Observe connected → dark → reconnected in one loaded lifetime, without a selected recovery/probe. Record both LOS sources, IRQ/poll counters and physical confirmations. | Polling sees reconnected light but no new IRQ: investigate notification. Light returns but PCS remains quiet: notification alone is insufficient. The bench's polling performs its existing public insertion handling; this is not a frozen-register experiment. |
| 6. Wrong external wavelength/rate or inaccurate mean-power measurement | Record working gateway mode, exact optical module, RX level/units and timestamp on the same fiber. Use a wavelength-selective calibrated meter or appropriate signal measurement if needed. | Previously identified XGS-PON equipment lowers this priority. Mean power and LOS cannot prove wavelength or modulation quality, and the prior gateway -17 dBm reading was not simultaneous. |
| 7. OEM initialization needs an additional documented step | Compare OEM controller startup and complete clock/reset paths against the public AN7581 sequence. Recover semantics for the five extra OEM reset fields before proposing a write. | A documented missing field/ordering difference justifies one bounded follow-up test. The restricted OEM-order experiment already failed; unknown reset fields remain evidence to research, not values to replay. |

## Firmware instrumentation

The next image adds two passive diagnostic words:

- `tdc_ncpo`: `EN7581_XPON_PMA_SS_LCPLL_TDC_RO_4`, physical
  `0x1fa8b05c`. The public `en7581_xgpon_phy_event_poll` reads it as NCPO
  without a preceding debug selector/latch write. Record raw values and
  within-phase variation; no invented conversion to MHz or lock state.
- `fifo_clock_status`: `EN7581_XPON_PMA_FIFO_CK_STATUS`, physical
  `0x1fa8b59c`. It is read by the public FIFO diagnostic, which also changes
  a debug control. This bench only reads it; its passive result may be stale
  or require a latch. It cannot prove clock presence or absence.

The existing `receiver.rx_analog0/1/2` already contain RX DAC/offset and
FIFO-full/empty/PI-calibration fields. Their documented bit fields can be
decoded offline; no new analog-control write is needed. Four-bit FIFO fields
are reported as raw observations, not reliable cumulative traffic counters.
Diagnostic schema 2 identifies the extra words; offline readers retain
schema 1 support for the historical captures.

All decoded analog fields are passive snapshots with unproven freshness:
the public eye/FIFO routines perform debug/latch writes before reading them.
The collector does not perform those writes. It neither runs an eye scan nor
labels passive DAC, PI or FIFO values as live input activity.

A freshly armed checker in confirmed darkness versus light is a possible
later experiment, not a case implemented here. Existing checker arming is
light-gated and requires completed PMA initialization. Such a test would need
explicit bounded dark arming after initialization, unchanged pattern/length,
generator/loopback-off checks, and before/after register snapshots. LOS-driven
reconfiguration would confound its interpretation. A completed checker in
both conditions could discredit it as a light detector; even a difference
would not establish valid XGS-PON data or BER.

## Single-script collection plan

The delivered `q1000k-rx-collect.py` embeds its readers and the exact image's
runtime hashes. Run it from a Linux host with Python 3, SSH access to
`192.168.255.1`, the unit's verified private firmware/calibration input tar,
and either an active serial log or a serial device captured by the script
at 115200 baud. The host management address is
`192.168.255.2/24`; DHCP is disabled in the bench image.

1. RAM-boot the supplied FIT using the existing second-stage http-uboot
   procedure. The script does not boot or flash the device.
2. Start the collector and confirm that fiber is connected. It verifies
   image revision, runtime hashes, idle ownership, RAM root and TX inhibit.
3. Record a 30-sample baseline, then check normal teardown and input cleanup.
4. Record one 180-sample live control. The script prompts for removal after
   an observed light window, and for reconnection after an observed dark
   window. Confirm each physical action when complete. It requires observed
   light, dark and reconnected light windows; time passing is not confirmation.
5. Finish normal teardown, verify cleanup, and save raw evidence, phase
   reports, confirmations and a single compressed results archive. If a
   transition, confirmation or cleanup is missing, record incomplete/stopped
   collection and preserve the evidence. Do not automatically retry.
6. If the result remains quiet, use `--baseline-only --fiber-connected` after
   a separately performed cold RAM boot, with a new output directory. Keep
   the first bundle for comparison. Obtain external measurements for the
   hypotheses the software cannot distinguish.

“Collection complete” means the observations and cleanup are complete. It
does not mean downstream synchronization, O5 registration or internet service.
User-confirmation timestamps are human markers; sampled state transitions
bound observation timing, not exact optical or interrupt latency.

The bench retains immutable TX inhibit, NAND disabled, no optical service
startup, and the existing checked module lifecycle. No device access is
required to research, build or validate these deliverables.

## Verification and delivery

The artifact directory contains the firmware, exact build checkpoint,
runtime hashes, image inspection and host-test results. The standalone
collector's dry-run is tested outside the repository. Offline fixtures cover
schema compatibility, malformed diagnostics, ordered physical transitions,
partial collection, cancellation and private-input exclusion from bundles.
Build identity and final commands are recorded in the delivered `README.md`.
