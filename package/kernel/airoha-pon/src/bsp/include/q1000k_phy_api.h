/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PHY_API_H
#define _Q1000K_PHY_API_H

#include <linux/types.h>

struct xpon_phy_api_data_s;

/* One XGS burst profile shared by the MAC and optical PHY install owners. */
struct q1000k_pon_profile {
	u8 preamble[8], delimiter[8];
	u8 index, version, repeat, preamble_len, delimiter_len, fec;
};

static inline bool q1000k_pon_profile_valid(const struct q1000k_pon_profile *p)
{
	return p && p->index < 4 && p->version < 16 && p->fec < 2 &&
		p->repeat && p->preamble_len && p->preamble_len <= 8 &&
		p->delimiter_len && p->delimiter_len <= 8;
}

/* Process context only. Wait for active PHY callbacks; recursion from this
 * PHY's callback returns EDEADLK before taking any control lock. Symbols pin
 * phy_10g for the MAC's lifetime. PHY callbacks must only enqueue MAC events.
 */
int q1000k_phy_configure(u32 mode);
/* Lifecycle owner only, after native attachment and CPU/DMA pause. Select
 * XGS-PON from the known USXGMII boot handoff with verified controller TX off.
 * An already selected XGS-PON path is unchanged; unknown/active modes fail.
 */
int q1000k_phy_prepare_wan(void);
/* Lifecycle owner, before drain: 1 if cold/unconfigured, 0 if configured,
 * negative on invalid context/state. Does not change hardware.
 */
int q1000k_phy_needs_configure(void);
/* Bench mode can change only before configuration/IRQ ownership. It requires
 * the controller's immutable TX inhibit; an existing mode is idempotent.
 * IRQ/poll callbacks observe RX without invoking vendor registration events.
 */
/* Opt-in reacquisition: one attempt per module lifetime, after ten consecutive
 * polling observations of light without sync. Optional PLL restoration requires
 * reacquisition and stays under the same single-attempt budget. Optional OEM
 * RX gain uses the same budget and restores its saved bits on stop. Neither enables
 * registration or optical TX. The explicit OEM_RESET_REPEAT probe instead
 * permits six attempts at least five seconds apart; it latches sync, loss of
 * light after the first attempt, errors and shutdown until module removal.
 */
int q1000k_phy_set_rx_bench(bool enabled, bool reacquire, bool restore_pll, bool restore_gain);
enum q1000k_rx_probe {
	Q1000K_RX_PROBE_NONE,
	Q1000K_RX_PROBE_BIT_ORDER,
	Q1000K_RX_PROBE_DESCRAMBLER,
	Q1000K_RX_PROBE_FEC_OC,
	Q1000K_RX_PROBE_FEC_OFF,
	Q1000K_RX_PROBE_GAIN_AUTO,
	Q1000K_RX_PROBE_GAIN_LOW,
	Q1000K_RX_PROBE_TDC_DELAY,
	Q1000K_RX_PROBE_PLL_ORDER,
	Q1000K_RX_PROBE_OEM_ORDER,
	Q1000K_RX_PROBE_CHECKER,
	Q1000K_RX_PROBE_CDR_AUTO_RELEASE,
	Q1000K_RX_PROBE_CDR_INTERNAL_AUTO,
	Q1000K_RX_PROBE_PRCAL_FINALIZE,
	Q1000K_RX_PROBE_FLL_AUTO,
	Q1000K_RX_PROBE_RX_SEQUENCE_AUTO,
	Q1000K_RX_PROBE_POST_EYE_READY,
	Q1000K_RX_PROBE_OEM_CLOCK_CYCLE,
	Q1000K_RX_PROBE_OEM_RX_ACQUIRE,
	Q1000K_RX_PROBE_OEM_PEAKING,
	Q1000K_RX_PROBE_CHECKER_DARK,
	Q1000K_RX_PROBE_COMBINED_AUTO,
	Q1000K_RX_PROBE_PRCAL_RERUN,
	Q1000K_RX_PROBE_EYE_CURRENT,
	Q1000K_RX_PROBE_OEM_ANALOG,
	Q1000K_RX_PROBE_OEM_FULL_RESET,
	Q1000K_RX_PROBE_OEM_CAL_RESET,
	Q1000K_RX_PROBE_OEM_CAL_AUTO,
	Q1000K_RX_PROBE_OEM_EYE_0,
	Q1000K_RX_PROBE_OEM_EYE_1,
	Q1000K_RX_PROBE_OEM_EYE_2,
	Q1000K_RX_PROBE_OEM_EYE_3,
	Q1000K_RX_PROBE_OEM_EYE_4,
	Q1000K_RX_PROBE_OEM_EYE_5,
	Q1000K_RX_PROBE_OEM_EYE_6,
	Q1000K_RX_PROBE_OEM_EYE_7,
	Q1000K_RX_PROBE_OEM_POST_INIT,
	Q1000K_RX_PROBE_OEM_POST_CAL,
	Q1000K_RX_PROBE_OEM_RESET_REPEAT,
	Q1000K_RX_PROBE_COUNT,
};
#define Q1000K_RX_REPEAT_LIMIT 6
#define Q1000K_RX_REPEAT_INTERVAL_MS 5000
/* Select before configuration, with RX bench + recovery enabled.
 * Exclusive with the older gain/PLL flags. Never enables an optical or PRBS TX.
 */
