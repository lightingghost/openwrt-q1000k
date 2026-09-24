// SPDX-License-Identifier: GPL-2.0-only
/* Q1000K packet consumer: the native Ethernet driver owns all DMA and NAPI. */
#include <linux/bitfield.h>
#include <q1000k_trace.h>
#include <linux/if_vlan.h>
#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>
#include "common/q1000k_transport.h"
#include "common/q1000k_services.h"
#include "common/q1000k_dhcp6_diag.h"

static bool dhcp6_diag;
module_param(dhcp6_diag, bool, 0600);
MODULE_PARM_DESC(dhcp6_diag, "Enable bounded DHCPv6 header-only bench counters (default off)");
static DEFINE_SPINLOCK(q6d_lock);
static struct {
	u64 count, errors, busy;
	struct q6d_sample last;
	int result;
} q6d_stats[Q6D_STAGES];

bool q1000k_dhcp6_sample(const struct sk_buff *skb, struct q6d_sample *sample)
{
	u8 header[Q6D_HEADER_BYTES];
	unsigned int length;

	if (!READ_ONCE(dhcp6_diag) || !skb)
		return false;
	length = min_t(unsigned int, skb->len, sizeof(header));
	if (skb_copy_bits(skb, 0, header, length))
		return false;
	return q6d_parse(header, length, skb->len, sample);
}

void q1000k_dhcp6_record(enum q6d_stage stage,
		const struct q6d_sample *sample, int result)
{
	unsigned long flags;

	if (!READ_ONCE(dhcp6_diag) || stage >= Q6D_STAGES)
		return;
	spin_lock_irqsave(&q6d_lock, flags);
	q6d_stats[stage].count++;
	q6d_stats[stage].errors += result < 0;
	q6d_stats[stage].busy += stage == Q6D_TX_NATIVE && result == NETDEV_TX_BUSY;
	q6d_stats[stage].last = *sample;
	q6d_stats[stage].result = result;
	spin_unlock_irqrestore(&q6d_lock, flags);
}

static int q6d_get_stats(char *buffer, const struct kernel_param *param)
{
	static const char *const names[] = {
		"tx_select", "tx_service", "tx_native_return",
		"rx_pre_consumer", "rx_select", "rx_consumer_return"
	};
	unsigned long flags;
	unsigned int i;
	int n = 0;

	spin_lock_irqsave(&q6d_lock, flags);
	n += scnprintf(buffer + n, PAGE_SIZE - n, "version=1 enabled=%u\n",
		READ_ONCE(dhcp6_diag));
	for (i = 0; i < Q6D_STAGES; i++) {
		const struct q6d_sample *s = &q6d_stats[i].last;
		n += scnprintf(buffer + n, PAGE_SIZE - n,
			"%s count=%llu errors=%llu busy=%llu result=%d tx=%u type=%u gem=%u tags=%u tci=%u,%u length=%u\n",
			names[i], q6d_stats[i].count, q6d_stats[i].errors,
			q6d_stats[i].busy, q6d_stats[i].result, s->tx, s->type,
			s->gem, s->tags, s->vlan[0], s->vlan[1], s->length);
	}
	spin_unlock_irqrestore(&q6d_lock, flags);
	return n;
}

static const struct kernel_param_ops q6d_stats_ops = { .get = q6d_get_stats };
module_param_cb(dhcp6_diag_stats, &q6d_stats_ops, NULL, 0400);
MODULE_PARM_DESC(dhcp6_diag_stats, "DHCPv6 bench counters; native return is not optical delivery");

static bool pon_rx_diag;
module_param(pon_rx_diag, bool, 0600);
MODULE_PARM_DESC(pon_rx_diag, "Enable IP receive header-only bench counters (default off)");
static struct {
	u64 count, ipv6, reason15, reason30, ifc_hits;
	struct q6d_l2 l2;
	u16 gem, hash, ifc_row;
	u32 word2;
	u8 reason;
	bool pon_present, dmac_matches;
} q4d_stats;

