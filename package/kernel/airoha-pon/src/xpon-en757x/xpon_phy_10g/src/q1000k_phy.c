#include <q1000k_trace.h>
// SPDX-License-Identifier: GPL-2.0-only
/* Sleepable Q1000K PHY control and callback lifetime. */
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>
#include <an7581_pon_phy.h>
#include <an7581_pon_scu.h>
#include <q1000k_pon.h>
#include <ecnt_pon_phy_api.h>
#include <ecnt_scu_api.h>
#include <q1000k_phy_api.h>
#include "phy_global.h"
#include "phy_debug.h"
#include "i2c.h"
#include "phy_init.h"
#include "phy_reg.h"
#include "en7581.h"
#include "en7581_reg.h"

static DEFINE_MUTEX(qphy_control);
static DEFINE_MUTEX(qphy_callback);
static struct task_struct *qphy_owner;
static struct q1000k_pon *qphy_controller;
static bool qphy_active, qphy_dead;
static int qphy_fault;
static struct q1000k_rx_sample qphy_last_rx;
static struct q1000k_rx_diagnostics qphy_last_diag;
static bool qphy_last_valid;
static u32 qphy_manual_used;
static u32 qphy_edges = ~0U;
static struct device *qphy_irq_dev;
static int qphy_irq = -1;
static bool qphy_rx_bench;
static bool qphy_rx_reacquire;
static bool qphy_rx_restore_pll;
static bool qphy_rx_restore_gain;
static u32 qphy_rx_probe_mode;
static u32 qphy_rx_attempts, qphy_rx_no_sync;
static bool qphy_rx_seen_light;
static u32 qphy_rx_irqs, qphy_rx_polls;
static const char *qphy_repeat_reason;
static u64 qphy_repeat_next_ms;
#define QPHY_RX_BENCH_IRQS (EN7581_XGPON_PHY_RX_RDY_INT_EN | \
	EN7581_XGPON_PHY_RX_LOF_INT_EN | EN7581_XGPON_PHY_RX_SYNC_OK_INT_EN | \
	EN7581_XGPON_PHY_RX_LOS_INT_EN)

static int qphy_rx_sample(struct q1000k_rx_sample *sample);

static int qphy_context(void)
{
	/* Without lockdep, rcu_read_lock_held() is unconditionally true. */
	return in_interrupt() || in_atomic() || irqs_disabled() ||
		rcu_preempt_depth() ||
		(IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()) ?
		-EWOULDBLOCK : 0;
}

/* Internal vendor helpers may nest, but must belong to this callback. */
int q1000k_phy_callback_context(void)
{
	int ret = qphy_context();

	return ret ? ret : READ_ONCE(qphy_owner) == current ? 0 : -EPERM;
}

static void qphy_callback_lock(void)
{
	mutex_lock(&qphy_callback);
	WRITE_ONCE(qphy_owner, current);
}

static void qphy_callback_unlock(void)
{
	WRITE_ONCE(qphy_owner, NULL);
	mutex_unlock(&qphy_callback);
}

static int qphy_ready(void)
{
	if (!gpPhyPriv || qphy_dead)
		return -ENODEV;
	if (qphy_fault)
		return qphy_fault;
	return an7581_pon_phy_status();
}

static int qphy_reg_write(u32 reg, u32 value)
{
	u32 actual;
	int ret;

	ret = an7581_pon_phy_write(reg, value);
	if (ret) {
		pr_err_ratelimited("q1000k: PHY register write failed: stage=write reg=%#x expected=%#x error=%d\n",
				   reg, value, ret);
		return ret;
	}
	ret = an7581_pon_phy_read(reg, &actual);
	if (ret) {
		pr_err_ratelimited("q1000k: PHY register write failed: stage=read reg=%#x expected=%#x error=%d\n",
				   reg, value, ret);
		return ret;
	}
	if (actual != value) {
		pr_err_ratelimited("q1000k: PHY register write failed: stage=readback reg=%#x expected=%#x actual=%#x error=%d\n",
				   reg, value, actual, -EIO);
		return -EIO;
	}
	return 0;
}

int q1000k_phy_controller_check(void)
{
	int ret = q1000k_phy_callback_context();

	return ret ? ret : qphy_controller ? q1000k_pon_check(qphy_controller) : -ENODEV;
}

int q1000k_phy_controller_oem_post(bool restore)
{
	int ret = q1000k_phy_controller_check();

	return ret ?: q1000k_pon_oem_post_init(qphy_controller, restore);
}

int q1000k_phy_board_profile(void)
{
	int ret = q1000k_phy_controller_check();

	if (ret)
		return ret;
	if (gpPhyPriv->wan_sel != SCU_WAN_CONF_REG_WAN_SEL_XGSPON)
		return -EINVAL;
	/* OEM Q1000K boot selects table index 82 (ECONET / EN7572) for
	 * its EN7573 pair. These exact values agree with the imported table.
	 * No generic SFP probe may access the controller's owned I2C address.
	 */
	ret = qphy_reg_write(EN7581_XGPON_PHY_SFP_VLD_LEVEL, 0x9);
	if (!ret)
		ret = qphy_reg_write(EN7581_XPON_PMA_XPON_SETTING_0, 0x10001);
	if (!ret)
		ret = qphy_reg_write(EN7581_XPON_PMA_XPON_SETTING_1, 0x1010100);
	if (!ret)
		gpPhyPriv->trans_index = 82;
	return ret;
}

int q1000k_phy_trans_power(u32 operation)
{
	bool preserve = operation == PHY_TX_DIS_ON_HW_ONLY;
	int ret = q1000k_phy_callback_context();

	if (ret)
		return ret;
	if (!qphy_controller)
		return -ENODEV;
	if (operation == PHY_TX_DIS_RESTORE_BY_SW)
		operation = gpPhyPriv->trans_tx_status;
	else if (preserve)
		operation = PHY_DISABLE;
	if (operation != PHY_ENABLE && operation != PHY_DISABLE)
		return -EINVAL;
	/* Only coordinated startup may authorize a later internal restore. */
	if (operation == PHY_ENABLE &&
	    (qphy_rx_bench || !READ_ONCE(qphy_active) || !gpPhyPriv->phyCfg.flags.txPowerEnFlag))
		return -EACCES;
	ret = q1000k_pon_set_tx(qphy_controller, operation == PHY_ENABLE);
	q1000k_trace(QT_TX, operation == PHY_ENABLE, ret, qphy_rx_bench, qphy_active, 3, operation);
	if (!ret && !preserve)
		gpPhyPriv->trans_tx_status = operation;
	return ret;
}

/* These masks are owned by the optical PHY, never the copper SerDes. */
static int qphy_mask(void)
{
	static const u32 masks[] = {
		EN7581_XGPON_PHY_XG_PON_INT_EN,
		EN7581_XPON_PMA_XPON_INT_EN_0,
		EN7581_XPON_PMA_XPON_INT_EN_1,
		EN7581_XPON_PMA_XPON_INT_EN_2,
		EN7581_XPON_PMA_XPON_INT_EN_3,
		EN7581_XPON_PMA_XPON_INT_EN_4,
	};
	int i, ret, first = 0;

	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		ret = qphy_reg_write(masks[i], 0);
		if (ret && !first)
			first = ret;
	}
	return first;
}

/* Callback mutex owns the entire series and its stop latch. A single observed
 * sync (including an IRQ/user sample) prevents any later acquisition reset.
 */
static void qphy_repeat_stop(const char *reason)
{
	if (qphy_rx_probe_mode != Q1000K_RX_PROBE_OEM_RESET_REPEAT || qphy_repeat_reason)
		return;
	qphy_repeat_reason = reason;
	pr_info("q1000k: RX recovery stopped reason=%s attempts=%u\n", reason, qphy_rx_attempts);
}