int q1000k_phy_set_rx_probe(u32 probe);
/* A second bounded attribute keeps the original RX status below PAGE_SIZE. */
#include <q1000k_rx_diag.h>
struct q1000k_rx_diagnostics {
	u64 sampled_ms;
	u32 probe, attempts, writes;
#define Q1000K_DIAG_MEMBER(name, reg) u32 name;
	Q1000K_RX_DIAG_FIELDS(Q1000K_DIAG_MEMBER)
#undef Q1000K_DIAG_MEMBER
};
int q1000k_phy_rx_diagnostics(struct q1000k_rx_diagnostics *sample);
/* Raw receiver control/status words, not measured frequencies or proof of
 * firmware execution. Read without selecting probes or changing hardware.
 */
struct q1000k_rx_registers {
	u32 rx_control, pcs_reset, pma_reset, clock_control;
	u32 cdr_control, rx_frequency, pll_status, tdc_control;
	u32 rx_analog0, rx_analog1, rx_analog2;
	u32 rx_sequence_force, rx_sequence_disable;
	u32 rx_sequence_force0, rx_sequence_disable0, rx_lock_force, rx_lock_disable;
	u32 rx_oscal_control, rx_reset0, rx_reset1;
	u32 pll_power, pll_filter, pll_pcw1, pll_pcw2;
	u32 pll_force, pll_measure, pll_kband, pll_outputs;
	u32 rx_frontend_gain;
	u32 sfp_status;
	u32 sfp_polarity;
	u32 digital_status;
	u32 pcs_debug_control;
	u32 serdes_control;
	u32 rx_clock_divider;
	u32 rx_bus_width;
	u32 rx_input_control;
	u32 rx_cdr_ratio;
	u32 rx_rate_control;
	u32 rx_osr_control;
	u32 signal_control;
	u32 rx_equalizer;
	u32 rx_frontend_power;
};
struct q1000k_pcs_counters {
	u32 cw_start;
	u32 cw_end;
	u32 sof_to_mac;
	u32 eof_to_mac;
	u32 psync_mismatch;
	u32 sfc_hec_error;
	u32 pon_id_hec_error;
};
struct q1000k_rx_sample {
	bool rx_bench, tx_inhibited, tx_enabled;
	bool controller_los, phy_los, synced;
	bool reacquire_enabled;
	bool pll_restore_enabled, gain_restore_enabled;
	bool rx_power_valid;
	u32 rx_power_nw;
	u32 sync_status, frames, lof, fec_total, fec_corrected, fec_uncorrected;
	u32 irq_calls, poll_calls;
	u32 reacquire_attempts;
	u64 sampled_ms;
	struct q1000k_rx_registers receiver;
	struct q1000k_pcs_counters pcs_counters;
};
/* Fresh, read-only status/counter sample. No counter latches or clears. */
int q1000k_phy_rx_sample(struct q1000k_rx_sample *sample);
/* One callback-locked snapshot, available during RX-only and normal service. */
int q1000k_phy_snapshot(struct q1000k_rx_sample *sample,
			 struct q1000k_rx_diagnostics *diagnostics);
int q1000k_phy_last_snapshot(struct q1000k_rx_sample *sample,
			     struct q1000k_rx_diagnostics *diagnostics, int *fault);
int q1000k_phy_bench_recover(unsigned int action);
int q1000k_phy_rx_bench_recipe(unsigned int action);
int q1000k_phy_fast_sample(u32 *sfp, u32 *sync, u32 *frames);
/* Complete Q1000K receiver startup after MAC hardware release, with TX off. */
int q1000k_phy_receiver_startup(void);
int q1000k_phy_start(void);
int q1000k_phy_stop(void);
/* Lifecycle owner: wait for callbacks; rejects invocation by a callback. */
int q1000k_phy_quiesce(void);
/* Final coordinated optical TX enable/disable, after successful PHY start. */
int q1000k_phy_set_tx(bool enable);
/* Wait for callbacks and verify the controller before snapshotting TX. */
int q1000k_phy_get_tx(bool *enabled);
/* Read the controller's optical RX measurement while the PHY is active,
 * including RX-only discovery. ENODATA does not fault the optical stack.
 */
int q1000k_phy_get_rx_power(u32 *nanowatts);
/* The physical pipeline must have quiesced PHY callbacks and TX first. */
int q1000k_phy_profile_matches(const struct q1000k_pon_profile *profile);
int q1000k_phy_profile_set(const struct q1000k_pon_profile *profile);
int q1000k_phy_call(struct xpon_phy_api_data_s *data);
/* Timer callback only queues work; PHY polling runs in process context. */
void q1000k_phy_poll(void);
int q1000k_phy_init(void);
void q1000k_phy_exit(void);

/* Private to phy_10g: verify process context and callback mutex ownership. */
int q1000k_phy_callback_context(void);
int q1000k_phy_top_reset(void);
int q1000k_phy_pma_reset(void);
int q1000k_phy_pma_init(void);
int q1000k_phy_rx_reacquire(bool restore_pll, bool restore_gain);
int q1000k_phy_rx_cleanup(void);
int q1000k_phy_rx_probe(u32 probe);
int q1000k_phy_controller_oem_post(bool restore);
int q1000k_phy_rx_probe_cleanup(void);
u32 q1000k_phy_rx_probe_writes(void);
int q1000k_phy_controller_check(void);
int q1000k_phy_board_profile(void);
int q1000k_phy_trans_power(u32 operation);

#endif
