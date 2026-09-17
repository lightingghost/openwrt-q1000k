// SPDX-License-Identifier: GPL-2.0-only
/* Complete physical port retirement; no service record is released here. */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <q1000k_trace.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <an7581_xpon.h>
#include <q1000k_phy_api.h>
#include <ecnt_hook/ecnt_hook_pon_phy.h>
#include "common/q1000k_transport.h"
#include "common/q1000k_pipeline.h"

#define Q1000K_MAC_ALL_STOPS (AN7581_XPON_MBI_RX_STOP | \
	AN7581_XPON_MBI_TX_STOP | AN7581_XPON_MPI_RX_STOP | \
	AN7581_XPON_MPI_TX_STOP)

static DEFINE_MUTEX(q1000k_pipeline_lock);
static struct q1000k_pipeline_status q1000k_pipeline;
static unsigned int bench_omci_min_len = 60;
module_param(bench_omci_min_len, uint, 0400);
MODULE_PARM_DESC(bench_omci_min_len, "Bench PON minimum: 60 byte Ethernet default or 48 byte baseline OMCI");

static void q1000k_pipeline_contain(void)
{
	unsigned int channel;
	int ret;

	/* Admission has already closed, even on pause timeout. Attempt every
	 * independent containment action; preserve the original failure and
	 * separately report the first failure to establish containment.
	 */
	for (channel = 0; channel < 32; channel++) {
		ret = q1000k_transport_set_tx_channel(channel, false);
		if (ret && !q1000k_pipeline.containment_error)
			q1000k_pipeline.containment_error = ret;
	}
	ret = an7581_xpon_mac_stop(Q1000K_MAC_ALL_STOPS, true);
	if (ret && !q1000k_pipeline.containment_error)
		q1000k_pipeline.containment_error = ret;
	ret = q1000k_phy_quiesce();
	if (ret && !q1000k_pipeline.containment_error)
		q1000k_pipeline.containment_error = ret;
	/* No reload may interpret a partial drain as a fresh, usable MAC. */
	an7581_xpon_invalidate();
}

int q1000k_pipeline_shutdown(void)
{
	unsigned int channel;
	int ret;

	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_pipeline_lock);
	ret = q1000k_pipeline.error;
	if (ret || q1000k_pipeline.stage == Q1000K_PIPELINE_PHY_STOPPED)
		goto out;
	ret = q1000k_transport_pause(1000);
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_CPU_PAUSED;
	ret = q1000k_phy_prepare_wan();
	if (ret)
		goto fail;
	ret = q1000k_phy_needs_configure();
	if (ret < 0)
		goto fail;
	if (ret) {
		/* The reference cold startup configures PHY clocks before waiting
		 * for MPI acknowledgment. Close both RX boundaries first: MPI
		 * request readback plus acknowledged MBI RX stop. Native admission
		 * is already closed; configure verifies controller TX off and does
		 * not start IRQ/polling. Never reset MAC/FE or replace IDs here.
		 */
		ret = an7581_xpon_mac_request_rx_stop();
		if (!ret)
			ret = an7581_xpon_mac_stop(AN7581_XPON_MBI_RX_STOP, true);
		if (!ret)
			ret = q1000k_phy_configure(PHY_XGSPON_CONFIG);
		if (ret) {
			pr_err("q1000k: cold PHY preparation failed: %d\n", ret);
			goto fail;
		}
	}
	ret = an7581_xpon_mac_stop(AN7581_XPON_MPI_RX_STOP, true);
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_INGRESS_STOPPED;
	/* MBI TX and MPI TX must still be able to empty during FE release. */
	for (channel = 0; channel < 32; channel++) {
		ret = q1000k_transport_retire_fe(channel);
		if (ret)
			goto fail;
		q1000k_pipeline.retired |= BIT(channel);
	}
	q1000k_pipeline.stage = Q1000K_PIPELINE_FE_RETIRED;
	ret = an7581_xpon_mac_stop(AN7581_XPON_MBI_TX_STOP, true);
	if (!ret)
		ret = an7581_xpon_mac_wait_tx_empty();
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_FIFO_EMPTY;
	ret = an7581_xpon_mac_stop(AN7581_XPON_MPI_TX_STOP |
				  AN7581_XPON_MBI_RX_STOP, true);
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_MAC_STOPPED;
	ret = q1000k_transport_drain_rx();
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_RX_DRAINED;
	/* PHY polling/IRQ callbacks drain before real controller TX is disabled.
	 * No MAC/FE reset or ID reuse precedes this boundary. An unconfigured
	 * PHY's initial clock/reset preparation above is needed to reach it.
	 */
	ret = q1000k_phy_quiesce();
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_PHY_STOPPED;
	goto out;