static int qphy_repeat_boundary(const struct q1000k_rx_sample *s, const char *phase, u32 attempt)
{
	u32 ncpo;
	int ret = an7581_pon_phy_read(EN7581_XPON_PMA_SS_LCPLL_TDC_RO_4, &ncpo);

	if (ret)
		return ret;
	pr_info("q1000k: RX recovery phase=%s attempt=%u sampled_ms=%llu synced=%u controller_los=%u phy_los=%u frames=%u lof=%u fec_total=%u fec_corrected=%u fec_uncorrected=%u cw_start=%u cw_end=%u sof_to_mac=%u eof_to_mac=%u psync_mismatch=%u sfc_hec_error=%u pon_id_hec_error=%u ncpo=%u writes=%u\n",
		phase, attempt, (unsigned long long)s->sampled_ms, s->synced, s->controller_los, s->phy_los,
		s->frames, s->lof, s->fec_total, s->fec_corrected, s->fec_uncorrected,
		s->pcs_counters.cw_start, s->pcs_counters.cw_end,
		s->pcs_counters.sof_to_mac, s->pcs_counters.eof_to_mac,
		s->pcs_counters.psync_mismatch, s->pcs_counters.sfc_hec_error,
		s->pcs_counters.pon_id_hec_error, ncpo, q1000k_phy_rx_probe_writes());
	return 0;
}

static int qphy_repeat_poll(struct q1000k_rx_sample *sample)
{
	int ret;

	if (qphy_repeat_reason)
		return 0;
	if (sample->controller_los || sample->phy_los) {
		qphy_rx_no_sync = 0;
		return 0;
	}
	if (!qphy_rx_attempts) {
		if (++qphy_rx_no_sync < 10)
			return 0;
	} else if (sample->sampled_ms < qphy_repeat_next_ms) {
		return 0;
	}
	if (qphy_rx_attempts >= Q1000K_RX_REPEAT_LIMIT) {
		qphy_repeat_stop("budget");
		return 0;
	}
	ret = qphy_repeat_boundary(sample, "before", qphy_rx_attempts + 1);
	if (ret)
		return ret;
	/* Consume the attempt before its first write; failures are never retried. */
	qphy_rx_attempts++;
	ret = q1000k_phy_rx_probe(Q1000K_RX_PROBE_OEM_RESET_REPEAT);
	if (!ret)
		ret = qphy_rx_sample(sample);
	if (!ret)
		ret = qphy_repeat_boundary(sample, "after", qphy_rx_attempts);
	if (ret) {
		pr_info("q1000k: RX recovery failure attempt=%u error=%d\n", qphy_rx_attempts, ret);
		return ret;
	}
	qphy_repeat_next_ms = ktime_to_ms(ktime_get_boottime()) + Q1000K_RX_REPEAT_INTERVAL_MS;
	if (qphy_rx_attempts == Q1000K_RX_REPEAT_LIMIT)
		qphy_repeat_stop("budget");
	return 0;
}

static void qphy_failed(int error)
{
	if (!error)
		return;
	qphy_repeat_stop("error");
	WRITE_ONCE(qphy_active, false);
	WRITE_ONCE(gpPhyPriv->pon_stop_flag, TRUE);
	WRITE_ONCE(gpPhyPriv->is_phy_start, FALSE);
	if (!qphy_fault) {
		q1000k_trace(QT_FAULT, 1, error, qphy_last_valid,
			qphy_last_rx.frames, qphy_last_rx.sync_status, qphy_manual_used);
		qphy_fault = error;
	}
	if (qphy_controller) {
		int tx_error = q1000k_pon_set_tx(qphy_controller, false);
		q1000k_trace(QT_TX, 0, tx_error, qphy_rx_bench, qphy_active, 2, 0);
	}
	qphy_mask();
}

static void qphy_poll_work(struct work_struct *work)
{
	struct q1000k_rx_sample sample;
	int ret;

	qphy_callback_lock();
	if (!READ_ONCE(qphy_active))
		goto out;
	q1000k_trace(QT_PHY_POLL, qphy_rx_bench, 0, qphy_rx_reacquire, qphy_rx_attempts, 0, 0);
	ret = q1000k_phy_controller_check();
	if (!ret && qphy_rx_bench) {
		qphy_rx_polls++;
		ret = qphy_rx_sample(&sample);
		if (!ret && qphy_rx_reacquire && READ_ONCE(qphy_active) &&
		    qphy_rx_probe_mode == Q1000K_RX_PROBE_OEM_RESET_REPEAT) {
			ret = qphy_repeat_poll(&sample);
		} else if (!ret && qphy_rx_reacquire && !qphy_rx_attempts && READ_ONCE(qphy_active)) {
			bool dark_checker = qphy_rx_probe_mode == Q1000K_RX_PROBE_CHECKER_DARK;
			bool eligible;

			if (!sample.controller_los && !sample.phy_los)
				qphy_rx_seen_light = true;
			/* Fresh dark checker after illuminated initialization. Bench LOS
			 * callbacks never invoke vendor power-save/insertion handlers.
			 * Mixed LOS and a synchronized receiver never consume the budget.
			 */
			eligible = dark_checker ? qphy_rx_seen_light && sample.controller_los && sample.phy_los :
				!sample.controller_los && !sample.phy_los;
			if (!eligible || sample.synced) {
				qphy_rx_no_sync = 0;
			} else if (++qphy_rx_no_sync == 10) {
				/* Consume the budget before touching hardware. The bounded
				 * PMA out/in path never enters the vendor polling handler,
				 * dispatches registration events, or resets the shared SCU.
				 */
				qphy_rx_attempts++;
				ret = qphy_rx_probe_mode ? q1000k_phy_rx_probe(qphy_rx_probe_mode) :
					q1000k_phy_rx_reacquire(qphy_rx_restore_pll, qphy_rx_restore_gain);
				if (!ret && READ_ONCE(qphy_active))
					ret = qphy_rx_sample(&sample);
			}
		}
		/* Quiesce may withdraw active while this callback owns the lock.
		 * It waits for us before releasing the controller and IRQ resources.
		 */
		if (ret == -EAGAIN && !READ_ONCE(qphy_active))
			ret = 0;
	} else if (!ret) {
		ret = ponPhyFunc[PHY_EVENT_POLL_FUNC]((char *)gpPhyPriv);
	}
	if (!ret)
		ret = an7581_pon_phy_status();
	if (ret)
		qphy_failed(ret);
	else if (READ_ONCE(qphy_active))
		mod_timer(&gpPhyPriv->event_poll_timer,
			  jiffies + msecs_to_jiffies(1500));
out:
	qphy_callback_unlock();
}
static DECLARE_WORK(qphy_poll_job, qphy_poll_work);

void q1000k_phy_poll(void)
{
	if (READ_ONCE(qphy_active))
		schedule_work(&qphy_poll_job);
}

static irqreturn_t qphy_irq_thread(int irq, void *data)
{
	struct q1000k_rx_sample sample;
	u32 status, enabled, rogue, rogue_en;
	int ret;
	irqreturn_t handled = IRQ_NONE;

	qphy_callback_lock();
	if (!READ_ONCE(qphy_active))
		goto out;
	ret = an7581_pon_phy_read(EN7581_XGPON_PHY_XG_PON_INT_STA, &status);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_XG_PON_INT_EN, &enabled);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XPON_PMA_XPON_INT_STA_0, &rogue);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XPON_PMA_XPON_INT_EN_0, &rogue_en);
	if (ret)
		goto fail;
	if (status == ~0U || enabled == ~0U || rogue == ~0U || rogue_en == ~0U) {
		ret = -EIO;
		goto fail;
	}
	if (!(status & enabled) && !(rogue & rogue_en))
		goto out;
	q1000k_trace(QT_PHY_IRQ, qphy_rx_bench, 0, status, enabled, rogue, rogue_en);
	handled = IRQ_HANDLED;
	/* Skip the legacy interrupt-count heuristic and its 50 ms busy wait.
	 * Dispatch only after checking this PHY's enabled pending sources.
	 */
	ret = q1000k_phy_controller_check();
	if (!ret && qphy_rx_bench) {
		/* Never dispatch vendor LOS/ready events into registration. Only
		 * acknowledge our enabled RX W1C sources; PMA/TX remain masked.
		 */
		if (enabled != QPHY_RX_BENCH_IRQS || rogue_en)
			ret = -EACCES;
		else
			ret = an7581_pon_phy_write(EN7581_XGPON_PHY_XG_PON_INT_STA,
						 status & enabled);
		if (!ret) {
			qphy_rx_irqs++;
			ret = qphy_rx_sample(&sample);
		}
	} else if (!ret) {
		ret = ponPhyFunc[PHY_ISR_FUNC]((char *)gpPhyPriv);
	}
	if (!ret)
		ret = an7581_pon_phy_status();
