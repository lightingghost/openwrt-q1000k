// SPDX-License-Identifier: GPL-2.0-only
/* Bounded RX-only experiments. No raw register access is exposed to users. */
#include <linux/bits.h>
#include <q1000k_trace.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <an7581_pon_phy.h>
#include <q1000k_phy_api.h>
#include "phy_global.h"
#include "phy_reg.h"
#include "en7581_reg.h"

struct qprobe_step { u32 reg, end, start, value, delay_us; };
#include "q1000k_phy_probe_steps.h"
#include "q1000k_phy_probe_oem_steps.h"
#include "q1000k_phy_probe_deep_steps.h"

/* Save distinct fields before their first update, including partial failures.
 * Replay originals in reverse order after callbacks drain, with TX still off.
 */
static struct { u32 reg, end, start, value; } saved[256];
static unsigned int saved_count;
static u32 probe_writes;
static bool post_pending;
static u32 repeat_calls;
static bool repeat_failed, probe_closed;
static int probe_prcal_finalize(void);

static int probe_ready(void)
{
	int ret = q1000k_phy_callback_context();

	if (ret)
		return ret;
	if (!gpPhyPriv || gpPhyPriv->wan_sel != SCU_WAN_CONF_REG_WAN_SEL_XGSPON)
		return -EINVAL;
	if (gpPhyPriv->trans_tx_status != PHY_DISABLE || gpPhyPriv->phyCfg.flags.txPowerEnFlag)
		return -EACCES;
	ret = an7581_pon_phy_status();
	return ret ?: q1000k_phy_controller_check();
}

static int probe_read(u32 reg, u32 *value)
{
	int ret = probe_ready();

	if (!ret)
		ret = an7581_pon_phy_read(reg, value);
	return ret ?: (*value == ~0U ? -EIO : 0);
}

static int probe_write(u32 reg, u32 end, u32 start, u32 value, bool save)
{
	u32 old = 0, actual, mask = GENMASK(end, start);
	unsigned int i;
	int ret = probe_read(reg, &old);

	if (ret)
		return ret;
	if (reg == EN7581_XGPON_PHY_DBG_CTRL && (old & BIT(16)))
		return -EACCES; /* Never replay the counter-clear strobe. */
	if (save) {
		for (i = 0; i < saved_count; i++)
			if (saved[i].reg == reg && saved[i].end == end && saved[i].start == start)
				break;
		if (i == saved_count) {
			if (i == ARRAY_SIZE(saved))
				return -E2BIG;
			saved[i].reg = reg;
			saved[i].end = end;
			saved[i].start = start;
			saved[i].value = (old & mask) >> start;
			saved_count++;
		}
	}
	ret = an7581_pon_phy_update(reg, end, start, value);
	if (!ret) {
		probe_writes++;
		ret = an7581_pon_phy_read(reg, &actual);
	}
	/* Also require unrelated bits to survive the masked update. */
	if (!ret && actual != ((old & ~mask) | ((value << start) & mask)))
		ret = -EIO;
	return ret ?: probe_ready();
}

static int probe_steps(const struct qprobe_step *steps, unsigned int count,
		       bool long_tdc, bool reset_delay)
{
	unsigned int i;
	int ret;

	q1000k_trace(QT_RECOVERY_PHASE, 0, 0, count, probe_writes, long_tdc, reset_delay);
	for (i = 0; i < count; i++) {
		ret = probe_ready();
		if (ret)
			return ret;
		if (steps[i].delay_us) {
			/* Sleepable callback mutex; preserve minimum settling time. */
			unsigned int delay = steps[i].delay_us;

			if (long_tdc && delay == 500)
				delay = 5000;
			if (delay >= 1000)
				usleep_range(delay, delay + 100);
			else
				udelay(delay);
		} else {
			ret = probe_write(steps[i].reg, steps[i].end, steps[i].start,
					  steps[i].value, true);
			if (ret)
				return ret;
		}
		if (reset_delay && i == 2)
			udelay(500);
	}
	if (reset_delay)
		udelay(100);
	return probe_ready();
}
#define RUN(steps) do { ret = probe_steps(steps, ARRAY_SIZE(steps), false, false); if (ret) return ret; } while (0)

