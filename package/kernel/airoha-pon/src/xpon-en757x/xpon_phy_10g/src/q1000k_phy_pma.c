// SPDX-License-Identifier: GPL-2.0-only
/* Checked phase boundaries around the imported AN7581 analog sequences. */
#include <linux/delay.h>
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

/* RX bench callback owner only, after a fresh inhibited/TX-off RX sample.
 * Reuse the reference no-LOS/no-ready out/in sequence without its repeated
 * polling, registration dispatch, or SCU reset escalation. Initial calibration
 * must already have completed, so PMA reset can only take PLUG_IN here.
 */
int q1000k_phy_rx_reacquire(bool restore_pll)
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