static void q4d_receive(const struct sk_buff *skb,
		const struct airoha_pon_rx_meta *meta)
{
	struct q6d_l2 l2;
	struct net_device *pon;
	u8 header[22], reason;
	bool present, matches;
	unsigned long flags;
	unsigned int length;

	if (!READ_ONCE(pon_rx_diag) || meta->omci)
		return;
	length = min_t(unsigned int, skb->len, sizeof(header));
	if (skb_copy_bits(skb, 0, header, length) ||
	    !q6d_parse_l2(header, length, &l2) ||
	    (l2.proto != ETH_P_IP && l2.proto != ETH_P_IPV6))
		return;
	rcu_read_lock();
	pon = dev_get_by_name_rcu(&init_net, "pon");
	present = !!pon;
	matches = pon && ether_addr_equal(header, pon->dev_addr);
	rcu_read_unlock();
	/* AN7581 QDMA RX word 1: reason bits 20:16, FOE hash bits 15:0. */
	reason = (meta->words[1] >> 16) & 31;
	spin_lock_irqsave(&q6d_lock, flags);
	q4d_stats.count += l2.proto == ETH_P_IP;
	q4d_stats.ipv6 += l2.proto == ETH_P_IPV6;
	q4d_stats.reason15 += reason == 15;
	q4d_stats.reason30 += reason == 30;
	/* AN7581 qdma_dev_7581.h: hit bit 7, nine-bit rule ID 16:8. */
	q4d_stats.ifc_hits += !!(meta->words[2] & BIT(7));
	q4d_stats.ifc_row = (meta->words[2] >> 8) & 0x1ff;
	q4d_stats.word2 = meta->words[2];
	q4d_stats.l2 = l2;
	q4d_stats.gem = meta->gem;
	q4d_stats.hash = meta->words[1] & 0xffff;
	q4d_stats.reason = reason;
	q4d_stats.pon_present = present;
	q4d_stats.dmac_matches = matches;
	spin_unlock_irqrestore(&q6d_lock, flags);
}

static int q4d_get_stats(char *buffer, const struct kernel_param *param)
{
	unsigned long flags;
	int n;

	spin_lock_irqsave(&q6d_lock, flags);
	n = scnprintf(buffer, PAGE_SIZE,
		"version=2 enabled=%u ipv4=%llu reason15=%llu reason30=%llu gem=%u outer=%u inner=%u tags=%u tci=%u,%u reason=%u hash=%u pon_present=%u dmac_matches=%u ipv6=%llu ifc_hits=%llu ifc_row=%u word2=%#010x\n",
		READ_ONCE(pon_rx_diag), q4d_stats.count, q4d_stats.reason15,
		q4d_stats.reason30, q4d_stats.gem, q4d_stats.l2.outer,
		q4d_stats.l2.proto, q4d_stats.l2.tags, q4d_stats.l2.vlan[0],
		q4d_stats.l2.vlan[1], q4d_stats.reason, q4d_stats.hash,
		q4d_stats.pon_present, q4d_stats.dmac_matches, q4d_stats.ipv6,
		q4d_stats.ifc_hits, q4d_stats.ifc_row, q4d_stats.word2);
	spin_unlock_irqrestore(&q6d_lock, flags);
	return n;
}

static const struct kernel_param_ops q4d_stats_ops = { .get = q4d_get_stats };
module_param_cb(pon_rx_diag_stats, &q4d_stats_ops, NULL, 0400);
MODULE_PARM_DESC(pon_rx_diag_stats, "IP counters and latest GEM/VLAN/PPE/IFC sample; no addresses");

#define Q1000K_TX_LIMIT		128
#define Q1000K_TX_BUDGET		32
#define Q1000K_TX_AGE_MS		1000
#define Q1000K_TX_DRAIN_MS	1000

struct q1000k_tx_packet {
	struct list_head list;
	struct sk_buff *skb;
	struct net_device *origin;
	struct airoha_pon_tx_meta meta;
	u64 auth_epoch;
	unsigned long expires;
	bool deferred;
	bool dhcp6;
	struct q6d_sample dhcp6_sample;
};