static int probe_recover(u32 mode)
{
	int ret;

	/* Reference out path, with each update checked. Never use SCU reset. */
	RUN(xpon_tdc_off);
	RUN(xpon_rx_l2r);
	RUN(xpon_fll_reset);
	RUN(xpon_dig_reset_hold);
	if (mode == Q1000K_RX_PROBE_OEM_ORDER) {
		/* OEM releases digital reset before L2D/TDC. Only the seven
		 * documented reset bits are used, not its unknown upper bits.
		 */
		ret = probe_steps(xpon_dig_reset_release, ARRAY_SIZE(xpon_dig_reset_release), false, true);
		if (ret) return ret;
	}
	RUN(xpon_rx_l2d);
	ret = probe_steps(xpon_tdc_on,
		ARRAY_SIZE(xpon_tdc_on) - (mode == Q1000K_RX_PROBE_OEM_ORDER ? 4 : 0),
		mode == Q1000K_RX_PROBE_TDC_DELAY || mode == Q1000K_RX_PROBE_OEM_ORDER, false);
	if (ret) return ret;
	if (mode == Q1000K_RX_PROBE_PLL_ORDER || mode == Q1000K_RX_PROBE_OEM_ORDER)
		RUN(txpll_on);
	if (mode != Q1000K_RX_PROBE_OEM_ORDER)
		RUN(xpon_dig_reset_release);
	RUN(xpon_rx_rxrdy);
	return probe_ready();
}

static int probe_oem_clock_cycle_reset(bool full)
{
	int ret;

	RUN(oem_tdc_off);
	RUN(oem_rx_l2r);
	if (full) {
		/* Exact twelve-bit reset from both Q1000K OEM binaries. Upper
		 * five field meanings remain unknown; isolated from the legacy
		 * seven-bit experiment. Optical TX is independently inhibited
		 * at the controller, checked around every register update.
		 */
		RUN(oem_full_reset);
	} else {
		RUN(oem_known_reset);
	}
	RUN(oem_rx_l2d);
	/* OEM XPON_TDC_on @0x28a94 shares this public prefix, with 5ms
	 * final settling and no final forced LPF reset. No FLL reset.
	 */
	ret = probe_steps(xpon_tdc_on, ARRAY_SIZE(xpon_tdc_on) - 4, true, false);
	if (ret)
		return ret;
	RUN(txpll_on);
	RUN(oem_rx_ready);
	return probe_ready();
}

static int probe_oem_clock_cycle(void)
{
	return probe_oem_clock_cycle_reset(false);
}

/* A fresh, finite eye observation. Never call the vendor readout helper:
 * it silently retries LPF reset on a failed measurement. Retain raw words
 * and completion flags even when no opening was found. This is not BER.
 */
static int probe_eye(void)
{
	u32 pi, done, ready, horizontal, vertical, dac0, dac1;
	int ret;

	ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 0, 0, 1, true);
	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 8, 8, 1, true);
	if (ret)
		return ret;
	RUN(oem_eye_setting_xgs);
	RUN(oem_xpon_eye_cal);
	RUN(oem_eye_start);
	RUN(oem_eye_latch);
#define EYE_READ(reg, value) do { ret = probe_read(reg, &value); if (ret) return ret; } while (0)
	EYE_READ(EN7581_XPON_PMA_RX_TORGS_DEBUG_2, pi);
	EYE_READ(EN7581_XPON_PMA_RX_TORGS_DEBUG_9, done);
	EYE_READ(EN7581_XPON_PMA_RX_TORGS_DEBUG_5, ready);
	EYE_READ(EN7581_XPON_PMA_RX_TORGS_DEBUG_10, horizontal);
	EYE_READ(EN7581_XPON_PMA_RX_TORGS_DEBUG_11, vertical);
	EYE_READ(EN7581_XPON_PMA_ADD_RO_RX2ANA_1, dac0);
	EYE_READ(EN7581_XPON_PMA_ADD_RO_RX2ANA_2, dac1);
