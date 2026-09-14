// SPDX-License-Identifier: GPL-2.0-only
/* Complete physical port retirement; no service record is released here. */
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <an7581_xpon.h>
#include <q1000k_phy_api.h>
#include "common/q1000k_transport.h"
#include "common/q1000k_pipeline.h"

#define Q1000K_MAC_ALL_STOPS (AN7581_XPON_MBI_RX_STOP | \
	AN7581_XPON_MBI_TX_STOP | AN7581_XPON_MPI_RX_STOP | \
	AN7581_XPON_MPI_TX_STOP)

static DEFINE_MUTEX(q1000k_pipeline_lock);
static struct q1000k_pipeline_status q1000k_pipeline;

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
	 * No reset or ID reuse precedes this boundary.
	 */
	ret = q1000k_phy_quiesce();
	if (ret)
		goto fail;
	q1000k_pipeline.stage = Q1000K_PIPELINE_PHY_STOPPED;
	goto out;
fail:
	q1000k_pipeline.error = ret;
	q1000k_pipeline_contain();
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
