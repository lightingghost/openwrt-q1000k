// SPDX-License-Identifier: GPL-2.0-only
/* Explicit RX-only RAM bench: no registration executor or MAC interrupts. */
#include <linux/module.h>
#include <linux/of.h>
#include <an7581_xpon.h>
#include <q1000k_phy_api.h>
#include "common/q1000k_protocol.h"
#include "common/q1000k_transport.h"
#include "common/q1000k_rx_bench.h"

static bool rx_bench;
module_param(rx_bench, bool, 0400);
MODULE_PARM_DESC(rx_bench, "Receive-only Q1000K RAM bench; requires immutable TX inhibit");
static bool rx_reacquire;
module_param(rx_reacquire, bool, 0400);
MODULE_PARM_DESC(rx_reacquire, "RX bench only: one PMA reacquisition attempt after ten light/no-sync polls");

static bool rx_restore_pll;
module_param(rx_restore_pll, bool, 0400);
MODULE_PARM_DESC(rx_restore_pll, "RX bench only: restore PHY PLL clocks after the single recovery");

static bool rx_restore_gain;
module_param(rx_restore_gain, bool, 0400);
MODULE_PARM_DESC(rx_restore_gain, "RX bench only: apply OEM receiver gain after the single recovery");

static uint rx_probe;
module_param(rx_probe, uint, 0400);
MODULE_PARM_DESC(rx_probe, "RX bench experiment 1..10, immutable, one attempt per load");

bool q1000k_rx_bench_enabled(void)
{
	return rx_bench;
}

int q1000k_rx_bench_prepare(void)
{
	struct device_node *root;
	bool bench;
	int ret;

	if (rx_probe >= Q1000K_RX_PROBE_COUNT || (rx_probe && (!rx_reacquire || rx_restore_pll || rx_restore_gain)))
		return -EINVAL;
	if ((rx_reacquire && !rx_bench) || ((rx_restore_pll || rx_restore_gain) && !rx_reacquire))
		return -EINVAL;
	if (rx_bench) {
		root = of_find_node_by_path("/");
		bench = of_machine_is_compatible("quantum,q1000k") &&
			of_property_read_bool(root, "quantum,xgspon-bench");
		of_node_put(root);
		if (!bench)
			return -EPERM;
	}
	/* The PHY independently verifies the controller's cached probe-time
	 * inhibit, lease and live TX-off state before accepting this mode.
	 */
	ret = q1000k_phy_set_rx_bench(rx_bench, rx_reacquire, rx_restore_pll, rx_restore_gain);
	return ret ?: q1000k_phy_set_rx_probe(rx_probe);
}