#undef EYE_READ
	pr_info("q1000k: RX eye fresh=1 pi=%08x done=%08x ready=%08x horizontal=%08x vertical=%08x dac0=%08x dac1=%08x\n",
		pi, done, ready, horizontal, vertical, dac0, dac1);
	RUN(oem_eye_stop);
	return probe_ready();
}

/* Apply the OEM gain before analog calibration, not after the public eye
 * scan. Each fixed peaking candidate gets its own clean module lifetime.
 * Does not claim to replay the complete OEM first-plug state machine.
 */
static int probe_analog(bool full_reset, int peaking, bool automatic)
{
	int ret;

	RUN(oem_xpon_rx_preset);
	RUN(oem_tdc_off);
	RUN(oem_rx_l2r);
	ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 8, 8, 1, true);
	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 1, 0, 1, true);
	if (!ret && peaking >= 0)
		ret = probe_write(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan, 19, 16, peaking, true);
	if (ret)
		return ret;
	ret = probe_prcal_finalize();
	if (ret)
		return ret;
	RUN(oem_xpon_rx_oscal);
	RUN(oem_xpon_rx_pical);
	RUN(oem_xpon_rx_pdos);
	RUN(oem_xpon_rx_feos);
	RUN(oem_xpon_rx_sdcal);
	/* OEM first-plug orders reset -> L2D -> eye -> TDC -> ready.
	 * Keep eye calibration ahead of tracking enable in this experiment.
	 */
	ret = probe_write(EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 8, 8, 1, true);
	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 0, true);
	if (ret)
		return ret;
	udelay(1);
	if (full_reset) {
		RUN(oem_full_reset);
	} else {
		RUN(oem_known_reset);
	}
	RUN(oem_rx_l2d);
	ret = probe_eye();
	if (ret)
		return ret;
	ret = probe_steps(xpon_tdc_on, ARRAY_SIZE(xpon_tdc_on) - 4, true, false);
	if (ret)
		return ret;
	RUN(oem_rx_ready);
	if (automatic) {
		ret = probe_write(EN7581_XPON_PMA_SS_RX_FLL_3, 0, 0, 0, true);
		if (ret)
			return ret;
		RUN(cdr_internal_auto);
		RUN(rx_sequence_auto);
	}
	return probe_ready();
}

static int probe_prcal_finish(u32 idac)
{
	int ret;

	/* A zero/sentinel cannot establish an already selected calibration.
	 * This is a range guard, not a claim that the oscillator is locked.
	 */
	if (!idac || idac > 0x7ff)
		return -ERANGE;
	RUN(prcal_release);
	ret = probe_write(EN7581_XPON_PMA_SS_RX_FLL_1, 10, 0, idac, true);
	if (ret)
		return ret;
	RUN(prcal_power_latch);
	return probe_ready();
}

static int probe_prcal_finalize(void)
{
	u32 value;
	int ret = probe_read(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac, &value);

	return ret ?: probe_prcal_finish(value & 0x7ff);
}

static int probe_prcal_measure(u32 idac, unsigned int sample, u32 *meter)
{
	int ret;

	ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac, 10, 0, idac, true);
	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_SS_RX_FREQ_DET_4, 2, 0, 3, true);
	if (ret)
		return ret;
	usleep_range(5000, 5100);
	ret = probe_read(EN7581_XPON_PMA_RO_RX_FREQDET, meter);
	if (ret)
		return ret;
	pr_info("q1000k: RX PrCal sample=%u idac=%#x meter=%#x\n", sample, idac, *meter);
	/* No completed oscillator count is available for either sentinel.
	 * Stop the search; saved fields still unwind through normal cleanup.
	 */
	return !(*meter >> 16) || (*meter >> 16) == 0xffff ? -ERANGE : 0;
}

