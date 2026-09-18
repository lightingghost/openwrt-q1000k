// SPDX-License-Identifier: GPL-2.0-only
/* Q1000K packet consumer: the native Ethernet driver owns all DMA and NAPI. */
#include <linux/bitfield.h>
#include <q1000k_trace.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>
#include "common/q1000k_transport.h"

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
			mod_delayed_work(system_unbound_wq, &transport->work, 1);
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
			dev_kfree_skb_any(packet->skb);
		} else {
			ret = airoha_pon_xmit(transport->pon, packet->skb,
					      &packet->meta);
		}

		spin_lock_bh(&transport->lock);
		if (ret == NETDEV_TX_BUSY && transport->active &&
		    (!packet->meta.omci || packet->auth_epoch == transport->auth_epoch)) {
			list_add(&packet->list, &transport->packets);
			/* A completion can race the failed submission. Always arm a
			 * bounded fallback; tx_wake can expedite it but isn't required.
			 */
			mod_delayed_work(system_unbound_wq, &transport->work, 1);
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
	u32 words[4];

	if (!READ_ONCE(transport->active)) {
		dev_kfree_skb_any(skb);
		return;
	}
	/* The vendor signature is mutable. Never expose native callback storage
	 * or retain its pointer beyond this call.
	 */
	memcpy(words, meta->words, sizeof(words));
	transport->receive(words, sizeof(words), skb, skb->len);
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

static const struct airoha_pon_ops q1000k_native_ops = {
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
