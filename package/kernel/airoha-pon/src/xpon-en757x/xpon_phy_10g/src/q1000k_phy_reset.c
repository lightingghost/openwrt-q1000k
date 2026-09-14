// SPDX-License-Identifier: GPL-2.0-only
/* Optical reset sequencing. Caller owns the PHY callback mutex. */
#include <linux/delay.h>
#include <an7581_pon_phy.h>
#include <an7581_pon_scu.h>
#include <q1000k_phy_api.h>
#include "phy_global.h"
#include "phy_init.h"
#include "phy_tx.h"
#include "en7581_reg.h"

static int qphy_checked_write(u32 reg, u32 value)
{
	u32 actual;
	int ret = an7581_pon_phy_write(reg, value);

	if (!ret)
		ret = an7581_pon_phy_read(reg, &actual);
	return ret ? ret : actual == value ? 0 : -EIO;
}

static int qphy_checked_bit(u32 reg, u32 bit, u32 value)
{
	u32 actual;
	int ret = an7581_pon_phy_update(reg, bit, bit, value);

	if (!ret)
		ret = an7581_pon_phy_read(reg, &actual);
	return ret ? ret : actual == ~0U || ((actual >> bit) & 1) != value ? -EIO : 0;
}

int q1000k_phy_top_reset(void)
{
	u32 mode;
	int ret = q1000k_phy_callback_context();

	if (ret)
		return ret;
	ret = an7581_pon_phy_status();
	if (ret)
		return ret;
	ret = an7581_pon_wan_get(&mode);
	if (ret)
		return ret;
	if (mode != 10)
		return -EINVAL;
	ret = qphy_checked_write(EN7581_XPON_PMA_PON_CK_SET, 0);
	if (ret)
		return ret;
	udelay(1);
	ret = an7581_pon_wan_set(17);
	if (ret)
		return ret;
	ret = qphy_checked_bit(EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN, 8, 0);
	if (!ret)
		ret = qphy_checked_bit(EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en,
				       24, 1);
	if (!ret)
		ret = qphy_checked_bit(EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en,
				       16, 0);
	if (!ret)
		ret = an7581_pon_phy_reset();
	/* On failure leave the WAN clocks gated; never replay a shared SCU word. */
	if (!ret)
		ret = an7581_pon_wan_set(mode);
	if (!ret)
		ret = qphy_checked_write(EN7581_XGPON_PHY_XG_PHY_RST_N,
					 EN7581_XGPON_PHY_XG_PHY_RST_N_ON);
	if (!ret) {
		udelay(1);
		ret = qphy_checked_write(EN7581_XGPON_PHY_XG_PHY_RST_N,
					 EN7581_XGPON_PHY_XG_PHY_RST_N_OFF);
	}
	return ret;
}

int q1000k_phy_pma_reset(void)
{
	int ret = q1000k_phy_callback_context();

	if (ret)
		return ret;
	ret = phy_trans_power_switch(PHY_TX_DIS_ON_HW_ONLY);
	if (!ret)
		ret = ponPhyFunc[PHY_PMA_RESET_FUNC](NULL);
	if (!ret)
		ret = an7581_pon_phy_status();
	if (!ret)
		ret = phy_trans_power_switch(PHY_TX_DIS_RESTORE_BY_SW);
	return ret;
}