static int qrx_status_get(char *buffer, const struct kernel_param *kp)
{
	struct q1000k_rx_sample s;
	char power[16];
	u32 mask;
	int ret;

	if (!rx_bench)
		return -EOPNOTSUPP;
	ret = q1000k_protocol_status();
	if (ret)
		return ret;
	if (!q1000k_transport_running())
		return -EAGAIN;
	/* Module parameter attributes can be read while init is in progress.
	 * Attachment alone does not establish the optical clocks/WAN selector.
	 * The PHY sample requires successful configuration and active RX first.
	 */
	ret = q1000k_phy_rx_sample(&s);
	if (ret)
		return ret;
	mask = get_xpon_data(0x5040);
	ret = an7581_xpon_status();
	if (ret || mask)
		return ret ?: -EIO;
	if (s.rx_power_valid)
		scnprintf(power, sizeof(power), "%u", s.rx_power_nw);
	else
		scnprintf(power, sizeof(power), "null");
	return scnprintf(buffer, PAGE_SIZE,
		"{\"rx_bench\":true,\"registration_enabled\":false,"
		"\"tx_inhibited\":true,\"tx_enabled\":false,\"mac_irq_mask\":0,"
		"\"controller_los\":%s,\"phy_los\":%s,\"synced\":%s,"
		"\"sync_status\":%u,\"frames\":%u,\"lof\":%u,"
		"\"fec_total\":%u,\"fec_corrected\":%u,\"fec_uncorrected\":%u,"
		"\"irq_calls\":%u,\"poll_calls\":%u,\"sampled_ms\":%llu,"
		"\"reacquire_enabled\":%s,\"reacquire_attempts\":%u,"
		"\"pll_restore_enabled\":%s,\"gain_restore_enabled\":%s,"
		"\"rx_power_valid\":%s,\"rx_power_nw\":%s,\"receiver_version\":5,"
		"\"receiver\":{\"rx_control\":%u,\"pcs_reset\":%u,\"pma_reset\":%u,"
		"\"clock_control\":%u,\"cdr_control\":%u,\"rx_frequency\":%u,"
		"\"pll_status\":%u,\"tdc_control\":%u,\"rx_analog0\":%u,"
		"\"rx_analog1\":%u,\"rx_analog2\":%u,\"rx_sequence_force\":%u,"
		"\"rx_sequence_disable\":%u,"
		"\"rx_sequence_force0\":%u,\"rx_sequence_disable0\":%u,"
		"\"rx_lock_force\":%u,\"rx_lock_disable\":%u,"
		"\"rx_oscal_control\":%u,\"rx_reset0\":%u,"
		"\"rx_reset1\":%u,\"pll_power\":%u,"
		"\"pll_filter\":%u,\"pll_pcw1\":%u,"
		"\"pll_pcw2\":%u,\"pll_force\":%u,\"pll_measure\":%u,\"pll_kband\":%u,\"pll_outputs\":%u,\"rx_frontend_gain\":%u,"
		"\"sfp_status\":%u,\"sfp_polarity\":%u,\"digital_status\":%u,\"pcs_debug_control\":%u,\"serdes_control\":%u,"
		"\"rx_clock_divider\":%u,\"rx_bus_width\":%u,\"rx_input_control\":%u,\"rx_cdr_ratio\":%u,\"rx_rate_control\":%u,"
		"\"rx_osr_control\":%u,\"signal_control\":%u,\"rx_equalizer\":%u,\"rx_frontend_power\":%u},"
		"\"pcs_counters\":{\"cw_start\":%u,\"cw_end\":%u,\"sof_to_mac\":%u,\"eof_to_mac\":%u,\"psync_mismatch\":%u,"
		"\"sfc_hec_error\":%u,\"pon_id_hec_error\":%u}}\n",
		s.controller_los ? "true" : "false", s.phy_los ? "true" : "false",
		s.synced ? "true" : "false", s.sync_status, s.frames, s.lof,
		s.fec_total, s.fec_corrected, s.fec_uncorrected,
		s.irq_calls, s.poll_calls, (unsigned long long)s.sampled_ms,
		s.reacquire_enabled ? "true" : "false", s.reacquire_attempts,
		s.pll_restore_enabled ? "true" : "false",
		s.gain_restore_enabled ? "true" : "false",
		s.rx_power_valid ? "true" : "false", power,
		s.receiver.rx_control, s.receiver.pcs_reset, s.receiver.pma_reset,
		s.receiver.clock_control, s.receiver.cdr_control, s.receiver.rx_frequency,
		s.receiver.pll_status, s.receiver.tdc_control, s.receiver.rx_analog0,
		s.receiver.rx_analog1, s.receiver.rx_analog2, s.receiver.rx_sequence_force,
		s.receiver.rx_sequence_disable,
		s.receiver.rx_sequence_force0, s.receiver.rx_sequence_disable0,
		s.receiver.rx_lock_force, s.receiver.rx_lock_disable,
		s.receiver.rx_oscal_control, s.receiver.rx_reset0,
		s.receiver.rx_reset1, s.receiver.pll_power,
		s.receiver.pll_filter, s.receiver.pll_pcw1,
		s.receiver.pll_pcw2, s.receiver.pll_force, s.receiver.pll_measure,
		s.receiver.pll_kband, s.receiver.pll_outputs, s.receiver.rx_frontend_gain,
		s.receiver.sfp_status,
		s.receiver.sfp_polarity,
		s.receiver.digital_status,
		s.receiver.pcs_debug_control,
		s.receiver.serdes_control,
		s.receiver.rx_clock_divider,
		s.receiver.rx_bus_width,
		s.receiver.rx_input_control,
		s.receiver.rx_cdr_ratio,
		s.receiver.rx_rate_control,
		s.receiver.rx_osr_control,
		s.receiver.signal_control,
		s.receiver.rx_equalizer,
		s.receiver.rx_frontend_power,
		s.pcs_counters.cw_start,
		s.pcs_counters.cw_end,
		s.pcs_counters.sof_to_mac,
		s.pcs_counters.eof_to_mac,
		s.pcs_counters.psync_mismatch,
		s.pcs_counters.sfc_hec_error,
		s.pcs_counters.pon_id_hec_error);
}
static const struct kernel_param_ops qrx_status_ops = { .get = qrx_status_get };
module_param_cb(rx_bench_status, &qrx_status_ops, NULL, 0400);

/* Separate attribute: maximum u32 values plus all keys fit below PAGE_SIZE. */
static int qrx_diagnostics_get(char *buffer, const struct kernel_param *kp)
{
	struct q1000k_rx_diagnostics s;
	int ret;

	if (!rx_bench) return -EOPNOTSUPP;
	ret = q1000k_protocol_status();
	if (ret) return ret;
	if (!q1000k_transport_running()) return -EAGAIN;
	ret = q1000k_phy_rx_diagnostics(&s);
	if (ret) return ret;
	return scnprintf(buffer, PAGE_SIZE,
		"{\"diagnostics_version\":1,\"probe\":%u,\"attempts\":%u,\"writes\":%u,"
		"\"sampled_ms\":%llu"
#define QDIAG_FORMAT(name, reg) ",\"" #name "\":%u"
		Q1000K_RX_DIAG_FIELDS(QDIAG_FORMAT)
#undef QDIAG_FORMAT
		"}\n", s.probe, s.attempts, s.writes, (unsigned long long)s.sampled_ms
#define QDIAG_VALUE(name, reg) , s.name
		Q1000K_RX_DIAG_FIELDS(QDIAG_VALUE)
#undef QDIAG_VALUE
	);
}
static const struct kernel_param_ops qrx_diagnostics_ops = { .get = qrx_diagnostics_get };
module_param_cb(rx_bench_diagnostics, &qrx_diagnostics_ops, NULL, 0400);