static int probe_prcal_rerun(void)
{
	u32 meter, selected = 0, candidate;
	unsigned int coarse, sample = 0;
	int bit, ret;

	/* OEM/public XPON_PrCal_WK: XGS target 0xa49a; exactly seven coarse
	 * and eight fine observations, with no retry. Keep TDC off while
	 * calibrating the RX oscillator and finish with checked reacquisition.
	 */
	RUN(oem_tdc_off);
	RUN(prcal_prepare);
	for (coarse = 1; coarse < 8; coarse++) {
		candidate = coarse << 8;
		ret = probe_prcal_measure(candidate, ++sample, &meter);
		if (ret)
			return ret;
		if ((meter >> 16) > 0xa49a)
			selected = candidate;
	}
	for (bit = 7; bit >= 0; bit--) {
		candidate = selected | BIT(bit);
		ret = probe_prcal_measure(candidate, ++sample, &meter);
		if (ret)
			return ret;
		selected = (meter >> 16) < 0xa49a ? candidate & ~BIT(bit) : candidate;
	}
	if (!selected || selected > 0x7ff)
		return -ERANGE;
	ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac, 10, 0, selected, true);
	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_SS_RX_FREQ_DET_4, 2, 0, 3, true);
	if (!ret)
		ret = probe_read(EN7581_XPON_PMA_RO_RX_FREQDET, &meter);
	if (ret)
		return ret;
	pr_info("q1000k: RX PrCal selected=%#x observations=%u final_meter=%#x\n",
		selected, sample, meter);
	ret = probe_prcal_finish(selected);
	return ret ?: probe_oem_clock_cycle();
}

static int probe_oem_peaking(void)
{
	u32 value;
	int ret = probe_read(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan, &value);

	if (ret)
		return ret;
	/* Public EO_Scan selects index0..7 into [19:17]; the same OEM scan
	 * @0x27d00/@0x27dd4 writes that index into [19:16]. Translate once,
	 * preserving other fields and avoiding a fresh unbounded eye scan.
	 */
	ret = probe_write(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan, 24, 24, 1, true);
	return ret ?: probe_write(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan,
				 19, 16, (value >> 17) & 7, true);
}

static int probe_oem_rx_acquire(bool automatic)
{
	int ret = probe_oem_peaking();

	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 8, 8, 1, true);
	if (!ret)
		ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 1, 0, 1, true);
	if (!ret)
		ret = probe_prcal_finalize();
	if (ret)
		return ret;
	RUN(post_eye_ready);
	ret = probe_oem_clock_cycle();
	if (ret || !automatic)
		return ret;
	/* An explicitly separate combination: public normal-mode hypotheses
	 * differ from the OEM's forced-ready path, and may be insufficient.
	 */
	ret = probe_write(EN7581_XPON_PMA_SS_RX_FLL_3, 0, 0, 0, true);
	if (ret)
		return ret;
	RUN(cdr_internal_auto);
	RUN(rx_sequence_auto);
	return probe_ready();
}