struct q1000k_transport {
	struct airoha_pon *pon;
	q1000k_pon_receive_t receive;
	spinlock_t lock;
	struct mutex auth_lock;
	u64 auth_epoch, last_auth_epoch;
	struct list_head packets;
	struct delayed_work work;
	unsigned int count; /* Includes the worker's packet outside the list. */
	bool active;
	bool detached;
	bool omci_paused; /* Protected by auth_lock; physical pause is not rekey. */
};

static DEFINE_MUTEX(q1000k_transport_mutex);
static struct q1000k_transport __rcu *q1000k_current;

static int q1000k_tx_meta(u32 word0, u32 word1,
			   struct airoha_pon_tx_meta *meta)
{
	struct airoha_pon_tx_meta result = {
		.gem = FIELD_GET(GENMASK(29, 14), word0),
		.channel = FIELD_GET(GENMASK(7, 3), word0),
		.queue = FIELD_GET(GENMASK(2, 0), word0),
		.mic_index = !!(word0 & BIT(30)),
		.omci = !!(word0 & BIT(8)),
	};
	u32 account0 = FIELD_GET(GENMASK(5, 0), word1);

	/* No fast path, checksum/TSO, PPE, metering or accounting requests.
	 * The imported default for disabled account0 is 31; native AN7581 uses
	 * the full six-bit disabled index 63. Both map to native no-accounting.
	 */
	if ((word0 & (BIT(31) | GENMASK(13, 9))) ||
	    (word1 & GENMASK(14, 11)) ||
	    FIELD_GET(GENMASK(23, 20), word1) != 2 ||
	    FIELD_GET(GENMASK(30, 24), word1) != 0x7f ||
	    FIELD_GET(GENMASK(10, 6), word1) != 0x1f ||
	    (account0 != 0x1f && account0 != 0x3f) ||
	    !!(word1 & BIT(31)) != result.omci ||
	    (!result.omci && result.mic_index) ||
	    (result.omci && result.channel))
		return -EOPNOTSUPP;

	/* Vendor NBOQ follows the T-CONT. The native owner instead fixes CPU
	 * ring/group selection to zero, independently of the optical channel.
	 * Accept only the expected vendor mapping, never a custom DMA group.
	 */
	if (FIELD_GET(GENMASK(19, 15), word1) != result.channel)
		return -EOPNOTSUPP;
	*meta = result;
	return 0;
}