fail:
	if (ret)
		qphy_failed(ret);
out:
	qphy_callback_unlock();
	return handled;
}

int q1000k_phy_prepare_wan(void)
{
	u32 mode;
	bool enabled;
	int ret = qphy_context();

	if (ret)
		return ret;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret)
		goto out;
	ret = an7581_pon_wan_get(&mode);
	if (ret || mode == SCU_WAN_CONF_REG_WAN_SEL_XGSPON)
		goto out;
	/* The second-stage Q1000K bootloader leaves the unused PON lane in
	 * USXGMII mode. Its XG MAC register bank is inaccessible in that mode.
	 * Only this known, unconfigured handoff may precede the physical drain.
	 * Copper Ethernet uses separate PCS/SCU selectors, which we never write.
	 */
	if (mode != 0x12) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (READ_ONCE(qphy_active) || qphy_irq_dev || gpPhyPriv->phy_init_done) {
		ret = -EBUSY;
		goto out;
	}
	if (!qphy_controller) {
		struct q1000k_pon *controller = q1000k_pon_get();

		if (IS_ERR(controller)) {
			ret = PTR_ERR(controller);
			goto out;
		}
		qphy_controller = controller;
	}
	ret = q1000k_pon_get_tx(qphy_controller, &enabled);
	if (!ret && enabled)
		ret = -EBUSY;
	if (ret)
		goto out;
	ret = an7581_pon_wan_set(SCU_WAN_CONF_REG_WAN_SEL_XGSPON);
	if (ret)
		qphy_failed(ret);
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_prepare_wan);

int q1000k_phy_needs_configure(void)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret)
		ret = !gpPhyPriv->phy_init_done;
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_needs_configure);

int q1000k_phy_configure(u32 mode)
{
	bool enabled;
	int ret = qphy_context();

	if (ret)
		return ret;
	if (mode != PHY_XGSPON_CONFIG)
		return -EOPNOTSUPP;
	/* Deferred MAC events may arrive before the PHY callback returns.
	 * Wait for that callback; reject recursion before taking control.
	 */
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret)
		goto out;
	if (READ_ONCE(qphy_active) || qphy_irq_dev) {
		ret = -EBUSY;
		goto out;
	}
	if ((GET_WAN_CONF() & SCU_WAN_CONF_REG_WAN_SEL_BITS) !=
	    SCU_WAN_CONF_REG_WAN_SEL_XGSPON) {
		ret = -EINVAL;
		goto out;
	}
	if (!qphy_controller) {
		struct q1000k_pon *controller = q1000k_pon_get();

		if (IS_ERR(controller)) {
			ret = PTR_ERR(controller);
			goto out;
		}
		qphy_controller = controller;
	}
	ret = q1000k_phy_controller_check();
	if (!ret)
		ret = q1000k_pon_get_tx(qphy_controller, &enabled);
	if (!ret && enabled)
		ret = -EBUSY;
	if (!ret)
		ret = an7581_pon_phy_prepare_pins();
	if (!ret)
		ret = an7581_pon_pbus_enable();
	if (ret)
		goto configure_failed;
	/* Optical TX stays disabled; its enable belongs to controller/MAC startup. */
	q1000k_trace_generation(1);
	ret = phy_mode_config(PHY_XGSPON_CONFIG, PHY_DISABLE);
	if (!ret)
		ret = qphy_mask();
	if (!ret)
		ret = an7581_pon_phy_status();
	configure_failed:
	if (ret) {
		gpPhyPriv->phy_init_done = FALSE;
		qphy_failed(ret);
	}
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_configure);

int q1000k_phy_start(void)
{
	struct device *dev;
	int irq, ret = qphy_context();

	if (ret)
		return ret;
	/* Deferred MAC events may arrive before the PHY callback returns.
	 * Wait for that callback; reject recursion before taking control.
	 */
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret)
		goto out;
	if (READ_ONCE(qphy_active))
		goto out;
	if (!gpPhyPriv->phy_init_done) {
		ret = -EAGAIN;
		goto out;
	}
	ret = q1000k_phy_controller_check();
	if (ret)
		goto fail;
	dev = get_pon_phy_dev();
	irq = get_pon_phy_irq();
	if (!dev || irq < 0) {
		ret = irq < 0 ? irq : -ENODEV;
		goto out;
	}
	ret = qphy_mask();
	if (ret)
		goto fail;
	ret = pon_phy_clear_int();
	if (!ret)
		ret = an7581_pon_phy_status();
	if (ret)
		goto fail;
	ret = request_threaded_irq(irq, NULL, qphy_irq_thread,
				   IRQF_ONESHOT | IRQF_SHARED, "q1000k-phy", dev);
	if (ret)
		goto out; /* Masked and retryable; no IRQ ownership was acquired. */
	qphy_irq = irq;
	qphy_irq_dev = dev;
	gpPhyPriv->is_irq_requested = TRUE;
	gpPhyPriv->pon_stop_flag = FALSE;
	gpPhyPriv->is_phy_start = TRUE;
	gpPhyPriv->phy_status = PHY_LINK_STATUS_UNKNOWN;
	/* The IRQ thread may run as soon as the mask is enabled. */
	qphy_rx_no_sync = 0;
	qphy_rx_seen_light = false;
	WRITE_ONCE(qphy_active, true);
	ret = qphy_reg_write(EN7581_XGPON_PHY_XG_PON_INT_EN,
		(qphy_rx_bench ? QPHY_RX_BENCH_IRQS :
		EN7581_XGPON_PHY_TX_FAULT_INT_EN |
		EN7581_XGPON_PHY_TX_BURST_SPACE_ERR_INT_EN |
		EN7581_XGPON_PHY_TX_MPI_ERR_INT_EN |
		EN7581_XGPON_PHY_TX_PSBU_INFO_ERR_INT_EN |
		EN7581_XGPON_PHY_TX_INFO_FIFO_INT_EN |
		EN7581_XGPON_PHY_RX_RDY_INT_EN |
		EN7581_XGPON_PHY_RX_INFO_FIFO_INT_EN |
		EN7581_XGPON_PHY_RX_NGPON2_OC_ERR_INT_EN |
		EN7581_XGPON_PHY_RX_BER_HIGH_INT_EN |
		EN7581_XGPON_PHY_RX_LOF_INT_EN |
		EN7581_XGPON_PHY_RX_SYNC_OK_INT_EN |
		EN7581_XGPON_PHY_RX_LOS_INT_EN));
	if (!ret)
		ret = an7581_pon_phy_status();
	if (ret) {
		WRITE_ONCE(qphy_active, false);
		qphy_mask();
		qphy_callback_unlock();
		free_irq(qphy_irq, qphy_irq_dev);
		qphy_callback_lock();
		qphy_irq_dev = NULL;
		qphy_irq = -1;
		gpPhyPriv->is_irq_requested = FALSE;
		goto fail;
	}
	mod_timer(&gpPhyPriv->event_poll_timer, jiffies + msecs_to_jiffies(1500));
	goto out;
fail:
	qphy_failed(ret);
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_start);