int q1000k_phy_rx_probe(u32 probe)
{
	u32 value = 0, route = 0, lane = 0;
	int ret = probe_ready();

	if (ret) {
		if (repeat_calls) repeat_failed = true;
		return ret;
	}
	if (!probe || probe >= Q1000K_RX_PROBE_COUNT) return -EINVAL;
	if (probe_closed) return -EBUSY;
	if (!gpPhyPriv->pma_init_done || gpPhyPriv->first_plugin_flag) return -EAGAIN;
	if (probe == Q1000K_RX_PROBE_OEM_RESET_REPEAT) {
		/* Only this fixed recipe may repeat, with its own hard bound.
		 * Keep the original field values until final lifecycle cleanup.
		 * Never retry a partial failure or reuse another probe's state.
		 */
		if (repeat_failed || repeat_calls >= Q1000K_RX_REPEAT_LIMIT ||
		    (!repeat_calls && (saved_count || probe_writes || post_pending)))
			return -EBUSY;
		repeat_calls++;
		ret = probe_oem_clock_cycle_reset(true);
		repeat_failed = !!ret;
		return ret;
	}
	if (saved_count || probe_writes || post_pending || repeat_calls) return -EBUSY;
	switch (probe) {
	case Q1000K_RX_PROBE_BIT_ORDER:
		ret = probe_read(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL, &value);
		return ret ?: probe_write(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL, 17, 17,
					 !(value & BIT(17)), true);
	case Q1000K_RX_PROBE_DESCRAMBLER:
		ret = probe_read(EN7581_XGPON_PHY_DBG_CTRL, &value);
		return ret ?: probe_write(EN7581_XGPON_PHY_DBG_CTRL, 9, 9, !(value & BIT(9)), true);
	case Q1000K_RX_PROBE_FEC_OC:
	case Q1000K_RX_PROBE_FEC_OFF:
		/* Mirror the public FEC setting handler, not misleading bit names. */
		ret = probe_write(EN7581_XGPON_PHY_DBG_CTRL, 6, 6, 0, true);
		if (!ret) ret = probe_write(EN7581_XGPON_PHY_DBG_CTRL, 5, 4,
					   probe == Q1000K_RX_PROBE_FEC_OC ? 3 : 0, true);
		if (!ret) ret = probe_write(EN7581_XGPON_PHY_DBG_CTRL, 1, 1,
					   probe == Q1000K_RX_PROBE_FEC_OC, true);
		return ret;
	case Q1000K_RX_PROBE_GAIN_AUTO:
		return probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 8, 8, 0, true);
	case Q1000K_RX_PROBE_GAIN_LOW:
		ret = probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 8, 8, 1, true);
		return ret ?: probe_write(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 1, 0, 0, true);
	case Q1000K_RX_PROBE_TDC_DELAY:
	case Q1000K_RX_PROBE_PLL_ORDER:
	case Q1000K_RX_PROBE_OEM_ORDER:
		return probe_recover(probe);
	case Q1000K_RX_PROBE_CDR_AUTO_RELEASE:
		RUN(oem_rx_l2r);
		RUN(oem_rx_l2d);
		return probe_ready();
	case Q1000K_RX_PROBE_CDR_INTERNAL_AUTO:
		RUN(cdr_internal_auto);
		return probe_ready();
	case Q1000K_RX_PROBE_PRCAL_FINALIZE:
		return probe_prcal_finalize();
	case Q1000K_RX_PROBE_FLL_AUTO:
		return probe_write(EN7581_XPON_PMA_SS_RX_FLL_3, 0, 0, 0, true);
	case Q1000K_RX_PROBE_RX_SEQUENCE_AUTO:
		RUN(rx_sequence_auto);
		return probe_ready();
	case Q1000K_RX_PROBE_POST_EYE_READY:
		RUN(post_eye_ready);
		return probe_ready();
	case Q1000K_RX_PROBE_OEM_CLOCK_CYCLE:
		return probe_oem_clock_cycle();
	case Q1000K_RX_PROBE_OEM_RX_ACQUIRE:
		return probe_oem_rx_acquire(false);
	case Q1000K_RX_PROBE_OEM_PEAKING:
		return probe_oem_peaking();
	case Q1000K_RX_PROBE_COMBINED_AUTO:
		return probe_oem_rx_acquire(true);
	case Q1000K_RX_PROBE_PRCAL_RERUN:
		return probe_prcal_rerun();
	case Q1000K_RX_PROBE_EYE_CURRENT:
		ret = probe_eye();
		if (ret)
			return ret;
		RUN(post_eye_ready);
		RUN(oem_rx_ready);
		return probe_ready();
	case Q1000K_RX_PROBE_OEM_ANALOG:
		return probe_analog(false, -1, false);
	case Q1000K_RX_PROBE_OEM_FULL_RESET:
		return probe_oem_clock_cycle_reset(true);
	case Q1000K_RX_PROBE_OEM_CAL_RESET:
		return probe_analog(true, -1, false);
	case Q1000K_RX_PROBE_OEM_CAL_AUTO:
		return probe_analog(true, -1, true);
	case Q1000K_RX_PROBE_OEM_EYE_0 ... Q1000K_RX_PROBE_OEM_EYE_7:
		return probe_analog(true, probe - Q1000K_RX_PROBE_OEM_EYE_0, false);
	case Q1000K_RX_PROBE_OEM_POST_INIT:
	case Q1000K_RX_PROBE_OEM_POST_CAL:
		/* Both NAND and update execute this controller bit after loading
		 * the PON modules. Do so only after the no-sync trigger here.
		 * The controller saves and verifies the field independently.
		 */
		post_pending = true;
		ret = q1000k_phy_controller_oem_post(false);
		if (ret)
			return ret;
		probe_writes++;
		pr_info("q1000k: RX OEM post-init checked=1 mask=00000100 value=00000100\n");
		return probe == Q1000K_RX_PROBE_OEM_POST_CAL ?
			probe_analog(true, -1, false) : probe_ready();
	case Q1000K_RX_PROBE_CHECKER:
	case Q1000K_RX_PROBE_CHECKER_DARK:
		/* Receiver PRBS checker only. The incoming XGS signal is not
		 * PRBS: errors/comparing are activity evidence, never a BER test.
		 * No generator, loopback or optical TX may be enabled.
		 */
		ret = probe_read(EN7581_XPON_PMA_BISTCTL_CONTROL, &value);
		if (!ret) ret = probe_read(EN7581_XPON_PMA_ADD_XPON_MODE_1, &route);
		if (!ret) ret = probe_read(EN7581_XPON_PMA_SS_BIST_1, &lane);
		if (ret) return ret;
		if ((value & BIT(8)) || (route & (BIT(16) | BIT(8))) || (lane & BIT(8)))
			return -EACCES;
		ret = probe_write(EN7581_XPON_PMA_BISTCTL_CONTROL, 16, 16, 0, true);
		return ret ?: probe_write(EN7581_XPON_PMA_BISTCTL_CONTROL, 16, 16, 1, true);
	default:
		return -EINVAL;
	}
}