static void q1000k_tx_work(struct work_struct *work)
{
	struct q1000k_transport *transport;
	unsigned int budget = Q1000K_TX_BUDGET;

	transport = container_of(to_delayed_work(work),
				 struct q1000k_transport, work);
	for (;;) {
		struct q1000k_tx_packet *packet;
		bool active;
		netdev_tx_t ret = NETDEV_TX_OK;

		spin_lock_bh(&transport->lock);
		if (list_empty(&transport->packets)) {
			spin_unlock_bh(&transport->lock);
			return;
		}
		if (transport->active && !budget) {
			/* Yield the worker after a bounded batch without imposing a
			 * timer tick on a queue which can still make progress.
			 */
			queue_delayed_work(system_unbound_wq, &transport->work, 0);
			spin_unlock_bh(&transport->lock);
			return;
		}
		packet = list_first_entry(&transport->packets,
					  struct q1000k_tx_packet, list);
		list_del(&packet->list);
		active = transport->active;
		if (active)
			budget--;
		spin_unlock_bh(&transport->lock);

		/* Never hold our lock while entering native TX: its wake callback
		 * may run while a DMA queue lock is held and take our lock.
		 */
		if (packet->meta.omci) {
			/* The rekey barrier waits for this native submission to return.
			 * Never hold the queue lock across native TX or its wake callback.
			 */
			mutex_lock(&transport->auth_lock);
			if (!READ_ONCE(transport->active) || !packet->auth_epoch ||
			    packet->auth_epoch != transport->auth_epoch ||
			    time_after_eq(jiffies, packet->expires)) {
				q1000k_trace(QT_CONTROL, 43, -ESTALE, packet->auth_epoch,
					transport->auth_epoch, packet->meta.gem,
					time_after_eq(jiffies, packet->expires));
				dev_kfree_skb_any(packet->skb);
			} else {
				/* Only authenticated OMCI can survive a physical rebuild.
				 * Pause is serialized with prepare + submission here, and
				 * a rekey still purges queued packets under this same lock.
				 * Never renew the original deadline on a retry.
				 */
				int admission = transport->omci_paused ? -ESHUTDOWN :
					airoha_pon_prepare_tx(transport->pon, &packet->meta);

				if (admission == -ESHUTDOWN) {
					if (!packet->deferred)
						q1000k_trace(QT_CONTROL, 41, admission,
							packet->auth_epoch, packet->meta.gem,
							transport->omci_paused, 0);
					packet->deferred = true;
					ret = NETDEV_TX_BUSY;
				} else if (admission) {
					q1000k_trace(QT_CONTROL, 43, admission,
						packet->auth_epoch, transport->auth_epoch,
						packet->meta.gem, 0);
					dev_kfree_skb_any(packet->skb);
				} else {
					ret = airoha_pon_xmit(transport->pon, packet->skb,
							      &packet->meta);
					if (ret != NETDEV_TX_BUSY)
						q1000k_trace(QT_CONTROL, 42, ret,
							packet->auth_epoch, packet->meta.epoch,
							packet->meta.gem, packet->deferred);
				}
			}
			mutex_unlock(&transport->auth_lock);
		} else if (!active || time_after_eq(jiffies, packet->expires)) {
			if (packet->dhcp6)
				q1000k_dhcp6_record(Q6D_TX_NATIVE,
					&packet->dhcp6_sample, -ETIME);
			dev_kfree_skb_any(packet->skb);
		} else {
			ret = airoha_pon_xmit(transport->pon, packet->skb,
					      &packet->meta);
			/* The native call may consume skb, so use saved numeric fields. */
			if (packet->dhcp6)
				q1000k_dhcp6_record(Q6D_TX_NATIVE,
					&packet->dhcp6_sample, ret);
		}

		spin_lock_bh(&transport->lock);
		if (ret == NETDEV_TX_BUSY && transport->active &&
		    (!packet->meta.omci || packet->auth_epoch == transport->auth_epoch)) {
			list_add(&packet->list, &transport->packets);
			/* A completion can race the failed submission and already
			 * queue an immediate retry. Preserve that pending wake;
			 * only arm the fallback when no retry is queued yet.
			 */
			queue_delayed_work(system_unbound_wq, &transport->work, 1);
			spin_unlock_bh(&transport->lock);
			return;
		}
		transport->count--;
		spin_unlock_bh(&transport->lock);
		if (ret == NETDEV_TX_BUSY)
			dev_kfree_skb_any(packet->skb);
		if (packet->origin)
			dev_put(packet->origin);
		kfree(packet);
	}
}

static void q1000k_native_rx(void *priv, struct sk_buff *skb,
			      const struct airoha_pon_rx_meta *meta)
{
	struct q1000k_transport *transport = priv;
	struct q6d_sample sample;
	bool dhcp6;
	int ret;
	u32 words[4];

	if (!READ_ONCE(transport->active)) {
		dev_kfree_skb_any(skb);
		return;
	}
	/* The vendor signature is mutable. Never expose native callback storage
	 * or retain its pointer beyond this call.
	 */
	memcpy(words, meta->words, sizeof(words));
	q4d_receive(skb, meta);
	dhcp6 = !meta->omci && q1000k_dhcp6_sample(skb, &sample) && !sample.tx;
	if (dhcp6) {
		sample.gem = meta->gem;
		q1000k_dhcp6_record(Q6D_RX_PRE, &sample, 0);
	}
	ret = transport->receive(words, sizeof(words), skb, skb->len);
	if (dhcp6)
		q1000k_dhcp6_record(Q6D_RX_CONSUMER, &sample, ret);
}

static void q1000k_native_wake(void *priv)
{
	struct q1000k_transport *transport = priv;

	spin_lock_bh(&transport->lock);
	if (transport->active && transport->count)
		mod_delayed_work(system_unbound_wq, &transport->work, 0);
	spin_unlock_bh(&transport->lock);
}

