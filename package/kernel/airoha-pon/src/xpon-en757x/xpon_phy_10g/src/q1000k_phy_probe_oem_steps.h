/* SPDX-License-Identifier: GPL-2.0-only */
/* Q1000K OEM phy_10g.ko disassembly: exact named AN7581 fields only.
 * These bounded experiments are separate from the imported public sequence.
 * See XGSPON-OEM-CLOCK-AUDIT.q1000k.md for evidence and interpretation.
 */

/* XPON_RX_L2R @0x26984, then XPON_RX_L2D @0x27b48. The OEM leaves
 * direct LPF reset under hardware ownership, unlike the public routine.
 */
static const struct qprobe_step oem_rx_l2r[] = {
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 16, 16, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 24, 24, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 0, 0, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 8, 8, 0, 0 },
	{ 0, 0, 0, 0, 100 },
};
static const struct qprobe_step oem_rx_l2d[] = {
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 0, 0, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 8, 8, 1, 0 },
	{ 0, 0, 0, 0, 200 },
};

/* XPON_TDC_off @0x26508: no public PCW override/pulse and a 1ms wait. */
static const struct qprobe_step oem_tdc_off[] = {
	{ EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_3, 8, 8, 1, 0 },
	{ EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_1, 0, 0, 0, 0 },
	{ EN7581_XPON_PMA_SS_LCPLL_TDC_PW_0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 1000 },
};

/* XPON_DIG_reset @0x275f8 restricted to documented bits6:0. Unknown
 * OEM bits11:7 are deliberately absent. Preserve OEM ordering/delays.
 */
static const struct qprobe_step oem_known_reset[] = {
	{ EN7581_XPON_PMA_SS_LCPLL_PWCTL_SETTING_0, 24, 24, 1, 0 },
	{ 0, 0, 0, 0, 100 },
	{ EN7581_XPON_PMA_SW_RST_SET, 6, 6, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 5, 5, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 4, 4, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 3, 3, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 2, 2, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 1, 1, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 10 },
	{ EN7581_XPON_PMA_SW_RST_SET, 6, 6, 1, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 5, 5, 1, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 4, 4, 1, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 3, 3, 1, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 2, 2, 1, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 1, 1, 1, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 0, 0, 1, 0 },
};

/* XPON_RX_rxrdy @0x27570 and XPON_phy_status @0x27524. */
static const struct qprobe_step oem_rx_ready[] = {
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 24, 24, 1, 0 },
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 24, 24, 0, 0 },
	{ 0, 0, 0, 0, 10 },
	{ EN7581_XPON_PMA_SW_RST_SET, 0, 0, 0, 0 },
	{ EN7581_XPON_PMA_SW_RST_SET, 0, 0, 1, 0 },
	{ 0, 0, 0, 0, 100 },
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 8, 8, 1, 0 },
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 0, 0 },
	{ 0, 0, 0, 0, 1 },
};

/* XPON_PrCal_WK @0x25e9c..0x25f5c: seven coarse/eight fine samples follow.
 * All are receiver oscillator controls despite some shared-word names.
 */
static const struct qprobe_step prcal_prepare[] = {
	{ EN7581_XPON_ANA_RG_PXP_CDR_PR_INJ_MODE, 24, 24, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 24, 24, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 16, 16, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 8, 8, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 0, 0, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac, 16, 16, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 24, 24, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 16, 16, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 16, 16, 1, 0 },
};

/* Exact finalization prefix @0x26148..0x261d8. The learned IDAC goes into
 * SS_RX_FLL_1 after this prefix and before prcal_power_latch below.
 */
static const struct qprobe_step prcal_release[] = {
	{ EN7581_XPON_ANA_RG_PXP_CDR_PR_INJ_MODE, 24, 24, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 24, 24, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 16, 16, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 8, 8, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en, 0, 0, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac, 16, 16, 0, 0 },
	{ EN7581_XPON_PMA_SS_RX_FLL_b, 0, 0, 1, 0 },
};
static const struct qprobe_step prcal_power_latch[] = {
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 16, 16, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 16, 16, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb, 24, 24, 1, 0 },
};

/* Documented normal modes, not OEM defaults. DISB normal mode uses 1.
 * Release the inner reset/lock controls before their outer overrides.
 */
static const struct qprobe_step cdr_internal_auto[] = {
	{ EN7581_XPON_PMA_RX_DISB_MODE_0, 24, 24, 1, 0 },
	{ EN7581_XPON_PMA_RX_DISB_MODE_0, 16, 16, 1, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 24, 24, 0, 0 },
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 8, 8, 0, 0 },
};
static const struct qprobe_step rx_sequence_auto[] = {
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 0, 0, 1, 0 },
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 1, 0 },
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 16, 16, 1, 0 },
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 24, 24, 1, 0 },
};
/* XPON_RX_pical ready state, reasserted after EO clears it. */
static const struct qprobe_step post_eye_ready[] = {
	{ EN7581_XPON_PMA_RX_DISB_MODE_3, 0, 0, 0, 0 },
	{ EN7581_XPON_PMA_RX_FORCE_MODE_3, 0, 0, 1, 0 },
};