int q1000k_phy_rx_probe_cleanup(void)
{
	int ret = q1000k_phy_callback_context();

	if (!ret && (saved_count || probe_writes || post_pending || repeat_calls)) probe_closed = true;
	if (ret || (!saved_count && !post_pending)) return ret;
	while (saved_count) {
		unsigned int i = saved_count - 1;

		ret = probe_write(saved[i].reg, saved[i].end, saved[i].start, saved[i].value, false);
		if (ret) return ret;
		saved_count--;
	}
	if (post_pending) {
		ret = q1000k_phy_controller_oem_post(true);
		if (ret)
			return ret;
		post_pending = false;
	}
	pr_info("q1000k: RX probe fields restored\n");
	return 0;
}

u32 q1000k_phy_rx_probe_writes(void)
{
	return probe_writes;
}

/* Fixed manual ladder shares saved originals across its two actions. The
 * lifecycle owner enforces per-action budgets, TX-off and no-sync eligibility.
 * This does not expose the other experimental probes for repeated execution. */
int q1000k_phy_rx_bench_recipe(unsigned int action)
{
	int ret = probe_ready();
	if (ret) return ret;
	if (probe_closed || repeat_calls || post_pending) return -EBUSY;
	if (!gpPhyPriv->pma_init_done || gpPhyPriv->first_plugin_flag) return -EAGAIN;
	if (action == 3) return probe_oem_clock_cycle_reset(true);
	if (action == 4) return probe_analog(true, -1, false);
	return -EINVAL;
}
