// SPDX-License-Identifier: GPL-2.0-only
/* Checked phase boundaries around the imported AN7581 analog sequences. */
#include <linux/delay.h>
#include <linux/printk.h>
#include <an7581_pon_phy.h>
#include <q1000k_phy_api.h>
#include "phy_global.h"
#include "phy_reg.h"
#include "en7581_reg.h"
#include "en7581_pma.h"

static int qpma_ready(void)
{
	int ret = an7581_pon_phy_status();

	return ret ?: q1000k_phy_controller_check();
}

/* Optional bench experiment: the thirteen control updates in TXPLL_on(),
 * also present in the OEM PHY's reconnect path. These are PHY clock controls,
 * not optical laser controls. Keep the controller's immutable TX inhibit.
 * Run after the existing checked recovery; this is not the full OEM reset
 * sequence (which also touches undocumented upper digital-reset bits).
 */
static int qpma_restore_pll(void)
{
	static const struct { u32 reg, bit, value; } steps[] = {
		{ EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en, 24, 1 },
		{ EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en, 16, 1 },
		{ EN7581_XPON_PMA_SS_LCPLL_PWCTL_SETTING_0, 24, 1 },
		{ EN7581_XPON_ANA_RG_PXP_JCPLL_FREQ_MEAS_EN, 0, 1 },
		{ EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en, 8, 1 },
		{ EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en, 0, 1 },
		{ EN7581_XPON_ANA_RG_PXP_TXPLL_TCL_KBAND_VREF, 16, 1 },
		{ EN7581_XPON_ANA_RG_PXP_TXPLL_TCL_KBAND_VREF, 8, 0 },
		{ EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN, 8, 1 },
		{ EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN, 0, 1 },
		{ EN7581_XPON_ANA_RG_PXP_JCPLL_FREQ_MEAS_EN, 0, 0 },
		{ EN7581_XPON_ANA_RG_PXP_JCPLL_FREQ_MEAS_EN, 24, 0 },
		{ EN7581_XPON_ANA_RG_PXP_JCPLL_FREQ_MEAS_EN, 16, 0 },
	};
	u32 mask, expected, actual;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		ret = qpma_ready();
		if (ret)
			return ret;
		mask = BIT(steps[i].bit);
		expected = steps[i].value ? mask : 0;
		ret = an7581_pon_phy_update(steps[i].reg, steps[i].bit,
					 steps[i].bit, steps[i].value);
		if (!ret)
			ret = an7581_pon_phy_read(steps[i].reg, &actual);
		if (ret || actual == ~0U || (actual & mask) != expected)
			return ret ?: -EIO;
		if (i == 2)
			udelay(6);
	}
	udelay(500);
	return qpma_ready();
}

static bool qpma_gain_saved;
static u32 qpma_gain_original;

static int qpma_restore_gain(void)
{
	const u32 reg = EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl;
	u32 actual;
	int i, ret;

	ret = qpma_ready();
	if (ret)
		return ret;
	if (qpma_gain_saved)
		return -EBUSY;
	ret = an7581_pon_phy_read(reg, &actual);
	if (ret || actual == ~0U)
		return ret ?: -EIO;
	qpma_gain_original = actual;
	qpma_gain_saved = true;
	/* OEM XPON_RX_preset: 0x1fa8b88c[8]=1, then [1:0]=1.
	 * Isolate this RX-only difference from the larger OEM reset sequence.
	 * Preserve every unrelated bit and verify both writes immediately.
	 */
	for (i = 0; i < 2; i++) {
		u32 end = i ? 1 : 8, start = i ? 0 : 8;
		u32 mask = i ? 3 : BIT(8), expected = i ? 1 : BIT(8);

		ret = qpma_ready();
		if (ret)
			return ret;
		ret = an7581_pon_phy_update(reg, end, start, 1);
		if (!ret)
			ret = an7581_pon_phy_read(reg, &actual);
		if (ret || actual == ~0U || (actual & mask) != expected)
			return ret ?: -EIO;
	}
	return qpma_ready();
}

/* Called after RX callbacks have drained and optical TX is disabled. Restore
 * only our two fields, preserving unrelated changes. A failure is returned to
 * the lifecycle owner, so a capture cannot claim successful cleanup.
 */