fail:
	q1000k_pipeline.error = ret;
	q1000k_pipeline_contain();
	pr_err("q1000k: shutdown failed after stage %u: %d retired=%#x containment=%d\n",
	       q1000k_pipeline.stage, ret, q1000k_pipeline.retired,
	       q1000k_pipeline.containment_error);
out:
	mutex_unlock(&q1000k_pipeline_lock);
	return ret;
}

void q1000k_pipeline_status(struct q1000k_pipeline_status *status)
{
	mutex_lock(&q1000k_pipeline_lock);
	*status = q1000k_pipeline;
	mutex_unlock(&q1000k_pipeline_lock);
}

/* Service namespace transaction. The caller has drained protocol/control
 * producers; only this task may issue table-replacement commands in callbacks.
 */
static struct task_struct *q1000k_table_owner;
static enum q1000k_table_phase q1000k_table_phase;

int q1000k_pipeline_table_context(enum q1000k_table_phase phase)
{
	/* A hard IRQ may interrupt the owner with the same current pointer. */
	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	if (READ_ONCE(q1000k_table_owner) != current)
		return -EPERM;
	return q1000k_table_phase == phase ? 0 : -EPERM;
}

/* TX FCS table state must not survive channel/GEM reuse. All MAC transfer
 * paths and DMA are stopped and empty here. The XG register definition gives
 * START bit 0 and DONE bit 8; write a fresh command with reserved bits zero.
 * This clears packet assembly state, preserving ONU, OMCC and key registers.
 */
static int q1000k_pipeline_clear_fcs(void)
{
	unsigned int retry;
	u32 value;
	int ret = an7581_xpon_status();

	if (ret)
		return ret;
	value = get_xpon_data(0x527c);
	if (value == ~0U)
		return -EIO;
	if ((value & BIT(0)) && !(value & BIT(8)))
		return -EBUSY;
	set_xpon_data(0x527c, BIT(0));
	for (retry = 0; retry < 3000; retry++) {
		value = get_xpon_data(0x527c);
		if (value == ~0U)
			return -EIO;
		if (value & BIT(8))
			return an7581_xpon_status();
		udelay(1);
	}
	return -ETIMEDOUT;
}

