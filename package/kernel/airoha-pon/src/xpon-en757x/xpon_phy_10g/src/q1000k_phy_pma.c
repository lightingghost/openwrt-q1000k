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