static void q1000k_native_detached(void *priv)
{
	struct q1000k_transport *transport = priv;

	spin_lock_bh(&transport->lock);
	transport->active = false;
	transport->detached = true;
	/* Do not enqueue work here: stop may already have cancelled the worker
	 * before asking the native owner to quiesce. Existing work will purge
	 * after an unsolicited detach; stop purges after synchronous cancel.
	 */
	spin_unlock_bh(&transport->lock);
}

static void q1000k_native_tx_status(void *priv, enum airoha_pon_tx_stage stage,
				  int result, const struct airoha_pon_tx_status *status)
{
	/* Header fields only; asynchronous completion is not optical delivery. */
	q1000k_trace(QT_OMCI_NATIVE_TX, stage, result, status->header, status->me,
		     (u32)status->gem << 16 | status->len, status->epoch);
}

static int q1000k_native_flow(void *priv, const struct net_device *upper,
			      u16 ethertype, struct airoha_pon_flow *flow)
{
	struct q1000k_transport *transport = priv;

	if (!q1000k_pwan_data_dev(upper))
		return -ENOENT;
	if (!READ_ONCE(transport->active))
		return -ENOLINK;
	return q1000k_services_flow(ethertype, flow);
}

static int q1000k_native_rx_flow(void *priv, const struct net_device *upper,
				 u16 ethertype, struct airoha_pon_flow *flow)
{
	struct q1000k_transport *transport = priv;

	if (!q1000k_pwan_data_dev(upper))
		return -ENOENT;
	if (!READ_ONCE(transport->active))
		return -ENOLINK;
	return q1000k_services_rx_flow(ethertype, flow);
}

void q1000k_transport_invalidate_flows(void)
{
	struct q1000k_transport *transport;

	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (transport)
		airoha_pon_invalidate_flows(transport->pon);
	rcu_read_unlock();
}

static const struct airoha_pon_ops q1000k_native_ops = {
	.flow = q1000k_native_flow,
	.rx_flow = q1000k_native_rx_flow,
	.rx = q1000k_native_rx,
	.tx_wake = q1000k_native_wake,
	.tx_status = q1000k_native_tx_status,
	.detached = q1000k_native_detached,
};

bool q1000k_transport_running(void)
{
	struct q1000k_transport *transport;
	bool active;

	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	active = transport && READ_ONCE(transport->active);
	rcu_read_unlock();
	return active;
}

int q1000k_transport_set_queue_close(u8 channel, u8 closed)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_set_queue_close(transport->pon, channel, closed);
	rcu_read_unlock();
	return ret;
}

int q1000k_transport_set_tx_channel(u8 channel, bool enabled)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_set_tx_channel(transport->pon, channel, enabled);
	rcu_read_unlock();
	return ret;
}

int q1000k_transport_get_queue_close(u8 channel, u8 *closed)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_get_queue_close(transport->pon, channel, closed);
	rcu_read_unlock();
	return ret;
}

int q1000k_transport_set_qos(u8 channel, const struct airoha_pon_qos *qos)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_set_qos(transport->pon, channel, qos);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_get_port_config(struct airoha_pon_port_config *config)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_get_port_config(transport->pon, config);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_configure_port(const struct airoha_pon_port_config *expected,
		const struct airoha_pon_port_config *config)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_configure_port(transport->pon, expected, config);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_pause(unsigned int timeout_ms)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active)) {
		mutex_lock(&transport->auth_lock);
		transport->omci_paused = true;
		ret = airoha_pon_pause(transport->pon, timeout_ms);
		q1000k_trace(QT_CONTROL, 44, ret, transport->auth_epoch, 1, 0, 0);
		mutex_unlock(&transport->auth_lock);
	}
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_retire_fe(u8 channel)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_retire_fe(transport->pon, channel);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_drain_rx(void)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_drain_rx(transport->pon);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_reset_epoch(void)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_reset_epoch(transport->pon);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_activate_rx(u32 channels)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_activate_rx(transport->pon, channels);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_resume(void)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active)) {
		mutex_lock(&transport->auth_lock);
		ret = airoha_pon_resume(transport->pon);
		if (!ret)
			transport->omci_paused = false;
		q1000k_trace(QT_CONTROL, 44, ret, transport->auth_epoch, 0, 0, 0);
		mutex_unlock(&transport->auth_lock);
		if (!ret)
			q1000k_native_wake(transport);
	}
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_get_qos(u8 channel, struct airoha_pon_qos *qos)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_get_qos(transport->pon, channel, qos);
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_quiesce_channel(u8 channel)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_quiesce_channel(transport->pon, channel);
	rcu_read_unlock();
	return ret;
}