int q1000k_pipeline_reconfigure(const struct q1000k_pipeline_ops *ops,
			       void *arg, u32 channels)
{
	struct airoha_pon_port_config old, config;
	unsigned int channel;
	int ret;

	if (!ops || !ops->clear || !ops->install || !(channels & BIT(0)))
		return -EINVAL;
	if (bench_omci_min_len != 60 && bench_omci_min_len != 48)
		return -EINVAL;
	ret = q1000k_pipeline_shutdown();
	if (ret)
		return ret;
	mutex_lock(&q1000k_pipeline_lock);
	ret = q1000k_pipeline.error;
	if (ret)
		goto out;
	if (q1000k_pipeline.stage != Q1000K_PIPELINE_PHY_STOPPED) {
		ret = -EBUSY;
		goto out;
	}
	ret = q1000k_transport_get_port_config(&old);
	if (ret)
		goto fail;
	if (ops->reset_mac) {
		ret = an7581_xpon_reset();
		if (!ret)
			ret = an7581_xpon_mac_stop(Q1000K_MAC_ALL_STOPS, true);
		if (ret)
			goto fail;
	}
	q1000k_pipeline.stage = Q1000K_PIPELINE_TABLES_CHANGING;
	q1000k_table_phase = Q1000K_TABLE_CLEAR;
	WRITE_ONCE(q1000k_table_owner, current);
	ret = ops->clear(arg);
	WRITE_ONCE(q1000k_table_owner, NULL);
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_TABLES_CLEARED;
	ret = q1000k_pipeline_clear_fcs();
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_FCS_CLEARED;
	ret = q1000k_transport_reset_epoch();
	if (ret)
		goto fail;
	q1000k_pipeline.retired = 0;
	q1000k_pipeline.stage = Q1000K_PIPELINE_EPOCH_READY;
	config = old;
	config.min_len = bench_omci_min_len;
	config.max_len = 2000;
	ret = q1000k_transport_configure_port(&old, &config);
	q1000k_trace(QT_CONTROL, 36, ret, old.min_len, config.min_len, old.max_len, config.max_len);
	if (ret)
		goto fail;
	q1000k_table_phase = Q1000K_TABLE_INSTALL;
	WRITE_ONCE(q1000k_table_owner, current);
	ret = ops->install(arg);
	WRITE_ONCE(q1000k_table_owner, NULL);
	if (ret)
		goto fail;
	for (channel = 0; channel < 32; channel++) {
		if (!(channels & BIT(channel)))
			continue;
		ret = q1000k_transport_set_tx_channel(channel, true);
		if (ret)
			goto fail;
	}
	q1000k_pipeline.channels = channels;
	q1000k_pipeline.stage = Q1000K_PIPELINE_PREPARED;
	goto out;
fail:
	q1000k_pipeline.error = ret;
	q1000k_pipeline_contain();
	pr_err("q1000k: reconfigure failed after stage %u: %d containment=%d\n",
	       q1000k_pipeline.stage, ret, q1000k_pipeline.containment_error);
out:
	mutex_unlock(&q1000k_pipeline_lock);
	return ret;
}

int q1000k_pipeline_activate_checked(bool transmit, int (*ready)(void *), void *arg)
{
	int ret;

	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_pipeline_lock);
	ret = q1000k_pipeline.error;
	if (ret)
		goto out;
	if (q1000k_pipeline.stage != Q1000K_PIPELINE_PREPARED) {
		ret = -EINVAL;
		goto out;
	}
	ret = q1000k_transport_activate_rx(q1000k_pipeline.channels);
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_RX_ACTIVE;
	ret = q1000k_phy_start();
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_PHY_ACTIVE;
	ret = an7581_xpon_mac_stop(Q1000K_MAC_ALL_STOPS, false);
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_MAC_ACTIVE;
	ret = q1000k_phy_receiver_startup();
	if (ret)
		goto fail;
	if (ready) {
		q1000k_table_phase = Q1000K_TABLE_ACTIVATE;
		WRITE_ONCE(q1000k_table_owner, current);
		ret = ready(arg);
		WRITE_ONCE(q1000k_table_owner, NULL);
		if (ret)
			goto fail;
	}
	ret = q1000k_phy_set_tx(transmit);
	if (ret)
		goto fail;
	ret = q1000k_transport_resume();
	if (ret)
		goto fail;
	/* CPU queues still need explicit provisioning after this succeeds. */
	q1000k_pipeline.stage = Q1000K_PIPELINE_UNDRAINED;
	goto out;
fail:
	q1000k_pipeline.error = ret;
	q1000k_pipeline_contain();
	pr_err("q1000k: activation failed after stage %u: %d containment=%d\n",
	       q1000k_pipeline.stage, ret, q1000k_pipeline.containment_error);
out:
	mutex_unlock(&q1000k_pipeline_lock);
	return ret;
}

int q1000k_pipeline_activate(void)
{
	return q1000k_pipeline_activate_checked(true, NULL, NULL);
}

int q1000k_pipeline_activate_receive_only(void)
{
	return q1000k_pipeline_activate_checked(false, NULL, NULL);
}
