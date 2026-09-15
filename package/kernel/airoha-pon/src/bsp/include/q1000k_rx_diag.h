/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_RX_DIAG_H
#define _Q1000K_RX_DIAG_H
/* Ordinary controls/status, never FIFO data, interrupt-ack or probe selectors.
 * Register names are resolved by the PHY; the MAC uses only the field names.
 */
#define Q1000K_RX_DIAG_FIELDS(X) \
	X(rx_meter_cycles, EN7581_XPON_PMA_SS_RX_FREQ_DET_1) \
	X(rx_meter_lock_target, EN7581_XPON_PMA_SS_RX_FREQ_DET_2) \
	X(rx_meter_unlock_target, EN7581_XPON_PMA_SS_RX_FREQ_DET_3) \
	X(rx_meter_control, EN7581_XPON_PMA_SS_RX_FREQ_DET_4) \
	X(rx_meter_result, EN7581_XPON_PMA_RO_RX_FREQDET) \
	X(pll_meter_result, EN7581_XPON_PMA_RO_PLL_FREQDET) \
	X(pll_fine_meter_result, EN7581_XPON_PMA_RO_PLL_FT_FREQDET) \
	X(pma_meter_result, EN7581_XPON_PMA_RO_PMA_FREQDET) \
	X(pma_meter_control, EN7581_XPON_PMA_RG_PMA_FREQDET) \
	X(tdc_tx_meter_result, EN7581_XPON_PMA_RO_TDC_TX_FREQDET) \
	X(jcpll_meter_result, EN7581_XPON_PMA_RO_JCPLL_FT_FREQDET) \
	X(jcpll_500m_result, EN7581_XPON_PMA_RO_JCPLL_500M_FREQDET) \
	X(checker_control, EN7581_XPON_PMA_BISTCTL_CONTROL) \
	X(checker_event, EN7581_XPON_PMA_BISTCTL_PRBS_EVENT) \
	X(checker_errors, EN7581_XPON_PMA_BISTCTL_PRBS_ERRCNT) \
	X(checker_pattern, EN7581_XPON_PMA_BISTCTL_ALIGN_PAT) \
	X(checker_seed, EN7581_XPON_PMA_BISTCTL_PRBS_INITIAL_SEED) \
	X(checker_threshold, EN7581_XPON_PMA_BISTCTL_PRBS_FAIL_THRESHOLD) \
	X(bist_lane_control, EN7581_XPON_PMA_SS_BIST_1) \
	X(data_route_control, EN7581_XPON_PMA_ADD_XPON_MODE_1) \
	X(tdc_filter0, EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_0) \
	X(tdc_filter1, EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_1) \
	X(tdc_filter3, EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_3) \
	X(tdc_filter5, EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_5) \
	X(tdc_filter6, EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_6) \
	X(tdc_power5, EN7581_XPON_PMA_SS_LCPLL_TDC_PW_5) \
	X(tdc_pcw_change, EN7581_XPON_PMA_rg_force_da_pxp_txpll_sdm_pcw_chg) \
	X(cdr_idac_control, EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac) \
	X(rx_impedance, EN7581_XPON_ANA_RG_PXP_RX_SIGDET_NOVTH) \
	X(rx_revision, EN7581_XPON_ANA_RG_PXP_RX_REV_0) \
	X(rx_equalizer_force, EN7581_XPON_ANA_RG_PXP_AEQ_CFORCE) \
	X(rx_oscal_window, EN7581_XPON_ANA_RG_PXP_RX_OSCAL_WATCH_WNDW)
#endif
