/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded Q1000K OEM receiver sequences, identical in NAND and update.
 * Call-site offsets refer to NAND phy_10g.ko f06f41a1bfcf25b3....
 * XGS branch selected explicitly for eye setup and 5500us measurement.
 * Excludes the conditional hidden LPF recovery in XPON_readout_EO.
 * See XGSPON-DEEP-RX-AUDIT.q1000k.md for provenance and limits. */
static const struct qprobe_step oem_xpon_rx_preset[] = {
	{ EN7581_XPON_ANA_RG_PXP_RX_SIGDET_NOVTH, 9, 8, 0x2, 0 }, /* 26318 */
	{ EN7581_XPON_ANA_RG_PXP_RX_SIGDET_NOVTH, 20, 16, 0x2, 0 }, /* 26330 */
	{ EN7581_XPON_ANA_RG_PXP_RX_DAC_RANGE, 25, 24, 0x3, 0 }, /* 26348 */
	{ EN7581_XPON_ANA_RG_PXP_CDR_PR_MONPR_EN, 19, 19, 0x0, 0 }, /* 26360 */
	{ EN7581_XPON_ANA_RG_PXP_CDR_PR_MONPR_EN, 18, 16, 0x7, 0 }, /* 26378 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 8, 8, 0x0, 0 }, /* 26390 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 0x0, 0 }, /* 263a8 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 0, 0, 0x0, 0 }, /* 263c0 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, 8, 8, 0x0, 0 }, /* 263d8 */
	{ EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan, 24, 24, 0x1, 0 }, /* 263f0 */
	{ EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan, 19, 16, 0x4, 0 }, /* 26408 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 8, 8, 0x1, 0 }, /* 26420 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, 1, 0, 0x1, 0 }, /* 26438 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 8, 8, 0x1, 0 }, /* 26450 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_8, 8, 8, 0x0, 0 }, /* 26468 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 16, 16, 0x0, 0 }, /* 26480 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_8, 16, 16, 0x0, 0 }, /* 26498 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 0, 0, 0x0, 0 }, /* 264b0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 0, 0, 0x0, 0 }, /* 264c8 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 16, 16, 0x1, 0 }, /* 264e0 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 16, 16, 0x1, 0 }, /* 264f8 */
};
static const struct qprobe_step oem_xpon_rx_oscal[] = {
	{ EN7581_XPON_PMA_RX_DISB_MODE_8, 0, 0, 0x0, 0 }, /* 26a20 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 0, 0, 0x1, 0 }, /* 26a38 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_jcpll_sdm_scan_rstb, 24, 24, 0x1, 0 }, /* 26a50 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_jcpll_sdm_scan_rstb, 16, 16, 0x1, 0 }, /* 26a68 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 24, 24, 0x1, 0 }, /* 26a80 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 16, 16, 0x1, 0 }, /* 26a98 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 8, 8, 0x1, 0 }, /* 26ab0 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 0, 0, 0x1, 0 }, /* 26ac8 */
	{ 0, 0, 0, 0, 200 }, /* 26ad4 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 0, 0, 0x0, 0 }, /* 26aec */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 0x0, 0 }, /* 26b04 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 0, 0, 0x0, 0 }, /* 26b1c */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 8, 8, 0x0, 0 }, /* 26b34 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 0, 0, 0x1, 0 }, /* 26b4c */
};
static const struct qprobe_step oem_xpon_rx_pical[] = {
	{ EN7581_XPON_PMA_RX_DISB_MODE_2, 24, 24, 0x1, 0 }, /* 26b78 */
	{ EN7581_XPON_PMA_SS_RX_PI_CAL, 10, 8, 0x4, 0 }, /* 26b90 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_0, 7, 0, 0x8, 0 }, /* 26ba8 */
	{ EN7581_XPON_PMA_RX_RESET_0, 16, 16, 0x0, 0 }, /* 26bc0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 8, 8, 0x0, 0 }, /* 26bd8 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_6, 8, 8, 0x0, 0 }, /* 26bf0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 0, 0, 0x0, 0 }, /* 26c08 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_6, 0, 0, 0x0, 0 }, /* 26c20 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_3, 0, 0, 0x0, 0 }, /* 26c38 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_6, 8, 8, 0x0, 0 }, /* 26c50 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_5, 24, 24, 0x0, 0 }, /* 26c68 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 8, 8, 0x0, 0 }, /* 26c80 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 8, 8, 0x0, 0 }, /* 26c98 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_3, 0, 0, 0x0, 0 }, /* 26cb0 */
	{ EN7581_XPON_PMA_RX_RESET_0, 16, 16, 0x1, 0 }, /* 26cc8 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 8, 8, 0x1, 0 }, /* 26ce0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 0, 0, 0x1, 0 }, /* 26cf8 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_6, 8, 8, 0x1, 0 }, /* 26d10 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 8, 8, 0x1, 0 }, /* 26d28 */
	{ 0, 0, 0, 0, 200 }, /* 26d34 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 8, 8, 0x0, 0 }, /* 26d4c */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_6, 8, 8, 0x0, 0 }, /* 26d64 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_3, 0, 0, 0x1, 0 }, /* 26d7c */
};
static const struct qprobe_step oem_xpon_rx_pdos[] = {
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_pwdb, 24, 24, 0x1, 0 }, /* 26da8 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_pwdb, 16, 16, 0x1, 0 }, /* 26dc0 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 8, 8, 0x0, 0 }, /* 26dd8 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 0x0, 0 }, /* 26df0 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_1, 8, 8, 0x1, 0 }, /* 26e08 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_1, 16, 16, 0x1, 0 }, /* 26e20 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_1, 24, 24, 0x1, 0 }, /* 26e38 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_2, 0, 0, 0x1, 0 }, /* 26e50 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_2, 8, 8, 0x1, 0 }, /* 26e68 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 0, 0, 0x0, 0 }, /* 26e80 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 0, 0, 0x0, 0 }, /* 26e98 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 24, 24, 0x0, 0 }, /* 26eb0 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 8, 8, 0x0, 0 }, /* 26ec8 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 24, 24, 0x0, 0 }, /* 26ee0 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 24, 24, 0x0, 0 }, /* 26ef8 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 16, 16, 0x0, 0 }, /* 26f10 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 0, 0, 0x0, 0 }, /* 26f28 */
	{ EN7581_XPON_PMA_RX_PDOS_CTRL_0, 18, 16, 0x2, 0 }, /* 26f40 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 16, 16, 0x0, 0 }, /* 26f58 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_6, 16, 16, 0x0, 0 }, /* 26f70 */
	{ EN7581_XPON_PMA_RX_RESET_1, 0, 0, 0x0, 0 }, /* 26f88 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 16, 16, 0x0, 0 }, /* 26fa0 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 16, 16, 0x0, 0 }, /* 26fb8 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 0, 0, 0x0, 0 }, /* 26fd0 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 0, 0, 0x0, 0 }, /* 26fe8 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 16, 16, 0x1, 0 }, /* 27000 */
	{ EN7581_XPON_PMA_RX_RESET_1, 0, 0, 0x1, 0 }, /* 27018 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 16, 16, 0x1, 0 }, /* 27030 */
	{ 0, 0, 0, 0, 200 }, /* 2703c */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 16, 16, 0x0, 0 }, /* 27054 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 0, 0, 0x0, 0 }, /* 2706c */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 24, 24, 0x0, 0 }, /* 27084 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 8, 8, 0x1, 0 }, /* 2709c */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 24, 24, 0x0, 0 }, /* 270b4 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 24, 24, 0x1, 0 }, /* 270cc */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 16, 16, 0x0, 0 }, /* 270e4 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 0, 0, 0x1, 0 }, /* 270fc */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_pwdb, 24, 24, 0x1, 0 }, /* 27114 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_pwdb, 16, 16, 0x0, 0 }, /* 2712c */
};
static const struct qprobe_step oem_xpon_rx_feos[] = {
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 8, 8, 0x0, 0 }, /* 27158 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 8, 8, 0x0, 0 }, /* 27170 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_2, 16, 16, 0x1, 0 }, /* 27188 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 0, 0, 0x0, 0 }, /* 271a0 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 0, 0, 0x0, 0 }, /* 271b8 */
	{ EN7581_XPON_PMA_SS_RX_FEOS, 7, 0, 0x30, 0 }, /* 271d0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 16, 16, 0x0, 0 }, /* 271e8 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 16, 16, 0x0, 0 }, /* 27200 */
	{ EN7581_XPON_PMA_RX_RESET_0, 8, 8, 0x0, 0 }, /* 27218 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 24, 24, 0x0, 0 }, /* 27230 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 24, 24, 0x0, 0 }, /* 27248 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 0, 0, 0x1, 0 }, /* 27260 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 0, 0, 0x0, 0 }, /* 27278 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 16, 16, 0x1, 0 }, /* 27290 */
	{ EN7581_XPON_PMA_RX_RESET_0, 8, 8, 0x1, 0 }, /* 272a8 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 24, 24, 0x1, 0 }, /* 272c0 */
	{ 0, 0, 0, 0, 200 }, /* 272cc */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 24, 24, 0x0, 0 }, /* 272e4 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 0, 0, 0x0, 0 }, /* 272fc */
};
static const struct qprobe_step oem_xpon_rx_sdcal[] = {
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_sigdet_cal_en, 8, 8, 0x1, 0 }, /* 27328 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_sigdet_cal_en, 0, 0, 0x0, 0 }, /* 27340 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 8, 8, 0x1, 0 }, /* 27358 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 0, 0, 0x1, 0 }, /* 27370 */
	{ EN7581_XPON_PMA_RX_RESET_0, 24, 24, 0x0, 0 }, /* 27388 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 8, 8, 0x0, 0 }, /* 273a0 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, 0, 0, 0x0, 0 }, /* 273b8 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 8, 8, 0x0, 0 }, /* 273d0 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 0, 0, 0x0, 0 }, /* 273e8 */
	{ EN7581_XPON_PMA_RX_RESET_0, 24, 24, 0x1, 0 }, /* 27400 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_8, 8, 8, 0x1, 0 }, /* 27418 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 0, 0, 0x1, 0 }, /* 27430 */
	{ 0, 0, 0, 0, 200 }, /* 2743c */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, 0, 0, 0x0, 0 }, /* 27454 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_sigdet_cal_en, 8, 8, 0x1, 0 }, /* 2746c */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_sigdet_cal_en, 0, 0, 0x0, 0 }, /* 27484 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_jcpll_sdm_scan_rstb, 24, 24, 0x1, 0 }, /* 2749c */
	{ EN7581_XPON_PMA_rg_force_da_pxp_jcpll_sdm_scan_rstb, 16, 16, 0x0, 0 }, /* 274b4 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 24, 24, 0x1, 0 }, /* 274cc */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 16, 16, 0x0, 0 }, /* 274e4 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 8, 8, 0x1, 0 }, /* 274fc */
	{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, 0, 0, 0x0, 0 }, /* 27514 */
};
static const struct qprobe_step oem_xpon_eye_cal[] = {
	{ EN7581_XPON_PMA_rg_force_da_pxp_tx_rate_ctrl, 22, 16, 0x0, 0 }, /* 2819c */
	{ EN7581_XPON_PMA_rg_force_da_pxp_tx_rate_ctrl, 24, 24, 0x0, 0 }, /* 281b4 */
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_fll_cor, 22, 16, 0x0, 0 }, /* 281cc */
	{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_fll_cor, 24, 24, 0x1, 0 }, /* 281e4 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_0, 7, 0, 0x80, 0 }, /* 281fc */
	{ EN7581_XPON_PMA_SS_RX_PI_CAL, 10, 8, 0x1, 0 }, /* 28214 */
	{ EN7581_XPON_PMA_RX_RESET_0, 16, 16, 0x0, 0 }, /* 2822c */
	{ EN7581_XPON_PMA_RX_DISB_MODE_6, 8, 8, 0x0, 0 }, /* 28244 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 8, 8, 0x0, 0 }, /* 2825c */
	{ EN7581_XPON_PMA_RX_DISB_MODE_6, 0, 0, 0x0, 0 }, /* 28274 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 0, 0, 0x0, 0 }, /* 2828c */
	{ EN7581_XPON_PMA_RX_DISB_MODE_5, 24, 24, 0x0, 0 }, /* 282a4 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_6, 8, 8, 0x0, 0 }, /* 282bc */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, 8, 8, 0x0, 0 }, /* 282d4 */
	{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, 8, 8, 0x0, 0 }, /* 282ec */
	{ EN7581_XPON_PMA_RX_RESET_0, 16, 16, 0x1, 0 }, /* 28304 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 8, 8, 0x1, 0 }, /* 2831c */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_7, 0, 0, 0x1, 0 }, /* 28334 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_6, 8, 8, 0x1, 0 }, /* 2834c */
	{ 0, 0, 0, 0, 1000 }, /* 28358 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_6, 8, 8, 0x0, 0 }, /* 28370 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_3, 0, 0, 0x0, 0 }, /* 28388 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_3, 0, 0, 0x1, 0 }, /* 283a0 */
};
static const struct qprobe_step oem_eye_setting_xgs[] = {
	{ EN7581_XPON_ANA_RG_PXP_CDR_LPF_RATIO, 1, 0, 0x0, 0 }, /* 27e34 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_0, 31, 24, 0xff, 0 }, /* 27e4c */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_0, 10, 0, 0x1c0, 0 }, /* 27e64 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_0, 26, 16, 0x240, 0 }, /* 27e7c */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_0, 9, 0, 0xf8, 0 }, /* 27ff0 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_0, 16, 16, 0x0, 0 }, /* 28008 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_2, 8, 8, 0x0, 0 }, /* 28020 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_1, 16, 16, 0x0, 0 }, /* 28038 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_2, 19, 0, 0xfff8, 0 }, /* 28050 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_3, 0, 0, 0x0, 0 }, /* 28068 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_3, 8, 8, 0x0, 0 }, /* 28080 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_3, 16, 16, 0x1, 0 }, /* 28098 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEOPENING_CTRL_0, 7, 0, 0x4, 0 }, /* 280b0 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEOPENING_CTRL_0, 15, 8, 0x4, 0 }, /* 280c8 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEOPENING_CTRL_1, 10, 0, 0x4, 0 }, /* 280e0 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEOPENING_CTRL_1, 23, 16, 0x4, 0 }, /* 280f8 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_1, 16, 16, 0x0, 0 }, /* 28110 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_1, 0, 0, 0x0, 0 }, /* 28128 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_1, 8, 8, 0x0, 0 }, /* 28140 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_2, 1, 0, 0x1, 0 }, /* 28158 */
	{ EN7581_XPON_PMA_PHY_EQ_CTRL_1, 24, 24, 0x0, 0 }, /* 28170 */
};
static const struct qprobe_step oem_eye_start[] = {
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_0, 9, 0, 0xa, 0 }, /* 283d0 */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYEINDEX_CTRL_2, 19, 0, 0x44c, 0 }, /* 283e8 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_8, 8, 8, 0x0, 0 }, /* 28400 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 8, 8, 0x1, 0 }, /* 28418 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 8, 8, 0x0, 0 }, /* 28430 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_8, 16, 16, 0x0, 0 }, /* 28448 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 16, 16, 0x0, 0 }, /* 28460 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 16, 16, 0x1, 0 }, /* 28478 */
	{ 0, 0, 0, 0, 5500 }, /* 284cc */
	{ EN7581_XPON_PMA_RX_DISB_MODE_2, 8, 8, 0x1, 0 }, /* 28514 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_2, 24, 24, 0x1, 0 }, /* 2852c */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 8, 8, 0x1, 0 }, /* 28544 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_7, 24, 24, 0x1, 0 }, /* 2855c */
	{ EN7581_XPON_PMA_RX_EYE_TOP_EYECNT_CTRL_1, 0, 0, 0x1, 0 }, /* 28574 */
};
static const struct qprobe_step oem_eye_stop[] = {
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 8, 8, 0x1, 0 }, /* 28598 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_8, 16, 16, 0x0, 0 }, /* 285b0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_9, 16, 16, 0x0, 0 }, /* 285c8 */
	{ EN7581_XPON_PMA_RX_DISB_MODE_3, 0, 0, 0x0, 0 }, /* 285e0 */
	{ EN7581_XPON_PMA_RX_FORCE_MODE_3, 0, 0, 0x0, 0 }, /* 285f8 */
};
static const struct qprobe_step oem_eye_latch[] = {
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 0, 0, 0x1, 0 }, /* 28638 */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 8, 8, 0x1, 0 }, /* 28650 */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 16, 16, 0x1, 0 }, /* 28668 */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 24, 24, 0x1, 0 }, /* 28680 */
	{ 0, 0, 0, 0, 50 }, /* 2868c */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 0, 0, 0x0, 0 }, /* 286a4 */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 8, 8, 0x0, 0 }, /* 286bc */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 16, 16, 0x0, 0 }, /* 286d4 */
	{ EN7581_XPON_PMA_SS_RX_FLL_6, 24, 24, 0x0, 0 }, /* 286ec */
	{ EN7581_XPON_PMA_RX_DEBUG_0, 24, 24, 0x0, 0 }, /* 28704 */
	{ 0, 0, 0, 0, 100 }, /* 28710 */
	{ EN7581_XPON_PMA_RX_DEBUG_0, 24, 24, 0x1, 0 }, /* 28728 */
};
static const struct qprobe_step oem_full_reset[] = {
	{ EN7581_XPON_PMA_SS_LCPLL_PWCTL_SETTING_0, 24, 24, 0x1, 0 }, /* 27614 */
	{ 0, 0, 0, 0, 100 }, /* 27620 */
	{ EN7581_XPON_PMA_SW_RST_SET, 11, 11, 0x0, 0 }, /* 27638 */
	{ EN7581_XPON_PMA_SW_RST_SET, 10, 10, 0x0, 0 }, /* 27650 */
	{ EN7581_XPON_PMA_SW_RST_SET, 9, 9, 0x0, 0 }, /* 27668 */
	{ EN7581_XPON_PMA_SW_RST_SET, 8, 8, 0x0, 0 }, /* 27680 */
	{ EN7581_XPON_PMA_SW_RST_SET, 7, 7, 0x0, 0 }, /* 27698 */
	{ EN7581_XPON_PMA_SW_RST_SET, 6, 6, 0x0, 0 }, /* 276b0 */
	{ EN7581_XPON_PMA_SW_RST_SET, 5, 5, 0x0, 0 }, /* 276c8 */
	{ EN7581_XPON_PMA_SW_RST_SET, 4, 4, 0x0, 0 }, /* 276e0 */
	{ EN7581_XPON_PMA_SW_RST_SET, 3, 3, 0x0, 0 }, /* 276f8 */
	{ EN7581_XPON_PMA_SW_RST_SET, 2, 2, 0x0, 0 }, /* 27710 */
	{ EN7581_XPON_PMA_SW_RST_SET, 1, 1, 0x0, 0 }, /* 27728 */
	{ EN7581_XPON_PMA_SW_RST_SET, 0, 0, 0x0, 0 }, /* 27740 */
	{ 0, 0, 0, 0, 10 }, /* 27788 */
	{ EN7581_XPON_PMA_SW_RST_SET, 11, 11, 0x1, 0 }, /* 277a0 */
	{ EN7581_XPON_PMA_SW_RST_SET, 10, 10, 0x1, 0 }, /* 277b8 */
	{ EN7581_XPON_PMA_SW_RST_SET, 9, 9, 0x1, 0 }, /* 277d0 */
	{ EN7581_XPON_PMA_SW_RST_SET, 8, 8, 0x1, 0 }, /* 277e8 */
	{ EN7581_XPON_PMA_SW_RST_SET, 7, 7, 0x1, 0 }, /* 27800 */
	{ EN7581_XPON_PMA_SW_RST_SET, 6, 6, 0x1, 0 }, /* 27818 */
	{ EN7581_XPON_PMA_SW_RST_SET, 5, 5, 0x1, 0 }, /* 27830 */
	{ EN7581_XPON_PMA_SW_RST_SET, 4, 4, 0x1, 0 }, /* 27848 */
	{ EN7581_XPON_PMA_SW_RST_SET, 3, 3, 0x1, 0 }, /* 27860 */
	{ EN7581_XPON_PMA_SW_RST_SET, 2, 2, 0x1, 0 }, /* 27878 */
	{ EN7581_XPON_PMA_SW_RST_SET, 1, 1, 0x1, 0 }, /* 27890 */
	{ EN7581_XPON_PMA_SW_RST_SET, 0, 0, 0x1, 0 }, /* 278a8 */
};
