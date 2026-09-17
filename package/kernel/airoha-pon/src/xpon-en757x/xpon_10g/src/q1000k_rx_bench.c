// SPDX-License-Identifier: GPL-2.0-only
/* Explicit RX-only RAM bench: no registration executor or MAC interrupts. */
#include <linux/module.h>
#include <linux/ktime.h>
#include <q1000k_trace.h>
#include "common/q1000k_gwan.h"
#include "common/q1000k_mac_cold.h"
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
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
MODULE_PARM_DESC(rx_probe, "RX bench experiment enum, immutable, one attempt per load");

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

static int qrx_status_format(char *buffer, const struct q1000k_rx_sample *sample, u32 mask)
{
	struct q1000k_rx_sample s = *sample;
	char power[16];

	if (s.rx_power_valid)
		scnprintf(power, sizeof(power), "%u", s.rx_power_nw);
	else
		scnprintf(power, sizeof(power), "null");
	return scnprintf(buffer, PAGE_SIZE,
		"{\"rx_bench\":%s,\"registration_enabled\":%s,"
		"\"tx_inhibited\":%s,\"tx_enabled\":%s,\"mac_irq_mask\":%u,"
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
		s.rx_bench ? "true" : "false", s.rx_bench ? "false" : "true",
		s.tx_inhibited ? "true" : "false", s.tx_enabled ? "true" : "false", mask,
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
static int qrx_status_get(char *buffer, const struct kernel_param *kp)
{
	struct q1000k_rx_sample s;
	u32 mask;
	int ret;

	if (!rx_bench) return -EOPNOTSUPP;
	ret = q1000k_protocol_status();
	if (ret) return ret;
	if (!q1000k_transport_running()) return -EAGAIN;
	ret = q1000k_phy_rx_sample(&s);
	if (ret) return ret;
	mask = get_xpon_data(0x5040);
	ret = an7581_xpon_status();
	if (ret || mask) return ret ?: -EIO;
	return qrx_status_format(buffer, &s, mask);
}
static const struct kernel_param_ops qrx_status_ops = { .get = qrx_status_get };
module_param_cb(rx_bench_status, &qrx_status_ops, NULL, 0400);

/* Separate attribute: maximum u32 values plus all keys fit below PAGE_SIZE. */
static int qrx_diagnostics_format(char *buffer, const struct q1000k_rx_diagnostics *sample)
{
	struct q1000k_rx_diagnostics s = *sample;

	return scnprintf(buffer, PAGE_SIZE,
		"{\"diagnostics_version\":5,\"probe\":%u,\"attempts\":%u,\"writes\":%u,"
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
static int qrx_diagnostics_get(char *buffer, const struct kernel_param *kp)
{
	struct q1000k_rx_diagnostics s;
	int ret;

	if (!rx_bench) return -EOPNOTSUPP;
	ret = q1000k_protocol_status();
	if (ret) return ret;
	if (!q1000k_transport_running()) return -EAGAIN;
	ret = q1000k_phy_rx_diagnostics(&s);
	return ret ?: qrx_diagnostics_format(buffer, &s);
}
static const struct kernel_param_ops qrx_diagnostics_ops = { .get = qrx_diagnostics_get };
module_param_cb(rx_bench_diagnostics, &qrx_diagnostics_ops, NULL, 0400);

/* Two JSON records from a single PHY callback-locked capture. seq_file avoids
 * PAGE_SIZE truncation while retaining the existing per-record schemas.
 * No cache or reader can replace the diagnostic half of another reader's pair.
 */
static struct proc_dir_entry *qrx_snapshot_entry;
static int qrx_snapshot_unavailable(struct seq_file *seq, int error)
{
 if (error != -EAGAIN) return error;
 seq_printf(seq, "{\"snapshot_available\":false,\"error\":%d,\"sampled_ns\":%llu}\n",
  error, ktime_get_boottime_ns());
 return 0;
}
static int qrx_snapshot_show(struct seq_file *seq, void *unused)
{
	struct q1000k_rx_sample rx;
	struct q1000k_rx_diagnostics diag;
	char *buffer;
	u32 mask;
	int ret = q1000k_protocol_status();

	if (ret) return qrx_snapshot_unavailable(seq, ret);
	if (!q1000k_transport_running()) return qrx_snapshot_unavailable(seq, -EAGAIN);
	ret = q1000k_phy_snapshot(&rx, &diag);
	if (ret) return qrx_snapshot_unavailable(seq, ret);
	mask = get_xpon_data(0x5040);
	ret = an7581_xpon_status();
	if (ret || mask == ~0U || (rx.rx_bench && mask)) return ret ?: -EIO;
	buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buffer) return -ENOMEM;
	qrx_status_format(buffer, &rx, mask);
	seq_puts(seq, buffer);
	qrx_diagnostics_format(buffer, &diag);
	seq_puts(seq, buffer);
	kfree(buffer);
	return 0;
}

static struct proc_dir_entry *qrx_last_entry, *qrx_mac_entry, *qrx_fast_entry;
static int qrx_last_show(struct seq_file *seq, void *unused)
{
	struct q1000k_rx_sample rx;
	struct q1000k_rx_diagnostics diag;
	int fault, ret = q1000k_phy_last_snapshot(&rx, &diag, &fault);
	char *buffer;
	if (ret) return ret;
	buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buffer) return -ENOMEM;
	seq_printf(seq, "{\"cached_snapshot\":true,\"mac_irq_mask_unavailable\":true,\"phy_fault\":%d}\n", fault);
	qrx_status_format(buffer, &rx, 0); seq_puts(seq, buffer);
	qrx_diagnostics_format(buffer, &diag); seq_puts(seq, buffer);
	kfree(buffer);
	return 0;
}
static int qrx_fast_show(struct seq_file *seq, void *unused)
{
	u32 sfp, sync, frames;
	u64 begin = ktime_get_boottime_ns();
	int ret = q1000k_phy_fast_sample(&sfp, &sync, &frames);
	if (ret == -EAGAIN) {
  seq_printf(seq, "{\"fast_version\":2,\"available\":false,\"error\":%d,\"begin_ns\":%llu,\"end_ns\":%llu}\n",
   ret, begin, ktime_get_boottime_ns());
  return 0;
 }
 if (ret) return ret;
 seq_printf(seq, "{\"fast_version\":2,\"available\":true,\"begin_ns\":%llu,\"end_ns\":%llu,\"sfp\":%u,\"sync\":%u,\"frames\":%u}\n",
		begin, ktime_get_boottime_ns(), sfp, sync, frames);
	return 0;
}

/* Config words are the existing cold-install/readback registers. Counters use
 * the existing gponDevGetGtcCounter read semantics (no latch or read-clear).
 * No interrupt/error ACK or PLOAM FIFO data port is read here. */
static int qrx_mac_show(struct seq_file *seq, void *unused)
{
	static const u32 regs[] = { 0x5100, 0x5104, 0x5108, 0x511c, 0x5120, 0x5124,
		0x509c, 0x510c, 0x5128, 0x5318, 0x5284, 0x52f0,
  0x5920, 0x5944, 0x5950, 0x5954, 0x5960, 0x5964, 0x5968, 0x596c, 0x5984 };
	u32 values[ARRAY_SIZE(regs)], generation;
	u64 begin = ktime_get_boottime_ns();
	unsigned int i;
	int token = q1000k_protocol_enter(), ret;
	if (token < 0) return token;
	ret = an7581_xpon_status();
	for (i = 0; !ret && i < ARRAY_SIZE(regs); i++) {
		values[i] = get_xpon_data(regs[i]);
		ret = an7581_xpon_status();
		if (!ret && i < 12 && values[i] == ~0U) ret = -EIO;
	}
	generation = q1000k_mac_generation();
	q1000k_protocol_leave(token);
	if (ret) return ret;
	seq_printf(seq, "{\"hardware_generation\":%u,\"mac_version\":2,\"begin_ns\":%llu,\"end_ns\":%llu,\"registers\":{",
		generation, begin, ktime_get_boottime_ns());
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(seq, "%s\"%04x\":%u", i ? "," : "", regs[i], values[i]);
	seq_puts(seq, "}}\n");
	return 0;
}

/* Called by the serialized MAC IRQ worker at discovery interrupts; never
 * I2C, FIFO reads, W1C ACKs, key/identity words, or measurement triggers. */
void q1000k_discovery_snapshot(u32 interrupts)
{
 static const u32 regs[] = { 0x509c, 0x5100, 0x5104, 0x5108, 0x510c,
  0x511c, 0x5120, 0x5124, 0x5128, 0x5318, 0x5944, 0x5954, 0x5284, 0x52f0 };
 unsigned int i;
 if (!q1000k_protocol_owned()) return;
 for (i = 0; i < ARRAY_SIZE(regs); i++) {
  u32 value = get_xpon_data(regs[i]);
  int ret = an7581_xpon_status();
  if (!ret && i != 10 && i != 11 && value == ~0U) ret = -EIO;
  q1000k_trace(QT_DISCOVERY, i, ret, regs[i], value, interrupts, q1000k_mac_generation());
  if (ret) break;
 }
}

static int qrx_recover_drained(void *arg)
{
	return q1000k_phy_bench_recover(*(unsigned int *)arg);
}
static int qrx_recover_set(const char *value, const struct kernel_param *kp)
{
	struct q1000k_rx_sample rx;
	unsigned int action;
	int token, ret = kstrtouint(value, 10, &action);
	if (ret) return ret;
	if (!rx_bench || action < 1 || action > 6) return -EPERM;
	token = q1000k_protocol_enter();
	if (token < 0) return token;
	ret = q1000k_phy_rx_sample(&rx);
	if (!ret && (!rx.tx_inhibited || rx.tx_enabled)) ret = -EACCES;
	if (!ret && (rx.controller_los || rx.phy_los || rx.synced)) ret = -EAGAIN;
	if (!ret) ret = action <= 4 ? q1000k_phy_bench_recover(action) :
		q1000k_gwan_refresh(qrx_recover_drained, &action);
	q1000k_protocol_leave(token);
	return ret;
}
static const struct kernel_param_ops qrx_recover_ops = { .set = qrx_recover_set };
module_param_cb(bench_recover, &qrx_recover_ops, NULL, 0200);

int q1000k_snapshot_init(void)
{
	if (qrx_snapshot_entry) return -EBUSY;
	qrx_snapshot_entry = proc_create_single("q1000k-pon-snapshot", 0400, NULL,
					     qrx_snapshot_show);
	qrx_last_entry = proc_create_single("q1000k-pon-last-snapshot", 0400, NULL, qrx_last_show);
	qrx_mac_entry = proc_create_single("q1000k-pon-mac", 0400, NULL, qrx_mac_show);
	qrx_fast_entry = proc_create_single("q1000k-pon-fast", 0400, NULL, qrx_fast_show);
	if (!qrx_snapshot_entry || !qrx_last_entry || !qrx_mac_entry || !qrx_fast_entry) {
		q1000k_snapshot_exit();
		return -ENOMEM;
	}
	return 0;
}

void q1000k_snapshot_exit(void)
{
	proc_remove(qrx_fast_entry); qrx_fast_entry = NULL;
	proc_remove(qrx_mac_entry); qrx_mac_entry = NULL;
	proc_remove(qrx_last_entry); qrx_last_entry = NULL;
	proc_remove(qrx_snapshot_entry);
	qrx_snapshot_entry = NULL;
}