/* Called with control held. Callback code never waits for control: callbacks
 * trying to reenter a lifecycle API receive -EDEADLK. Do not hold callback over
 * free_irq/cancel_work_sync, which wait for that same code to finish.
 */
static int qphy_stop(void)
{
	const char *failed_phase = NULL;
	int prior_fault = READ_ONCE(qphy_fault);
	int ret = 0, err;

	if (!gpPhyPriv) {
		pr_err_ratelimited("q1000k: PHY stop failed: phase=no-private-state error=%d prior_fault=%d\n",
				   -ENODEV, prior_fault);
		return -ENODEV;
	}
	WRITE_ONCE(qphy_active, false);
	WRITE_ONCE(gpPhyPriv->pon_stop_flag, TRUE);
	WRITE_ONCE(gpPhyPriv->is_phy_start, FALSE);
	if (gpPhyPriv->phy_init_done || qphy_irq_dev) {
		ret = qphy_mask();
		if (ret)
			failed_phase = "mask-before-drain";
	}
	if (qphy_irq_dev) {
		free_irq(qphy_irq, qphy_irq_dev);
		qphy_irq_dev = NULL;
		qphy_irq = -1;
	}
	gpPhyPriv->is_irq_requested = FALSE;
	timer_delete_sync(&gpPhyPriv->event_poll_timer);
	cancel_work_sync(&qphy_poll_job);
	/* A poll already running when stop began may have rearmed this timer. */
	timer_delete_sync(&gpPhyPriv->event_poll_timer);
	qphy_callback_lock();
	if (qphy_rx_attempts || qphy_rx_no_sync)
		qphy_repeat_stop("shutdown");
	if (qphy_controller) {
		err = q1000k_pon_set_tx(qphy_controller, false);
		q1000k_trace(QT_TX, 0, err, qphy_rx_bench, qphy_active, 1, 0);
		if (!ret) {
			ret = err;
			if (ret)
				failed_phase = "controller-tx-disable";
		}
		gpPhyPriv->phyCfg.flags.txPowerEnFlag = false;
		gpPhyPriv->trans_tx_status = PHY_DISABLE;
	}
	if (gpPhyPriv->phy_init_done) {
		err = qphy_mask();
		if (!ret) {
			ret = err;
			if (ret)
				failed_phase = "mask-after-drain";
		}
	}
	if (!ret) {
		ret = q1000k_phy_rx_cleanup();
		if (ret)
			failed_phase = "rx-gain-restore";
	}
	if (!ret) {
		ret = q1000k_phy_rx_probe_cleanup();
		if (ret)
			failed_phase = "rx-probe-restore";
	}
	if (!ret) {
		ret = an7581_pon_phy_status();
		if (ret)
			failed_phase = "provider-status";
	}
	if (ret)
		qphy_failed(ret);
	if (!ret) {
		ret = qphy_fault;
		if (ret)
			failed_phase = "sticky-fault";
	}
	if (ret)
		pr_err_ratelimited("q1000k: PHY stop failed: phase=%s error=%d prior_fault=%d sticky_fault=%d\n",
				   failed_phase, ret, prior_fault, qphy_fault);
	qphy_callback_unlock();
	return ret;
}

int q1000k_phy_stop(void)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	ret = qphy_stop();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_stop);

/* The MAC lifecycle worker has drained event/control producers before entry.
 * A callback cannot wait on its own IRQ/work completion.
 */
int q1000k_phy_quiesce(void)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	ret = qphy_stop();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_quiesce);

int q1000k_phy_set_tx(bool enable)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret && (!qphy_controller || !gpPhyPriv->phy_init_done))
		ret = -EAGAIN;
	if (!ret && enable && qphy_rx_bench) {
		ret = -EACCES;
		qphy_failed(ret);
	}
	if (!ret && enable && !READ_ONCE(qphy_active))
		ret = -EAGAIN;
	if (!ret) {
		ret = q1000k_pon_set_tx(qphy_controller, enable);
		q1000k_trace(QT_TX, enable, ret, qphy_rx_bench, qphy_active, 0, 0);
		if (!ret) {
			gpPhyPriv->phyCfg.flags.txPowerEnFlag = enable;
			gpPhyPriv->trans_tx_status = enable ? PHY_ENABLE : PHY_DISABLE;
		} else {
			qphy_failed(ret);
		}
	}
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_set_tx);

int q1000k_phy_get_tx(bool *enabled)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (!enabled)
		return -EINVAL;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret && (!qphy_controller || !gpPhyPriv->phy_init_done))
		ret = -EAGAIN;
	if (!ret) {
		ret = q1000k_pon_get_tx(qphy_controller, enabled);
		if (ret)
			qphy_failed(ret);
	}
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_get_tx);

int q1000k_phy_get_rx_power(u32 *nanowatts)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (!nanowatts)
		return -EINVAL;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret && (!qphy_controller || !gpPhyPriv->phy_init_done || !qphy_active))
		ret = -EAGAIN;
	if (!ret) {
		ret = q1000k_pon_get_rx_power(qphy_controller, nanowatts);
		if (ret && ret != -ENODATA)
			qphy_failed(ret);
	}
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_get_rx_power);

int q1000k_phy_set_rx_bench(bool enabled, bool reacquire, bool restore_pll, bool restore_gain)
{
	bool inhibited, tx;
	int ret = qphy_context();

	if (ret)
		return ret;
	if ((reacquire && !enabled) || ((restore_pll || restore_gain) && !reacquire))
		return -EINVAL;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret || (enabled == qphy_rx_bench && reacquire == qphy_rx_reacquire &&
		    restore_pll == qphy_rx_restore_pll && restore_gain == qphy_rx_restore_gain))
		goto out;
	if (qphy_active || qphy_irq_dev || gpPhyPriv->phy_init_done) {
		ret = -EBUSY;
		goto out;
	}
	if (enabled) {
		if (!qphy_controller) {
			struct q1000k_pon *controller = q1000k_pon_get();

			if (IS_ERR(controller)) { ret = PTR_ERR(controller); goto out; }
			qphy_controller = controller;
		}
		ret = q1000k_pon_get_tx_inhibit(qphy_controller, &inhibited);
		if (!ret)
			ret = q1000k_pon_get_tx(qphy_controller, &tx);
		if (!ret && (!inhibited || tx))
			ret = -EACCES;
		if (ret)
			goto out;
	}
	if (qphy_rx_probe_mode && (!enabled || !reacquire || restore_pll || restore_gain)) {
		ret = -EINVAL;
		goto out;
	}
	qphy_rx_bench = enabled;
	qphy_rx_reacquire = reacquire;
	qphy_rx_restore_pll = restore_pll;
	qphy_rx_restore_gain = restore_gain;
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_set_rx_bench);

int q1000k_phy_set_rx_probe(u32 probe)
{
	bool inhibited, tx;
	int ret = qphy_context();

	if (ret) return ret;
	if (probe >= Q1000K_RX_PROBE_COUNT) return -EINVAL;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret || probe == qphy_rx_probe_mode) goto out;
	if (qphy_active || qphy_irq_dev || gpPhyPriv->phy_init_done) {
		ret = -EBUSY;
		goto out;
	}
	if (probe) {
		if (!qphy_rx_bench || !qphy_rx_reacquire || qphy_rx_restore_pll ||
		    qphy_rx_restore_gain || !qphy_controller) {
			ret = -EINVAL;
			goto out;
		}
		ret = q1000k_pon_get_tx_inhibit(qphy_controller, &inhibited);
		if (!ret) ret = q1000k_pon_get_tx(qphy_controller, &tx);
		if (!ret && (!inhibited || tx)) ret = -EACCES;
		if (ret) goto out;
	}
	qphy_rx_probe_mode = probe;
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_set_rx_probe);

