// SPDX-License-Identifier: GPL-2.0-only
/* Sleepable Q1000K PHY control and callback lifetime. */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <an7581_pon_phy.h>
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
static bool qphy_active, qphy_dead;
static int qphy_fault;
static struct device *qphy_irq_dev;
static int qphy_irq = -1;

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

static bool qphy_callback_trylock(void)
{
	if (!mutex_trylock(&qphy_callback))
		return false;
	WRITE_ONCE(qphy_owner, current);
	return true;
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
	if (ret)
		return ret;
	ret = an7581_pon_phy_read(reg, &actual);
	return ret ? ret : actual != value ? -EIO : 0;
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

static void qphy_failed(int error)
{
	if (!error)
		return;
	WRITE_ONCE(qphy_active, false);
	WRITE_ONCE(gpPhyPriv->pon_stop_flag, TRUE);
	WRITE_ONCE(gpPhyPriv->is_phy_start, FALSE);
	if (!qphy_fault)
		qphy_fault = error;
	qphy_mask();
}

static void qphy_poll_work(struct work_struct *work)
{
	int ret;

	qphy_callback_lock();
	if (!READ_ONCE(qphy_active))
		goto out;
	ret = ponPhyFunc[PHY_EVENT_POLL_FUNC]((char *)gpPhyPriv);
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
	handled = IRQ_HANDLED;
	/* Skip the legacy interrupt-count heuristic and its 50 ms busy wait.
	 * Dispatch only after checking this PHY's enabled pending sources.
	 */
	ret = ponPhyFunc[PHY_ISR_FUNC]((char *)gpPhyPriv);
	if (!ret)
		ret = an7581_pon_phy_status();
fail:
	if (ret)
		qphy_failed(ret);
out:
	qphy_callback_unlock();
	return handled;
}

int q1000k_phy_configure(u32 mode)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (mode != PHY_XGSPON_CONFIG)
		return -EOPNOTSUPP;
	if (!mutex_trylock(&qphy_control))
		return -EBUSY;
	if (!qphy_callback_trylock()) {
		ret = -EBUSY;
		goto control_out;
	}
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
	/* Optical TX stays disabled; its enable belongs to controller/MAC startup. */
	ret = phy_mode_config(PHY_XGSPON_CONFIG, PHY_DISABLE);
	if (!ret)
		ret = qphy_mask();
	if (!ret)
		ret = an7581_pon_phy_status();
	if (ret) {
		gpPhyPriv->phy_init_done = FALSE;
		qphy_failed(ret);
	}
out:
	qphy_callback_unlock();
control_out:
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
	if (!mutex_trylock(&qphy_control))
		return -EBUSY;
	if (!qphy_callback_trylock()) {
		ret = -EBUSY;
		goto control_out;
	}
	ret = qphy_ready();
	if (ret)
		goto out;
	if (READ_ONCE(qphy_active))
		goto out;
	if (!gpPhyPriv->phy_init_done) {
		ret = -EAGAIN;
		goto out;
	}
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
	WRITE_ONCE(qphy_active, true);
	ret = qphy_reg_write(EN7581_XGPON_PHY_XG_PON_INT_EN,
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
		EN7581_XGPON_PHY_RX_LOS_INT_EN);
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
control_out:
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_start);

/* Called with control held. Callback code never waits for control: callbacks
 * trying to reenter a lifecycle API receive -EBUSY. Do not hold callback over
 * free_irq/cancel_work_sync, which wait for that same code to finish.
 */
static int qphy_stop(void)
{
	int ret = 0, err;

	if (!gpPhyPriv)
		return -ENODEV;
	WRITE_ONCE(qphy_active, false);
	WRITE_ONCE(gpPhyPriv->pon_stop_flag, TRUE);
	WRITE_ONCE(gpPhyPriv->is_phy_start, FALSE);
	if (gpPhyPriv->phy_init_done || qphy_irq_dev)
		ret = qphy_mask();
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
	if (gpPhyPriv->phy_init_done) {
		err = qphy_mask();
		if (!ret)
			ret = err;
		err = phy_fw_ready(PHY_DISABLE);
		if (!ret)
			ret = err;
	}
	if (!ret)
		ret = an7581_pon_phy_status();
	if (ret)
		qphy_failed(ret);
	if (!ret)
		ret = qphy_fault;
	qphy_callback_unlock();
	return ret;
}

int q1000k_phy_stop(void)
{
	int ret = qphy_context();

	if (ret)
		return ret;
	if (!mutex_trylock(&qphy_control))
		return -EBUSY;
	/* A nested stop from this PHY's own callback must not wait on itself. */
	if (!qphy_callback_trylock()) {
		mutex_unlock(&qphy_control);
		return -EBUSY;
	}
	qphy_callback_unlock();
	ret = qphy_stop();
	mutex_unlock(&qphy_control);
	return ret;
}
EXPORT_SYMBOL(q1000k_phy_stop);

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
		if (data->cmd_id == PON_SET_PHY_START)
			return data->ret = q1000k_phy_start();
		if (data->cmd_id == PON_SET_PHY_STOP)
			return data->ret = q1000k_phy_stop();
		/* Lifecycle and optical TX cannot bypass coordinated startup. */
		if (data->cmd_id == PON_SET_PHY_MODE_CONFIG ||
		    data->cmd_id == PON_SET_PHY_TRANS_POWER_SWITCH ||
		    data->cmd_id == PON_SET_PHY_TX_POWER_CONFIG)
			return data->ret = -EOPNOTSUPP;
	}
	if (!mutex_trylock(&qphy_control))
		return data->ret = -EBUSY;
	if (!qphy_callback_trylock()) {
		mutex_unlock(&qphy_control);
		return data->ret = -EBUSY;
	}
	ret = qphy_ready();
	if (!ret && data->api_type == XPON_PHY_API_TYPE_GET &&
	    data->cmd_id == PON_GET_PHY_INIT_STATUS) {
		ret = !!gpPhyPriv->phy_init_done;
		goto out;
	}
	if (!ret && !gpPhyPriv->phy_init_done)
		ret = -EAGAIN;
	if (!ret) {
		data->ret = -EOPNOTSUPP;
		pon_phy_api_dispatch((struct ecnt_data *)data);
		ret = an7581_pon_phy_status();
		if (!ret)
			ret = data->ret;
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
	kfree(gpPhyPriv);
	gpPhyPriv = NULL;
	ponPhyFunc = NULL;
	qphy_callback_unlock();
	mutex_unlock(&qphy_control);
}