int q1000k_phy_rx_cleanup(void)
{
	const u32 reg = EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl;
	u32 actual;
	int i, ret = q1000k_phy_callback_context();

	if (ret || !qpma_gain_saved)
		return ret;
	if (!gpPhyPriv || gpPhyPriv->trans_tx_status != PHY_DISABLE ||
	    gpPhyPriv->phyCfg.flags.txPowerEnFlag)
		return -EACCES;
	for (i = 0; i < 2; i++) {
		u32 end = i ? 8 : 1, start = i ? 8 : 0;
		u32 mask = i ? BIT(8) : 3;
		u32 expected = qpma_gain_original & mask;

		ret = qpma_ready();
		if (ret)
			return ret;
		ret = an7581_pon_phy_update(reg, end, start, expected >> start);
		if (!ret)
			ret = an7581_pon_phy_read(reg, &actual);
		if (ret || actual == ~0U || (actual & mask) != expected)
			return ret ?: -EIO;
	}
	ret = qpma_ready();
	if (!ret) {
		qpma_gain_saved = false;
		pr_info("q1000k: RX gain restored to %#x\n", qpma_gain_original & 0x103);
	}
	return ret;
}

/* RX bench callback owner only, after a fresh inhibited/TX-off RX sample.
 * Reuse the reference no-LOS/no-ready out/in sequence without its repeated
 * polling, registration dispatch, or SCU reset escalation. Initial calibration
 * must already have completed, so PMA reset can only take PLUG_IN here.
 */
int q1000k_phy_rx_reacquire(bool restore_pll, bool restore_gain)
{
	int ret = q1000k_phy_callback_context();

	if (ret)
		return ret;
	if (!gpPhyPriv || gpPhyPriv->wan_sel != SCU_WAN_CONF_REG_WAN_SEL_XGSPON)
		return -EINVAL;
	if (!gpPhyPriv->pma_init_done || gpPhyPriv->first_plugin_flag)
		return -EAGAIN;
	if (gpPhyPriv->trans_tx_status != PHY_DISABLE || gpPhyPriv->phyCfg.flags.txPowerEnFlag)
		return -EACCES;
	ret = qpma_ready();
	if (ret)
		return ret;
	fiber_plug_reset(PLUG_OUT, gpPhyPriv->wan_sel);
	ret = qpma_ready();
	if (!ret)
		ret = q1000k_phy_pma_reset();
	if (!ret && restore_pll)
		ret = qpma_restore_pll();
	if (!ret && restore_gain)
		ret = qpma_restore_gain();
	return ret ?: qpma_ready();
}

int q1000k_phy_pma_init(void)
{
	u32 status;
	bool los;
	int ret = q1000k_phy_callback_context();

	if (ret) return ret;
	if (!gpPhyPriv || gpPhyPriv->wan_sel != SCU_WAN_CONF_REG_WAN_SEL_XGSPON)
		return -EINVAL;
	ret = qpma_ready();
	if (ret) return ret;
	gpPhyPriv->pma_init_done = FALSE;
	xpon_init(gpPhyPriv->wan_sel);
	ret = qpma_ready();
	if (ret) return ret;
	ret = an7581_pon_phy_read(EN7581_XGPON_PHY_SFP_STA, &status);
	if (ret) return ret;
	if (status == ~0U) return -EIO;
	los = !!(status & EN7581_XGPON_PHY_SFP_RX_LOS_ST);

	/* Preserve the vendor's calibration-before-power-save sequence even
	 * without signal. Never treat a failed LOS query as signal present.
	 */
	fiber_plug_reset(FIRST_PLUG_IN, gpPhyPriv->wan_sel);
	ret = qpma_ready();
	if (ret) return ret;
	if (los) {
		usleep_range(1000, 1500);
		ret = qpma_ready();
		if (ret) return ret;
		fiber_plug_reset(PLUG_OUT, gpPhyPriv->wan_sel);
		ret = qpma_ready();
		if (ret) return ret;
		usleep_range(1000, 1500);
	} else {
		/* This delay already exists in the reference initialization. The
		 * callback is sleepable; do not busy-wait for 350 milliseconds.
		 */
		msleep(350);
	}
	ret = qpma_ready();
	if (ret) return ret;
	if (!los) {
		gpPhyPriv->first_plugin_flag = FALSE;
		gpPhyPriv->pma_init_done = TRUE;
	}
	return 0;
}