/* Caller holds both control and callback locks. */
static int qphy_diagnostics(struct q1000k_rx_diagnostics *sample)
{
	struct q1000k_rx_diagnostics result = {};
	int ret;

	if (!qphy_active || !gpPhyPriv->phy_init_done)
		return -EAGAIN;
	ret = q1000k_phy_controller_check();
#define QDIAG_READ(name, reg) if (!ret) ret = an7581_pon_phy_read(reg, &result.name);
	Q1000K_RX_DIAG_FIELDS(QDIAG_READ)
#undef QDIAG_READ
	if (!ret) ret = an7581_pon_phy_status();
	if (!ret && ((result.checker_control & BIT(8)) ||
		    (result.data_route_control & (BIT(16) | BIT(8))) ||
		    (result.bist_lane_control & BIT(8)))) ret = -EACCES;
	if (!ret) {
		result.sampled_ms = ktime_to_ms(ktime_get_boottime());
		result.probe = qphy_rx_probe_mode;
		result.attempts = qphy_rx_attempts;
		result.writes = q1000k_phy_rx_probe_writes();
		*sample = result;
	}
	return ret;
}

int q1000k_phy_rx_diagnostics(struct q1000k_rx_diagnostics *sample)
{
	int ret = qphy_context();

	if (ret) return ret;
	if (!sample) return -EINVAL;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret) goto out;
	if (!ret) ret = qphy_rx_bench ? qphy_diagnostics(sample) : -EAGAIN;
	if (ret && ret != -EAGAIN) qphy_failed(ret);
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_rx_diagnostics);

int q1000k_phy_receiver_startup(void)
{
	int ret = qphy_context();

	if (ret) return ret;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret) goto out;
	if (!ret && (!qphy_active || !qphy_controller)) ret = -EAGAIN;
	if (!ret) ret = q1000k_pon_receiver_startup(qphy_controller);
	if (ret && ret != -EAGAIN) qphy_failed(ret);
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_receiver_startup);

int q1000k_phy_snapshot(struct q1000k_rx_sample *sample,
			 struct q1000k_rx_diagnostics *diagnostics)
{
	struct q1000k_rx_sample rx;
	struct q1000k_rx_diagnostics diag;
	int ret = qphy_context();

	if (ret) return ret;
	if (!sample || !diagnostics) return -EINVAL;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret) goto out;
	if (!ret) ret = qphy_rx_sample(&rx);
	if (!ret) ret = qphy_diagnostics(&diag);
	if (!ret) {
		*sample = rx;
		*diagnostics = diag;
		qphy_last_rx = rx; qphy_last_diag = diag; qphy_last_valid = true;
		{
			u32 edges = rx.controller_los | rx.phy_los << 1 | rx.synced << 2;
			if (edges != qphy_edges) {
				q1000k_trace(QT_PHY_EDGE, 0, 0, qphy_edges, edges, rx.frames, rx.sync_status);
				qphy_edges = edges;
			}
		}
	} else if (ret != -EAGAIN) {
		qphy_failed(ret);
	}
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_snapshot);


/* Callback mutex held. Registers below are ordinary read-only snapshots;
 * do not latch/clear counters or select a PHY debug probe.
 */
static int qphy_rx_sample(struct q1000k_rx_sample *sample)
{
	struct q1000k_rx_sample result = {};
	const struct {
		u32 reg;
		u32 *value;
	} receiver[] = {
		{ EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL, &result.receiver.rx_control },
		{ EN7581_XGPON_PHY_XG_PHY_RST_N, &result.receiver.pcs_reset },
		{ EN7581_XPON_PMA_SW_RST_SET, &result.receiver.pma_reset },
		{ EN7581_XPON_PMA_PON_CK_SET, &result.receiver.clock_control },
		{ EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data, &result.receiver.cdr_control },
		{ EN7581_XPON_PMA_RO_RX_FREQDET, &result.receiver.rx_frequency },
		{ EN7581_XPON_PMA_ADD_LCPLL_RO_1, &result.receiver.pll_status },
		{ EN7581_XPON_PMA_SS_LCPLL_TDC_PW_0, &result.receiver.tdc_control },
		{ EN7581_XPON_PMA_ADD_RO_RX2ANA_1, &result.receiver.rx_analog0 },
		{ EN7581_XPON_PMA_ADD_RO_RX2ANA_2, &result.receiver.rx_analog1 },
		{ EN7581_XPON_PMA_ADD_RO_RX2ANA_3, &result.receiver.rx_analog2 },
		{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_1, &result.receiver.rx_sequence_force },
		{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1, &result.receiver.rx_sequence_disable },
		{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_FORCE_CTRL_0, &result.receiver.rx_sequence_force0 },
		{ EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_0, &result.receiver.rx_sequence_disable0 },
		{ EN7581_XPON_PMA_RX_FORCE_MODE_9, &result.receiver.rx_lock_force },
		{ EN7581_XPON_PMA_RX_DISB_MODE_8, &result.receiver.rx_lock_disable },
		{ EN7581_XPON_PMA_rg_force_da_pxp_rx_oscal_en, &result.receiver.rx_oscal_control },
		{ EN7581_XPON_PMA_RX_RESET_0, &result.receiver.rx_reset0 },
		{ EN7581_XPON_PMA_RX_RESET_1, &result.receiver.rx_reset1 },
		{ EN7581_XPON_PMA_SS_LCPLL_PWCTL_SETTING_0, &result.receiver.pll_power },
		{ EN7581_XPON_PMA_SS_LCPLL_TDC_FLT_3, &result.receiver.pll_filter },
		{ EN7581_XPON_PMA_SS_LCPLL_TDC_PCW_1, &result.receiver.pll_pcw1 },
		{ EN7581_XPON_PMA_SS_LCPLL_TDC_PCW_2, &result.receiver.pll_pcw2 },
		{ EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en, &result.receiver.pll_force },
		{ EN7581_XPON_ANA_RG_PXP_JCPLL_FREQ_MEAS_EN, &result.receiver.pll_measure },
		{ EN7581_XPON_ANA_RG_PXP_TXPLL_TCL_KBAND_VREF, &result.receiver.pll_kband },
		{ EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN, &result.receiver.pll_outputs },
		{ EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl, &result.receiver.rx_frontend_gain },
		{ EN7581_XGPON_PHY_SFP_STA, &result.receiver.sfp_status },
		{ EN7581_XGPON_PHY_SFP_VLD_LEVEL, &result.receiver.sfp_polarity },
		{ EN7581_XGPON_PHY_XG_PHY_STA, &result.receiver.digital_status },
		{ EN7581_XGPON_PHY_DBG_CTRL, &result.receiver.pcs_debug_control },
		{ EN7581_XGPON_PHY_XG_PON_SERDES_CTR, &result.receiver.serdes_control },
		{ EN7581_XPON_ANA_RG_PXP_RX_PHYCK_DIV, &result.receiver.rx_clock_divider },
		{ EN7581_XPON_ANA_RG_PXP_RX_BUSBIT_SEL, &result.receiver.rx_bus_width },
		{ EN7581_XPON_ANA_RG_PXP_RX_MPXSEL, &result.receiver.rx_input_control },
		{ EN7581_XPON_ANA_RG_PXP_CDR_LPF_RATIO, &result.receiver.rx_cdr_ratio },
		{ EN7581_XPON_PMA_RG_XPON_RX_RESERVED_1, &result.receiver.rx_rate_control },
		{ EN7581_XPON_PMA_rg_force_da_pxp_aeq_speed, &result.receiver.rx_osr_control },
		{ EN7581_XPON_PMA_XPON_SETTING_0, &result.receiver.signal_control },
		{ EN7581_XPON_ANA_RG_PXP_RX_FE_EQ_HZEN, &result.receiver.rx_equalizer },
		{ EN7581_XPON_ANA_RG_PXP_RX_FE_VCM_GEN_PWDB, &result.receiver.rx_frontend_power },
	};
	struct {
		u32 reg;
		u32 *value;
	} counters[] = {
		{ EN7581_XGPON_PHY_DBG_RX_CW_START_CNT, &result.pcs_counters.cw_start },
		{ EN7581_XGPON_PHY_DBG_RX_CW_END_CNT, &result.pcs_counters.cw_end },
		{ EN7581_XGPON_PHY_DBG_RX_SOF2MAC_CNT, &result.pcs_counters.sof_to_mac },
		{ EN7581_XGPON_PHY_DBG_RX_EOF2MAC_CNT, &result.pcs_counters.eof_to_mac },
		{ EN7581_XGPON_PHY_DBG_PSYNC_MISMATCH_CNT, &result.pcs_counters.psync_mismatch },
		{ EN7581_XGPON_PHY_DBG_SFC_HEC_ERR_CNT, &result.pcs_counters.sfc_hec_error },
		{ EN7581_XGPON_PHY_DBG_PON_ID_HEC_ERR_CNT, &result.pcs_counters.pon_id_hec_error },
	};
	u32 sfp, irq_mask;
	bool inhibited, tx;
	int i, ret;

	if (!qphy_controller || !qphy_active)
		return -EAGAIN;
	ret = q1000k_pon_get_tx_inhibit(qphy_controller, &inhibited);
	if (!ret)
		ret = q1000k_pon_get_tx(qphy_controller, &tx);
	if (ret)
		return ret;
	if (qphy_rx_bench && (!inhibited || tx))
		return -EACCES;
	result.rx_bench = qphy_rx_bench;
	result.tx_inhibited = inhibited;
	result.tx_enabled = tx;
	ret = q1000k_pon_get_los(qphy_controller);
	if (ret < 0)
		return ret;
	result.controller_los = !!ret;
	ret = q1000k_pon_get_rx_power(qphy_controller, &result.rx_power_nw);
	if (ret && ret != -ENODATA)
		return ret;
	result.rx_power_valid = !ret;
	ret = an7581_pon_phy_read(EN7581_XGPON_PHY_SFP_STA, &sfp);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_RX_SYNC_ST, &result.sync_status);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_XG_PON_INT_EN, &irq_mask);
	if (ret)
		return ret;
	if (sfp == ~0U || result.sync_status == ~0U || irq_mask == ~0U ||
	    (qphy_rx_bench && irq_mask != QPHY_RX_BENCH_IRQS))
		return -EIO;
	result.phy_los = !!(sfp & EN7581_XGPON_PHY_SFP_RX_LOS_ST);
	result.synced = !result.controller_los && !result.phy_los &&
		(result.sync_status & EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC) ==
		EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC;
	/* The reference freq_check() and PMA initialization use these ordinary
	 * control/status words. Snapshot them without running recovery, touching
	 * the debug mux, or replaying reset strobes. All-ones is a failed control
	 * read; the full-width traffic counters below may legitimately wrap.
	 */
	for (i = 0; i < ARRAY_SIZE(receiver); i++) {
		ret = an7581_pon_phy_read(receiver[i].reg, receiver[i].value);
		if (ret || *receiver[i].value == ~0U)
			return ret ?: -EIO;
	}
	ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_RX_FRAME2PHYD_CNT, &result.frames);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_LOF_CNT, &result.lof);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_TOTAL_CW_CNT, &result.fec_total);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_CORRECTED_CW_CNT, &result.fec_corrected);
	if (!ret)
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_UNCORRECTED_CW_CNT, &result.fec_uncorrected);
	if (ret)
		return ret;
	/* Ordinary PCS counters, also listed by the reference PHY register dump.
	 * Never read FIFO/debug-probe ports or write the counter-clear controls.
	 * Unlike configuration words, all-ones is a legitimate counter value.
	 */
	for (i = 0; i < ARRAY_SIZE(counters); i++) {
		ret = an7581_pon_phy_read(counters[i].reg, counters[i].value);
		if (ret)
			return ret;
	}
	result.irq_calls = qphy_rx_irqs;
	result.poll_calls = qphy_rx_polls;
	result.reacquire_enabled = qphy_rx_reacquire;
	result.pll_restore_enabled = qphy_rx_restore_pll;
	result.gain_restore_enabled = qphy_rx_restore_gain;
	result.reacquire_attempts = qphy_rx_attempts;
	result.sampled_ms = ktime_to_ms(ktime_get_boottime());
	if (result.synced)
		qphy_repeat_stop("sync");
	else if (qphy_rx_attempts && (result.controller_los || result.phy_los))
		qphy_repeat_stop("los");
	*sample = result;
	return 0;
}