int q1000k_transport_start(const char *lower, q1000k_pon_receive_t receive)
{
	struct q1000k_transport *transport;
	struct net_device *dev;
	int ret = 0;

	if (!lower || !*lower || !dev_valid_name(lower) || !receive)
		return -EINVAL;
	mutex_lock(&q1000k_transport_mutex);
	if (rcu_access_pointer(q1000k_current)) {
		ret = -EBUSY;
		goto unlock;
	}
	dev = dev_get_by_name(&init_net, lower);
	if (!dev) {
		ret = -ENODEV;
		goto unlock;
	}
	transport = kzalloc(sizeof(*transport), GFP_KERNEL);
	if (!transport) {
		dev_put(dev);
		ret = -ENOMEM;
		goto unlock;
	}
	spin_lock_init(&transport->lock);
	mutex_init(&transport->auth_lock);
	INIT_LIST_HEAD(&transport->packets);
	INIT_DELAYED_WORK(&transport->work, q1000k_tx_work);
	transport->receive = receive;
	transport->pon = airoha_pon_attach(dev, &q1000k_native_ops, transport);
	dev_put(dev);
	if (IS_ERR(transport->pon)) {
		ret = PTR_ERR(transport->pon);
		kfree(transport);
		goto unlock;
	}
	spin_lock_bh(&transport->lock);
	if (transport->detached) {
		spin_unlock_bh(&transport->lock);
		airoha_pon_release(transport->pon);
		kfree(transport);
		ret = -ENODEV;
		goto unlock;
	}
	transport->active = true;
	rcu_assign_pointer(q1000k_current, transport);
	spin_unlock_bh(&transport->lock);
unlock:
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

int q1000k_transport_stop(void)
{
	struct q1000k_transport *transport;
	struct q1000k_tx_packet *packet, *tmp;
	int ret = 0;

	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (!transport)
		goto unlock;
	rcu_assign_pointer(q1000k_current, NULL);
	spin_lock_bh(&transport->lock);
	transport->active = false;
	spin_unlock_bh(&transport->lock);
	synchronize_rcu();
	cancel_delayed_work_sync(&transport->work);
	ret = airoha_pon_quiesce(transport->pon, Q1000K_TX_DRAIN_MS);
	airoha_pon_release(transport->pon);
	list_for_each_entry_safe(packet, tmp, &transport->packets, list) {
		list_del(&packet->list);
		dev_kfree_skb_any(packet->skb);
		if (packet->origin)
			dev_put(packet->origin);
		kfree(packet);
	}
	kfree(transport);
unlock:
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}

static int q1000k_transport_queue(struct sk_buff *skb, u32 word0, u32 word1,
				   u64 auth_epoch)
{
	struct airoha_pon_tx_meta meta;
	struct q1000k_transport *transport;
	struct q1000k_tx_packet *packet;
	int ret;

	ret = q1000k_tx_meta(word0, word1, &meta);
	if (ret)
		return ret;
	if (meta.omci != !!auth_epoch)
		return -EKEYREJECTED;
	if (!skb || skb_shared(skb) || !skb->len || skb->len > 16128 ||
	    !skb_headlen(skb))
		return -EINVAL;
	if (skb_is_gso(skb) || skb_has_frag_list(skb) ||
	    skb->ip_summed == CHECKSUM_PARTIAL || skb_vlan_tag_present(skb))
		return -EOPNOTSUPP;
	packet = kmalloc(sizeof(*packet), GFP_ATOMIC);
	if (!packet)
		return -ENOMEM;
	packet->skb = skb;
	packet->origin = skb->dev;
	/* skb->dev is not itself a reference. An upper can be unregistered
	 * while a prepared packet waits; retain it until native consumption
	 * (which switches skb->dev to the lower) or the software drop finishes.
	 */
	if (packet->origin)
		dev_hold(packet->origin);
	packet->meta = meta;
	packet->auth_epoch = auth_epoch;
	packet->expires = jiffies + msecs_to_jiffies(Q1000K_TX_AGE_MS);
	packet->deferred = false;
	packet->dhcp6 = !meta.omci &&
		q1000k_dhcp6_sample(skb, &packet->dhcp6_sample) &&
		packet->dhcp6_sample.tx;
	if (packet->dhcp6)
		packet->dhcp6_sample.gem = meta.gem;
	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (!transport) {
		ret = -ENODEV;
		goto unlock;
	}
	/* Data frames capture admission once: they must never inherit a reused
	 * channel. Authenticated OMCI instead waits above native admission;
	 * the worker verifies its immutable auth epoch before each submission.
	 */
	if (!meta.omci) {
		ret = airoha_pon_prepare_tx(transport->pon, &packet->meta);
		if (ret)
			goto unlock;
	}
	spin_lock_bh(&transport->lock);
	if (!transport->active)
		ret = -ENODEV;
	else if (meta.omci && auth_epoch != transport->auth_epoch)
		ret = -EKEYREJECTED;
	else if (transport->count == Q1000K_TX_LIMIT)
		ret = -ENOBUFS;
	else {
		list_add_tail(&packet->list, &transport->packets);
		transport->count++;
		mod_delayed_work(system_unbound_wq, &transport->work, 0);
		ret = 0;
	}
	spin_unlock_bh(&transport->lock);
unlock:
	rcu_read_unlock();
	if (ret) {
		if (packet->origin)
			dev_put(packet->origin);
		kfree(packet);
	}
	return ret;
}

int q1000k_transport_xmit(struct sk_buff *skb, u32 word0, u32 word1)
{
	/* Unauthenticated legacy OMCI callers cannot bypass the session owner. */
	return q1000k_transport_queue(skb, word0, word1, 0);
}

int q1000k_transport_xmit_omci(struct sk_buff *skb, u16 gem, u8 mic_index,
			     u64 auth_epoch)
{
	u32 word0;

	if (gem == 0xffff || mic_index > 1)
		return -EINVAL;
	word0 = FIELD_PREP(GENMASK(29, 14), gem) | BIT(8) |
		(mic_index ? BIT(30) : 0);
	/* OMCI uses channel/queue/CPU ring zero and no metering/accounting. */
	return q1000k_transport_queue(skb, word0, 0xff2007ff, auth_epoch);
}

int q1000k_transport_set_auth_epoch(u64 auth_epoch)
{
	struct q1000k_transport *transport;
	struct q1000k_tx_packet *packet, *tmp;
	LIST_HEAD(discard);
	int ret = 0;

	if (in_interrupt() || in_atomic() || irqs_disabled() ||
	    rcu_preempt_depth() ||
	    (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held()))
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (!transport) {
		ret = -ENODEV;
		goto unlock;
	}
	/* lifecycle -> authentication -> queue; native wake takes queue only. */
	mutex_lock(&transport->auth_lock);
	spin_lock_bh(&transport->lock);
	if (!transport->active)
		ret = -ENODEV;
	else if (auth_epoch == transport->auth_epoch)
		goto complete;
	else if (auth_epoch && auth_epoch <= transport->last_auth_epoch)
		ret = -ESTALE;
	if (ret)
		goto complete;
	transport->auth_epoch = auth_epoch;
	if (auth_epoch)
		transport->last_auth_epoch = auth_epoch;
	list_for_each_entry_safe(packet, tmp, &transport->packets, list) {
		if (!packet->meta.omci)
			continue;
		list_move_tail(&packet->list, &discard);
		transport->count--;
	}
complete:
	spin_unlock_bh(&transport->lock);
	list_for_each_entry_safe(packet, tmp, &discard, list) {
		list_del(&packet->list);
		dev_kfree_skb_any(packet->skb);
		if (packet->origin)
			dev_put(packet->origin);
		kfree(packet);
	}
	mutex_unlock(&transport->auth_lock);
unlock:
	mutex_unlock(&q1000k_transport_mutex);
	return ret;
}
