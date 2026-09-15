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

bool q1000k_rx_bench_enabled(void)
{
	return rx_bench;
}

int q1000k_rx_bench_prepare(void)
{
	struct device_node *root;
	bool bench;

	if (rx_reacquire && !rx_bench)
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
	return q1000k_phy_set_rx_bench(rx_bench, rx_reacquire);
}

static int qrx_status_get(char *buffer, const struct kernel_param *kp)
{
	struct q1000k_rx_sample s;
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
	return scnprintf(buffer, PAGE_SIZE,
		"{\"rx_bench\":true,\"registration_enabled\":false,"
		"\"tx_inhibited\":true,\"tx_enabled\":false,\"mac_irq_mask\":0,"
		"\"controller_los\":%s,\"phy_los\":%s,\"synced\":%s,"
		"\"sync_status\":%u,\"frames\":%u,\"lof\":%u,"
		"\"fec_total\":%u,\"fec_corrected\":%u,\"fec_uncorrected\":%u,"
		"\"irq_calls\":%u,\"poll_calls\":%u,\"sampled_ms\":%llu,"
		"\"reacquire_enabled\":%s,\"reacquire_attempts\":%u,"
		"\"receiver\":{\"rx_control\":%u,\"pcs_reset\":%u,\"pma_reset\":%u,"
		"\"clock_control\":%u,\"cdr_control\":%u,\"rx_frequency\":%u,"
		"\"pll_status\":%u,\"tdc_control\":%u,\"rx_analog0\":%u,"
		"\"rx_analog1\":%u,\"rx_analog2\":%u,\"rx_sequence_force\":%u,"
		"\"rx_sequence_disable\":%u}}\n",
		s.controller_los ? "true" : "false", s.phy_los ? "true" : "false",
		s.synced ? "true" : "false", s.sync_status, s.frames, s.lof,
		s.fec_total, s.fec_corrected, s.fec_uncorrected,
		s.irq_calls, s.poll_calls, (unsigned long long)s.sampled_ms,
		s.reacquire_enabled ? "true" : "false", s.reacquire_attempts,
		s.receiver.rx_control, s.receiver.pcs_reset, s.receiver.pma_reset,
		s.receiver.clock_control, s.receiver.cdr_control, s.receiver.rx_frequency,
		s.receiver.pll_status, s.receiver.tdc_control, s.receiver.rx_analog0,
		s.receiver.rx_analog1, s.receiver.rx_analog2, s.receiver.rx_sequence_force,
		s.receiver.rx_sequence_disable);
}
static const struct kernel_param_ops qrx_status_ops = { .get = qrx_status_get };
module_param_cb(rx_bench_status, &qrx_status_ops, NULL, 0400);