int q1000k_phy_rx_sample(struct q1000k_rx_sample *sample)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (!sample)
		return -EINVAL;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret) {
		ret = qphy_rx_bench ? qphy_rx_sample(sample) : -EAGAIN;
		if (ret && ret != -EAGAIN)
			qphy_failed(ret);
	}
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_rx_sample);

static int qphy_reg_update(u32 reg, u32 mask, u32 value, u32 omit)
{
	u32 old;
	int ret = an7581_pon_phy_read(reg, &old);

	if (ret || old == ~0U)
		return ret ?: -EIO;
	return qphy_reg_write(reg, (old & ~(mask | omit)) | value);
}

/* Read only the ordinary profile words, under the callback lock. Return
 * one for a complete match, zero for a valid mismatch, negative on failure.
 * This checks current hardware, including after resets/reconfiguration. */
int q1000k_phy_profile_matches(const struct q1000k_pon_profile *p)
{
 u32 offset, info, value;
 unsigned int i;
 int ret = qphy_context();
 if (ret) return ret;
 if (!q1000k_pon_profile_valid(p)) return -EINVAL;
 if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
 offset = 8U * p->index;
 info = (u32)p->repeat << 16 | (u32)p->preamble_len << 8 | p->delimiter_len;
 {
  const u32 regs[] = { EN7581_XGPON_PHY_PREAMBLE1_UPPER + offset,
   EN7581_XGPON_PHY_PREAMBLE1_LOWER + offset, EN7581_XGPON_PHY_DELIMITER1_UPPER + offset,
   EN7581_XGPON_PHY_DELIMITER1_LOWER + offset, EN7581_XGPON_PHY_PSBU_INFO1 + 4U * p->index,
   EN7581_XGPON_PHY_XG_TX_FEC_EN_CTRL };
  const u32 expected[] = { get_unaligned_be32(p->preamble), get_unaligned_be32(p->preamble + 4),
   get_unaligned_be32(p->delimiter), get_unaligned_be32(p->delimiter + 4), info, (u32)p->fec << offset };
  mutex_lock(&qphy_control);
  qphy_callback_lock();
  ret = qphy_ready();
  if (!ret && !gpPhyPriv->phy_init_done) ret = -EAGAIN;
  for (i = 0; !ret && i < ARRAY_SIZE(regs); i++) {
   u32 mask = i == 5 ? 1U << offset : ~0U;
   ret = an7581_pon_phy_read(regs[i], &value);
   if (!ret && ((value ^ expected[i]) & mask)) break;
  }
  if (!ret) ret = i == ARRAY_SIZE(regs);
  qphy_callback_unlock();
  mutex_unlock(&qphy_control);
 }
 return ret;
}
EXPORT_SYMBOL(q1000k_phy_profile_matches);

