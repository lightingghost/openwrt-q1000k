// SPDX-License-Identifier: GPL-2.0-only
/* Q1000K packet consumer: the native Ethernet driver owns all DMA and NAPI. */
#include <linux/bitfield.h>
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
	unsigned long expires;
};

struct q1000k_transport {
	struct airoha_pon *pon;
	q1000k_pon_receive_t receive;
	spinlock_t lock;
	struct list_head packets;
	struct delayed_work work;
	unsigned int count; /* Includes the worker's packet outside the list. */
	bool active;
	bool detached;
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
		if (!active || time_after_eq(jiffies, packet->expires))
			dev_kfree_skb_any(packet->skb);
		else
			ret = airoha_pon_xmit(transport->pon, packet->skb,
					      &packet->meta);

		spin_lock_bh(&transport->lock);
		if (ret == NETDEV_TX_BUSY && transport->active) {
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

static const struct airoha_pon_ops q1000k_native_ops = {
	.rx = q1000k_native_rx,
	.tx_wake = q1000k_native_wake,
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

int q1000k_transport_pause(unsigned int timeout_ms)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_pause(transport->pon, timeout_ms);
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

int q1000k_transport_resume(void)
{
	struct q1000k_transport *transport;
	int ret = -ENODEV;

	if (in_interrupt() || irqs_disabled())
		return -EWOULDBLOCK;
	mutex_lock(&q1000k_transport_mutex);
	transport = rcu_dereference_protected(q1000k_current,
				lockdep_is_held(&q1000k_transport_mutex));
	if (transport && READ_ONCE(transport->active))
		ret = airoha_pon_resume(transport->pon);
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

int q1000k_transport_xmit(struct sk_buff *skb, u32 word0, u32 word1)
{
	struct airoha_pon_tx_meta meta;
	struct q1000k_transport *transport;
	struct q1000k_tx_packet *packet;
	int ret;

	ret = q1000k_tx_meta(word0, word1, &meta);
	if (ret)
		return ret;
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
	packet->expires = jiffies + msecs_to_jiffies(Q1000K_TX_AGE_MS);
	rcu_read_lock();
	transport = rcu_dereference(q1000k_current);
	if (!transport) {
		ret = -ENODEV;
		goto unlock;
	}
	/* Capture admission exactly once. Rechecking it on BUSY would allow a
	 * pre-retirement frame to inherit a reused channel. Native submission
	 * rejects stale epochs even if closure races this enqueue or its worker.
	 * Do not take our lock before entering native admission (TX may wake us).
	 */
	ret = airoha_pon_prepare_tx(transport->pon, &packet->meta);
	if (ret)
		goto unlock;
	spin_lock_bh(&transport->lock);
	if (!transport->active)
		ret = -ENODEV;
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
