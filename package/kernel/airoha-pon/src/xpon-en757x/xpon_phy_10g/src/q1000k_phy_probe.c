// SPDX-License-Identifier: GPL-2.0-only
/* Bounded RX-only experiments. No raw register access is exposed to users. */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <an7581_pon_phy.h>
#include <q1000k_phy_api.h>
#include "phy_global.h"
#include "phy_reg.h"
#include "en7581_reg.h"

struct qprobe_step { u32 reg, end, start, value, delay_us; };
#include "q1000k_phy_probe_steps.h"

/* Save distinct fields before their first update, including partial failures.
 * Replay originals in reverse order after callbacks drain, with TX still off.
 */
static struct { u32 reg, end, start, value; } saved[64];
static unsigned int saved_count;
static u32 probe_writes;

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

int q1000k_phy_rx_probe(u32 probe)
{
	u32 value = 0, route = 0, lane = 0;
	int ret = probe_ready();

	if (ret) return ret;
	if (!probe || probe >= Q1000K_RX_PROBE_COUNT) return -EINVAL;
	if (saved_count || probe_writes) return -EBUSY;
	if (!gpPhyPriv->pma_init_done || gpPhyPriv->first_plugin_flag) return -EAGAIN;
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
	case Q1000K_RX_PROBE_CHECKER:
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

	if (ret || !saved_count) return ret;
	while (saved_count) {
		unsigned int i = saved_count - 1;

		ret = probe_write(saved[i].reg, saved[i].end, saved[i].start, saved[i].value, false);
		if (ret) return ret;
		saved_count--;
	}
	pr_info("q1000k: RX probe fields restored\n");
	return 0;
}

u32 q1000k_phy_rx_probe_writes(void)
{
	return probe_writes;
}