int q1000k_phy_profile_set(const struct q1000k_pon_profile *p)
{
	u32 offset, info;
	int ret = qphy_context();

	if (ret)
		return ret;
	if (!q1000k_pon_profile_valid(p))
		return -EINVAL;
	if (READ_ONCE(qphy_owner) == current)
		return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret && !gpPhyPriv->phy_init_done)
		ret = -EAGAIN;
	if (!ret && (READ_ONCE(qphy_active) || qphy_irq_dev))
		ret = -EBUSY;
	if (ret)
		goto out;
	ret = q1000k_phy_controller_check();
	if (ret)
		goto fail;
	offset = 8U * p->index;
	ret = qphy_reg_write(EN7581_XGPON_PHY_PREAMBLE1_UPPER + offset, get_unaligned_be32(p->preamble));
	if (!ret)
		ret = qphy_reg_write(EN7581_XGPON_PHY_PREAMBLE1_LOWER + offset, get_unaligned_be32(p->preamble + 4));
	if (!ret)
		ret = qphy_reg_write(EN7581_XGPON_PHY_DELIMITER1_UPPER + offset, get_unaligned_be32(p->delimiter));
	if (!ret)
		ret = qphy_reg_write(EN7581_XGPON_PHY_DELIMITER1_LOWER + offset, get_unaligned_be32(p->delimiter + 4));
	info = (u32)p->repeat << 16 | (u32)p->preamble_len << 8 | p->delimiter_len;
	if (!ret)
		ret = qphy_reg_write(EN7581_XGPON_PHY_PSBU_INFO1 + 4U * p->index, info);
	if (!ret)
		ret = qphy_reg_update(EN7581_XGPON_PHY_XG_TX_FEC_EN_CTRL,
			1U << offset, (u32)p->fec << offset, 0);
fail:
	if (ret)
		qphy_failed(ret);
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_profile_set);

static int qphy_receive_set(struct xpon_phy_api_data_s *data)
{
	u32 value, mask;
	int ret;

	if (READ_ONCE(qphy_active) || qphy_irq_dev)
		return -EBUSY;
	if (data->cmd_id != PON_SET_PHY_RX_FEC_SETTING)
		return qphy_reg_update(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL,
			EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL_RX_ENABLE,
			data->cmd_id == PON_SET_PHY_XGPON_RX_ENABLE ?
			EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL_RX_ENABLE : 0, 0);
	if (!data->data || *data->data < DS_FEC_SETTING_FORCE_OFF ||
	    *data->data > DS_FEC_SETTING_FORCE_OC)
		return -EINVAL;
	ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_CTRL, &value);
	if (ret || value == ~0U)
		return ret ?: -EIO;
	/* Preserve the AN7581 vendor mode encoding, including the distinct
	 * OC-reference controls. Never replay the counter-clear strobe.
	 */
	mask = EN7581_XGPON_PHY_DBG_RX_FEC_OC_REF_EN |
		EN7581_XGPON_PHY_DBG_XG_OC_EN;
	value &= ~(mask | EN7581_XGPON_PHY_DBG_CTRL_DBG_CNT_CLEAR);
	if (*data->data == DS_FEC_SETTING_FORCE_ON)
		value |= EN7581_XGPON_PHY_DBG_RX_FEC_FORCE_OFF;
	else if (*data->data == DS_FEC_SETTING_FORCE_OFF)
		value &= ~(EN7581_XGPON_PHY_DBG_RX_FEC_FORCE_OFF | EN7581_XGPON_PHY_DBG_RX_FEC_FORCE_ON);
	else
		value |= mask | EN7581_XGPON_PHY_DBG_RX_FEC_FORCE_OFF;
	ret = qphy_reg_write(EN7581_XGPON_PHY_DBG_CTRL, value);
	if (!ret)
		gpPhyPriv->rx_fec_setting = *data->data;
	return ret;
}

static int qphy_status_read(u32 reg, u32 *value)
{
	int ret = an7581_pon_phy_read(reg, value);

	return ret ?: *value == ~0U ? -EIO : 0;
}

/* External queries never select probes, clear counters, read an unowned
 * transceiver address, or report a vendor stub's initial zero as success.
 * Counter outputs are snapshots: publish only after every read succeeds.
 */
static int qphy_get(struct xpon_phy_api_data_s *data)
{
	u32 value, other;
	bool enabled;
	int ret;

	switch (data->cmd_id) {
	case PON_GET_PHY_MODE:
		return gpPhyPriv->phyCfg.flags.mode;
	case PON_GET_PHY_GET_TX_POWER_EN_FLAG:
		ret = q1000k_pon_get_tx(qphy_controller, &enabled);
		return ret ?: enabled;
	case PON_GET_PHY_LOS_STATUS:
		ret = qphy_status_read(EN7581_XGPON_PHY_SFP_STA, &value);
		return ret ?: !!(value & EN7581_XGPON_PHY_SFP_RX_LOS_ST);
	case PON_GET_PHY_READY_STATUS:
		ret = qphy_status_read(EN7581_XGPON_PHY_DBG_RX_SYNC_ST, &value);
		return ret ?: (value & EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC) ==
			EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC;
	case PON_GET_PHY_IS_SYNC:
		ret = qphy_status_read(EN7581_XGPON_PHY_SFP_STA, &value);
		if (!ret)
			ret = qphy_status_read(EN7581_XGPON_PHY_DBG_RX_SYNC_ST, &other);
		return ret ?: !(value & EN7581_XGPON_PHY_SFP_RX_LOS_ST) &&
			(other & EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC) ==
			EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC;
	case PON_GET_PHY_RX_FEC_GETTING:
		ret = qphy_status_read(EN7581_XGPON_PHY_DBG_CTRL, &value);
		return ret ?: !!(value & EN7581_XGPON_PHY_DBG_RX_FEC_FORCE_OFF);
	case PON_GET_PHY_TX_FEC_STATUS:
		ret = qphy_status_read(EN7581_XGPON_PHY_DBG_TX_FEC_STA, &value);
		return ret ?: !!(value & EN7581_XGPON_PHY_TX_FEC);
	case PON_GET_PHY_RX_FEC_COUNTER: {
		PHY_FecCount_T result = {};

		if (!data->rx_fec_cnt)
			return -EINVAL;
		/* All-ones is a valid full-width counter value, not an error. */
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_CORRECTED_BYTE_CNT, &result.correct_bytes);
		if (!ret)
			ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_CORRECTED_CW_CNT, &result.correct_codewords);
		if (!ret)
			ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_UNCORRECTED_CW_CNT, &result.uncorrect_codewords);
		if (!ret)
			ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_TOTAL_CW_CNT, &result.total_rx_codewords);
		if (!ret)
			ret = an7581_pon_phy_read(EN7581_XGPON_PHY_FEC_ERR_SECONDS, &result.fec_seconds);
		if (!ret)
			*data->rx_fec_cnt = result;
		return ret;
	}
	case PON_GET_PHY_RX_FRAME_COUNTER: {
		PHY_FrameCount_T result = {};

		if (!data->rx_frame_cnt)
			return -EINVAL;
		ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_RX_FRAME2PHYD_CNT, &result.frame_count_low);
		if (!ret)
			ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_LOF_CNT, &result.lof_counter);
		if (!ret)
			*data->rx_frame_cnt = result;
		return ret;
	}
	default:
		return -EOPNOTSUPP;
	}
}

int q1000k_phy_call(struct xpon_phy_api_data_s *data)
{
	int ret = qphy_context();

	if (!data)
		return -EINVAL;
	if (ret)
		return data->ret = ret;
	if (data->api_type != XPON_PHY_API_TYPE_GET &&
	    data->api_type != XPON_PHY_API_TYPE_SET)
		return data->ret = -EINVAL;
	if (data->api_type == XPON_PHY_API_TYPE_SET) {
		/* Reset, TX, mode and profile writes require typed lifecycle APIs. */
		if (data->cmd_id != PON_SET_PHY_RX_FEC_SETTING &&
		    data->cmd_id != PON_SET_PHY_XGPON_RX_ENABLE &&
		    data->cmd_id != PON_SET_PHY_XGPON_RX_DISABLE)
			return data->ret = -EOPNOTSUPP;
	}
	if (READ_ONCE(qphy_owner) == current)
		return data->ret = -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret && data->api_type == XPON_PHY_API_TYPE_GET &&
	    data->cmd_id == PON_GET_PHY_INIT_STATUS) {
		ret = !!gpPhyPriv->phy_init_done;
		goto out;
	}
	if (!ret && !gpPhyPriv->phy_init_done)
		ret = -EAGAIN;
	if (!ret && data->api_type == XPON_PHY_API_TYPE_SET) {
		ret = qphy_receive_set(data);
		if (ret && ret != -EINVAL && ret != -EBUSY)
			qphy_failed(ret);
		goto out;
	}
	if (!ret) {
		ret = q1000k_phy_controller_check();
		if (!ret)
			ret = qphy_get(data);
		if (ret < 0 && ret != -EINVAL && ret != -EOPNOTSUPP)
			qphy_failed(ret);
	}
out:
	data->ret = ret;
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_call);

int q1000k_phy_init(void)
{
	int ret;

	if (!get_pon_phy_dev() || get_pon_phy_irq() < 0)
		return -ENODEV;
	ret = an7581_pon_phy_status();
	if (ret)
		return ret;
	if ((GET_HIR() & 0xffff) != 0xe)
		return -ENODEV;
	gpPhyPriv = kzalloc(sizeof(*gpPhyPriv), GFP_KERNEL);
	if (!gpPhyPriv)
		return -ENOMEM;
	qphy_last_valid = false; qphy_manual_used = 0; qphy_edges = ~0U;
	qphy_rx_bench = false;
	qphy_rx_reacquire = false;
	qphy_rx_restore_pll = false;
	qphy_rx_restore_gain = false;
	qphy_rx_probe_mode = 0;
	qphy_rx_attempts = qphy_rx_no_sync = 0;
	qphy_rx_seen_light = false;
	qphy_repeat_reason = NULL;
	qphy_repeat_next_ms = 0;
	qphy_rx_irqs = qphy_rx_polls = 0;
	gpPhyPriv->scu_hir_np_sys_hw_id = 0xe;
	gpPhyPriv->wan_sel = SCU_WAN_CONF_REG_WAN_SEL_XGSPON;
	gpPhyPriv->rx_fec_setting = PHY_DEFAULT;
	gpPhyPriv->phyCfg.flags.mode = PHY_UNKNOWN_CONFIG;
	gpPhyPriv->trans_index = PHY_TRANS_NOT_FOUND_IN_IOT_LIST;
	gpPhyPriv->i2c_u2_clk_div = I2C_U2_CLK_DIV;
	gpPhyPriv->i2c_addr_num = 1;
	gpPhyPriv->phy_status = PHY_LINK_STATUS_UNKNOWN;
	gpPhyPriv->trans_tx_enable = PHY_DISABLE;
	gpPhyPriv->trans_tx_status = PHY_DISABLE;
	gpPhyPriv->first_plugin_flag = TRUE;
	gpPhyPriv->trans_msg_print_cnt = 95;
	gpPhyPriv->debugLevel = PHY_MSG_ERR;
	gpPhyPriv->pon_stop_flag = TRUE;
	gpPhyPriv->event_poll_timer_value = 1500;
	spin_lock_init(&gpPhyPriv->event_handle_lock);
	spin_lock_init(&gpPhyPriv->pma_reset_lock);
	timer_setup(&gpPhyPriv->event_poll_timer, phy_event_poll, 0);
	ponPhyFunc = en7581_xgpon_func;
	/* No debug proc writers, raw callback pointers, hook publication or
	 * hardware initialization occurs during module load on Q1000K.
	 */
	return 0;
}

void q1000k_phy_exit(void)
{
	mutex_lock(&qphy_control);
	if (!gpPhyPriv) {
		mutex_unlock(&qphy_control);
		return;
	}
	qphy_dead = true;
	qphy_stop();
	qphy_callback_lock();
	timer_shutdown_sync(&gpPhyPriv->event_poll_timer);
	if (qphy_controller) {
		q1000k_pon_put(qphy_controller);
		qphy_controller = NULL;
	}
	kfree(gpPhyPriv);
	gpPhyPriv = NULL;
	ponPhyFunc = NULL;
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
}

/* Cached capture survives a contained hardware fault. Its original timestamp
 * is retained; consumers must never treat this as a new healthy observation. */
int q1000k_phy_last_snapshot(struct q1000k_rx_sample *rx,
			     struct q1000k_rx_diagnostics *diag, int *fault)
{
	int ret = qphy_context();
	if (ret) return ret;
	if (!rx || !diag || !fault) return -EINVAL;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	qphy_callback_lock();
	ret = qphy_last_valid ? 0 : -ENODATA;
	if (!ret) { *rx = qphy_last_rx; *diag = qphy_last_diag; *fault = qphy_fault; }
	qphy_callback_unlock();
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_last_snapshot);

/* Fixed recipes only. Caller serializes the protocol. Actions 5/6 additionally
 * require the MAC/DMA transaction to have stopped and drained PHY callbacks.
 * Each action consumes one bit before any write; no retries after a fault. */
int q1000k_phy_bench_recover(unsigned int action)
{
	struct q1000k_rx_sample before;
	bool inhibited = false, enabled = true;
	int ret = qphy_context();
	if (ret) return ret;
	if (action < 1 || action > 6) return -EINVAL;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	mutex_lock(&qphy_control);
	qphy_callback_lock();
	ret = qphy_ready();
	if (ret) goto out;
	if (!qphy_rx_bench || qphy_rx_reacquire || !qphy_controller) { ret = -EPERM; goto out; }
	if (qphy_manual_used & BIT(action)) { ret = -EALREADY; goto out; }
	ret = q1000k_pon_get_tx_inhibit(qphy_controller, &inhibited);
	if (!ret) ret = q1000k_pon_get_tx(qphy_controller, &enabled);
	if (!ret && (!inhibited || enabled)) ret = -EACCES;
	if (ret) goto out;
	if (action <= 4) {
		ret = qphy_rx_sample(&before);
		if (ret) goto out;
		if (before.controller_los || before.phy_los || before.synced) { ret = -EAGAIN; goto out; }
	} else if (qphy_active || qphy_irq_dev) { ret = -EBUSY; goto out; }
	qphy_manual_used |= BIT(action);
	q1000k_trace(QT_RECOVERY, action, 0, 0, qphy_manual_used, 0, 0);
	q1000k_trace_generation(action + 1);
	if (action <= 2) ret = q1000k_phy_rx_reacquire(action == 2, false);
	else if (action <= 4) ret = q1000k_phy_rx_bench_recipe(action);
	else if (action == 5) {
		/* Force the same complete PHY initialization as a fresh module load;
		 * keep the controller lease and its calibrated MCU image intact. */
		gpPhyPriv->phy_init_done = FALSE;
		gpPhyPriv->pma_init_done = FALSE;
		gpPhyPriv->first_plugin_flag = TRUE;
		ret = phy_mode_config(PHY_XGSPON_CONFIG, PHY_DISABLE);
		if (!ret) ret = qphy_mask();
	} else ret = q1000k_pon_bench_reinitialize(qphy_controller);
	if (!ret) ret = q1000k_phy_controller_check();
	if (!ret) ret = an7581_pon_phy_status();
	q1000k_trace(QT_RECOVERY, action, ret, 1, qphy_manual_used, 0, 0);
	if (ret) qphy_failed(ret);
out:
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_bench_recover);

/* Three ordinary MMIO reads; no controller I2C, FIFO or counter clear. */
int q1000k_phy_fast_sample(u32 *sfp, u32 *sync, u32 *frames)
{
	int ret = qphy_context();
	if (ret) return ret;
	if (!sfp || !sync || !frames) return -EINVAL;
	if (READ_ONCE(qphy_owner) == current) return -EDEADLK;
	qphy_callback_lock();
	ret = qphy_ready();
	if (!ret && !qphy_active) ret = -EAGAIN;
	if (!ret) ret = an7581_pon_phy_read(EN7581_XGPON_PHY_SFP_STA, sfp);
	if (!ret) ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_RX_SYNC_ST, sync);
	if (!ret) ret = an7581_pon_phy_read(EN7581_XGPON_PHY_DBG_RX_FRAME2PHYD_CNT, frames);
	if (!ret && (*sfp == ~0U || *sync == ~0U)) ret = -EIO;
	if (ret && ret != -EAGAIN) qphy_failed(ret);
	qphy_callback_unlock();
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_fast_sample);
