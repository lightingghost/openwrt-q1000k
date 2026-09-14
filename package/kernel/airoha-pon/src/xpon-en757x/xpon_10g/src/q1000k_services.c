// SPDX-License-Identifier: GPL-2.0-only
/* OMCI entities and ANI-side service selection for the native PON datapath. */
#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "common/q1000k_services.h"
#include "common/q1000k_gwan.h"
#include "common/q1000k_protocol.h"
#include "common/q1000k_transport.h"

#define QS_TCONT_BASE 0x8000
#define QS_TCONTS 31
#define QS_MAX 256
struct qs_gem { u16 entity, gem, tcont; u8 direction; bool valid; };
struct qs_rules { size_t count; u8 channels[QS_MAX]; struct omci_service_config rule[]; };
struct qs_scheduler { u8 policy, weight[8]; };
static u16 qs_alloc[QS_TCONTS];
static struct qs_scheduler qs_schedulers[QS_TCONTS];
static struct qs_gem qs_gems[QS_MAX];
static bool qs_uni[4];
static bool qs_enabled, qs_changing;
static struct qs_rules __rcu *qs_current;

static int qs_uni_index(u16 entity)
{
	/* The core normalizes ordinary UNI entity IDs to their port byte. */
	if ((entity >= 1 && entity <= 4) || (entity >= 0x101 && entity <= 0x104))
		return (entity & 0xff) - 1;
	if (entity == 0xa01 || entity == 0x601)
		return 0;
	return -EOPNOTSUPP;
}

void q1000k_services_init(void)
{
	memset(qs_alloc, 0xff, sizeof(qs_alloc));
	memset(qs_schedulers, 1, sizeof(qs_schedulers));
	memset(qs_gems, 0, sizeof(qs_gems));
	memset(qs_uni, 1, sizeof(qs_uni));
	qs_enabled = qs_changing = false;
	RCU_INIT_POINTER(qs_current, NULL);
}

void q1000k_services_destroy(void)
{
	struct qs_rules *old = rcu_access_pointer(qs_current);

	WRITE_ONCE(qs_enabled, false);
	RCU_INIT_POINTER(qs_current, NULL);
	synchronize_rcu();
	kfree(old);
}

void q1000k_services_enable(bool enabled)
{
	WRITE_ONCE(qs_enabled, enabled);
}

void q1000k_services_reset(void)
{
	struct qs_rules *old;

	WARN_ON_ONCE(!q1000k_protocol_owned());
	WRITE_ONCE(qs_changing, true);
	old = rcu_replace_pointer(qs_current, NULL, q1000k_protocol_owned());
	synchronize_rcu();
	kfree(old);
	memset(qs_alloc, 0xff, sizeof(qs_alloc));
	memset(qs_schedulers, 1, sizeof(qs_schedulers));
	memset(qs_gems, 0, sizeof(qs_gems));
	memset(qs_uni, 1, sizeof(qs_uni));
	WRITE_ONCE(qs_changing, false);
}

int q1000k_services_topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	if (!t)
		return -EINVAL;
	*t = (struct omci_ani_topology) {
		.tcont_base = QS_TCONT_BASE, .scheduler_base = 0x8000, .queue_base = 0x8000,
		.tcont_count = QS_TCONTS, .queues_per_tcont = 8,
		/* Queue storage is shared dynamically, not statically reserved. */
		.maximum_queue_size = 0xffff, .allocated_queue_size = 0,
		.queue_config_option = 1, .scheduler_policy = 1,
	};
	return 0;
}

static int qs_channel(const struct q1000k_gwan_table *t, u16 alloc)
{
	unsigned int i;

	for (i = 1; i < Q1000K_GWAN_CHANNELS; i++)
		if (t->alloc_id[i] == alloc)
			return i;
	return Q1000K_GWAN_UNKNOWN_CHANNEL;
}

static int qs_record(const struct q1000k_gwan_table *t, u16 gem)
{
	unsigned int i;

	for (i = 0; i < Q1000K_GWAN_GEMS; i++)
		if (t->gem[i].valid && t->gem[i].gem == gem)
			return i;
	return -ENOENT;
}

int q1000k_services_tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid)
{
	struct q1000k_gwan_table *tables;
	unsigned int i, index;
	bool was_changing;
	int ret, token;

	if (entity < QS_TCONT_BASE || entity >= QS_TCONT_BASE + QS_TCONTS ||
	    (valid && alloc > 0x3fff && alloc != 0xffff))
		return -EINVAL;
	if (!valid)
		alloc = 0xffff;
	index = entity - QS_TCONT_BASE;
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	ret = 0;
	if (qs_alloc[index] == alloc)
		goto leave;
	for (i = 0; i < QS_TCONTS; i++)
		if (i != index && alloc != 0xffff && qs_alloc[i] == alloc) {
			ret = -EEXIST;
			goto leave;
		}
	tables = kcalloc(2, sizeof(*tables), GFP_KERNEL);
	if (!tables) { ret = -ENOMEM; goto leave; }
	ret = q1000k_gwan_snapshot(&tables[0]);
	if (ret)
		goto free;
	tables[1] = tables[0];
	for (i = 0; i < QS_MAX; i++) {
		int n;

		if (!qs_gems[i].valid || qs_gems[i].tcont != entity)
			continue;
		if (alloc == 0xffff) { ret = -EBUSY; goto free; }
		n = qs_record(&tables[1], qs_gems[i].gem);
		if (n < 0) { ret = -ESTALE; goto free; }
		tables[1].gem[n].alloc_id = alloc;
		tables[1].gem[n].channel = qs_channel(&tables[1], alloc);
	}
	was_changing = READ_ONCE(qs_changing);
	WRITE_ONCE(qs_changing, true);
	ret = q1000k_gwan_apply(&tables[0], &tables[1]);
	if (!ret)
		qs_alloc[index] = alloc;
	/* The core reconciles the resulting complete service set next. */
	if (ret && ret != -EUCLEAN)
		WRITE_ONCE(qs_changing, was_changing);
free:
	kfree(tables);
leave:
	q1000k_protocol_leave(token);
	return ret;
}

int q1000k_services_gem(struct omci_device *odev, u16 entity, u16 gem, u16 tcont,
		       u8 direction, bool valid, bool encrypted)
{
	struct q1000k_gwan_table *tables;
	int token, ret, slot = -1, record = -1;
	unsigned int i;
	bool was_changing;
	u16 alloc = 0xffff;

	if (valid && (!gem || direction < 1 || direction > 3 ||
		      tcont < QS_TCONT_BASE || tcont >= QS_TCONT_BASE + QS_TCONTS))
		return -EINVAL;
	if (encrypted)
		return -EOPNOTSUPP;
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	for (i = 0; i < QS_MAX; i++) {
		if (qs_gems[i].valid && qs_gems[i].entity == entity) { slot = i; break; }
		if (!qs_gems[i].valid && slot < 0) slot = i;
	}
	ret = -ENOSPC;
	if (slot < 0)
		goto leave;
	if (!valid && !qs_gems[slot].valid) { ret = 0; goto leave; }
	if (valid) {
		alloc = qs_alloc[tcont - QS_TCONT_BASE];
		if (alloc == 0xffff) { ret = -ENODATA; goto leave; }
		for (i = 0; i < QS_MAX; i++)
			if ((int)i != slot && qs_gems[i].valid && qs_gems[i].gem == gem) {
				ret = -EEXIST; goto leave;
			}
	}
	tables = kcalloc(2, sizeof(*tables), GFP_KERNEL);
	if (!tables) { ret = -ENOMEM; goto leave; }
	ret = q1000k_gwan_snapshot(&tables[0]);
	if (ret)
		goto free;
	tables[1] = tables[0];
	if (qs_gems[slot].valid) {
		record = qs_record(&tables[1], qs_gems[slot].gem);
		if (record < 0) { ret = -ESTALE; goto free; }
		tables[1].gem[record].valid = false;
	}
	if (valid) {
		if (qs_record(&tables[1], gem) >= 0) { ret = -EEXIST; goto free; }
		if (record < 0)
			for (i = 0; i < QS_MAX; i++)
				if (!tables[1].gem[i].valid) { record = i; break; }
		if (record < 0) { ret = -ENOSPC; goto free; }
		tables[1].gem[record] = (struct q1000k_gwan_entry) {
			.valid = true, .gem = gem, .alloc_id = alloc, .ani = 1,
			.channel = qs_channel(&tables[1], alloc),
		};
	}
	was_changing = READ_ONCE(qs_changing);
	WRITE_ONCE(qs_changing, true);
	ret = q1000k_gwan_apply(&tables[0], &tables[1]);
	if (!ret)
		qs_gems[slot] = (struct qs_gem) {
			.valid = valid, .entity = entity, .gem = gem, .tcont = tcont, .direction = direction,
		};
	if (ret && ret != -EUCLEAN)
		WRITE_ONCE(qs_changing, was_changing);
free:
	kfree(tables);
leave:
	q1000k_protocol_leave(token);
	return ret;
}

int q1000k_services_uni(struct omci_device *odev, u16 entity, bool enabled)
{
	int token, index = qs_uni_index(entity);

	if (index < 0)
		return index;
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	WRITE_ONCE(qs_uni[index], enabled);
	q1000k_protocol_leave(token);
	return 0;
}

struct qs_qos_update {
	u8 channel;
	struct qs_scheduler scheduler;
};

/* The namespace owner invokes this only after full physical drain, with
 * queues closed. Read the actual global units instead of guessing byte/packet
 * policy or changing units shared with OMCC and other Alloc-IDs.
 */
static int qs_qos_install(void *arg)
{
	const struct qs_qos_update *update = arg;
	struct airoha_pon_qos qos;
	unsigned int i;
	int ret = q1000k_transport_get_qos(update->channel, &qos);

	if (ret)
		return ret;
	qos.mode = update->scheduler.policy == 1 ? 1 : 0;
	for (i = 0; i < 8; i++)
		qos.weights[i] = qos.mode == 1 ? 0 : update->scheduler.weight[i];
	return q1000k_transport_set_qos(update->channel, &qos);
}

static int qs_scheduler_update(unsigned int index, const struct qs_scheduler *candidate)
{
	struct q1000k_gwan_table *table;
	struct qs_qos_update update = { .scheduler = *candidate };
	bool was_changing;
	int ret, channel;

	if (!memcmp(&qs_schedulers[index], candidate, sizeof(*candidate)))
		return 0;
	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	ret = q1000k_gwan_snapshot(table);
	if (ret)
		goto free;
	channel = qs_alloc[index] == 0xffff ? Q1000K_GWAN_UNKNOWN_CHANNEL :
		qs_channel(table, qs_alloc[index]);
	if (channel != Q1000K_GWAN_UNKNOWN_CHANNEL) {
		update.channel = channel;
		was_changing = READ_ONCE(qs_changing);
		WRITE_ONCE(qs_changing, true);
		ret = q1000k_gwan_refresh(qs_qos_install, &update);
		if (ret != -EUCLEAN)
			WRITE_ONCE(qs_changing, was_changing);
		if (ret)
			goto free;
	}
	/* Unassigned T-CONTs retain their scheduler intent. Service activation
	 * programs it into the PLOAM-selected channel before opening queues.
	 */
	qs_schedulers[index] = *candidate;
free:
	kfree(table);
	return ret;
}

int q1000k_services_queue(struct omci_device *odev, u16 entity,
			 const struct omci_priority_queue_config *q)
{
	struct qs_scheduler candidate;
	unsigned int index, queue;
	int token, ret;

	if (!q || entity < 0x8000 || entity >= 0x8000 + QS_TCONTS * 8)
		return -EINVAL;
	index = (entity - 0x8000) / 8;
	queue = (entity - 0x8000) % 8;
	/* Shared buffers and fixed queue wiring have no per-queue reservation,
 * discard-reset or backpressure implementation. Do not acknowledge changes.
 */
	if (q->configuration != 1 || q->maximum_size != 0xffff ||
	    q->allocated_size || q->discard_reset || q->discard_threshold ||
	    q->backpressure_operation || q->backpressure_time ||
	    q->backpressure_occur != 0xffff || q->backpressure_clear ||
	    q->tcont_entity_id != QS_TCONT_BASE + index ||
	    q->scheduler_entity_id != 0x8000 + index || q->priority != 7 - queue)
		return -EOPNOTSUPP;
	if (!q->weight || q->weight > 127)
		return -ERANGE;
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	candidate = qs_schedulers[index];
	candidate.weight[queue] = q->weight;
	ret = qs_scheduler_update(index, &candidate);
	q1000k_protocol_leave(token);
	return ret;
}

int q1000k_services_scheduler(struct omci_device *odev, u16 entity,
			 const struct omci_traffic_scheduler_config *s)
{
	struct qs_scheduler candidate;
	unsigned int index;
	int token, ret;

	if (!s || entity < 0x8000 || entity >= 0x8000 + QS_TCONTS)
		return -EINVAL;
	index = entity - 0x8000;
	if (s->tcont_entity_id != QS_TCONT_BASE + index || s->parent_entity_id ||
	    s->priority || (s->policy != 1 && s->policy != 2))
		return -EOPNOTSUPP;
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	candidate = qs_schedulers[index];
	candidate.policy = s->policy;
	ret = qs_scheduler_update(index, &candidate);
	q1000k_protocol_leave(token);
	return ret;
}

static int qs_service_qos_install(void *arg)
{
	const struct qs_rules *rules = arg;
	bool installed[32] = { false };
	size_t i;

	for (i = 0; i < rules->count; i++) {
		const struct omci_service_config *s = &rules->rule[i];
		struct qs_qos_update update;
		/* The caller captured channel IDs while it owned the namespace.
		 * Binding lookup is closed inside the installation phase.
		 */
		int ret;
		unsigned int channel = rules->channels[i];

		if (installed[channel])
			continue;
		update.channel = channel;
		update.scheduler = qs_schedulers[s->tcont_entity_id - QS_TCONT_BASE];
		ret = qs_qos_install(&update);
		if (ret)
			return ret;
		installed[channel] = true;
	}
	return 0;
}

int q1000k_services_replace(struct omci_device *odev,
		const struct omci_service_config *services, size_t count)
{
	struct q1000k_gwan_binding binding;
	struct qs_rules *next, *old;
	u8 closed[32];
	size_t i, j;
	int ret = 0, token;
	bool was_changing;

	if (count > QS_MAX || (count && !services))
		return -EINVAL;
	next = kzalloc(struct_size(next, rule, count), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	next->count = count;
	if (count)
		memcpy(next->rule, services, count * sizeof(*services));
	token = q1000k_protocol_enter();
	if (token < 0) { kfree(next); return token; }
	was_changing = READ_ONCE(qs_changing);
	memset(closed, 255, sizeof(closed));
	for (i = 0; i < count; i++) {
		const struct omci_service_config *s = &services[i];
		int uni = qs_uni_index(s->uni_entity_id);

		if (uni < 0 || s->queue > 7 || (s->pcp_valid && s->pcp > 7) ||
		    s->tcont_entity_id < QS_TCONT_BASE || s->tcont_entity_id >= QS_TCONT_BASE + QS_TCONTS ||
		    (s->vlan_valid && s->vlan_id > 4094) || s->direction < 1 || s->direction > 3) {
			ret = -EINVAL; goto free;
		}
		if (s->default_service && (s->vlan_valid || s->pcp_valid)) { ret = -EINVAL; goto free; }
		if (s->multicast || s->vlan_treatment_valid || s->multicast_ani_valid) {
			ret = -EOPNOTSUPP; goto free;
		}
		ret = q1000k_gwan_binding(s->gem_port_id, true, &binding);
		if (ret)
			goto free;
		if (binding.alloc_id != s->alloc_id) { ret = -ESTALE; goto free; }
		if (qs_alloc[s->tcont_entity_id - QS_TCONT_BASE] != s->alloc_id) { ret = -ESTALE; goto free; }
		next->channels[i] = binding.channel;
		for (j = 0; j < i; j++) {
			const struct omci_service_config *p = &services[j];

			/* Equal specificity with an overlapping match is ambiguous on
			 * this single ANI netdevice, including across logical UNIs.
			 */
			if (s->cookie == p->cookie ||
			    (s->default_service && p->default_service) ||
			    (s->vlan_valid == p->vlan_valid && s->pcp_valid == p->pcp_valid &&
			     (!s->vlan_valid || s->vlan_id == p->vlan_id) &&
			     (!s->pcp_valid || s->pcp == p->pcp))) {
				ret = -EEXIST; goto free;
			}
		}
		if (s->direction != OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)
			closed[binding.channel] &= ~BIT(s->queue);
	}
	WRITE_ONCE(qs_changing, true);
	/* Retire old classifier-selected traffic, including native retries and
	 * downstream frames, even when the GEM record set itself is identical.
	 */
	ret = q1000k_gwan_refresh(qs_service_qos_install, next);
	if (ret) {
		if (ret != -EUCLEAN) WRITE_ONCE(qs_changing, was_changing);
		goto free;
	}
	for (i = 1; i < 32; i++) {
		ret = q1000k_transport_set_queue_close(i, closed[i]);
		if (ret) {
			q1000k_protocol_fail(ret);
			ret = -EUCLEAN;
			goto free;
		}
	}
	old = rcu_replace_pointer(qs_current, next, q1000k_protocol_owned());
	next = NULL;
	synchronize_rcu();
	kfree(old);
	WRITE_ONCE(qs_changing, false);
free:
	kfree(next);
	q1000k_protocol_leave(token);
	return ret;
}

static int qs_tag(const struct sk_buff *skb, bool *tagged, u16 *vid, u8 *pcp)
{
	u8 data[18];
	u16 proto, tci;

	if (skb->len < ETH_HLEN || skb_copy_bits(skb, 0, data, ETH_HLEN))
		return -EMSGSIZE;
	proto = get_unaligned_be16(data + 12);
	*tagged = false; *vid = 0; *pcp = 0;
	if (skb_vlan_tag_present(skb)) {
		tci = skb_vlan_tag_get(skb);
	} else {
		if (proto != ETH_P_8021Q && proto != ETH_P_8021AD)
			return 0;
		if (skb->len < sizeof(data) || skb_copy_bits(skb, 0, data, sizeof(data)))
			return -EMSGSIZE;
		tci = get_unaligned_be16(data + 14);
	}
	*tagged = true; *vid = tci & VLAN_VID_MASK; *pcp = tci >> VLAN_PRIO_SHIFT;
	return 0;
}

int q1000k_services_tx(struct sk_buff *skb)
{
	const struct omci_service_config *selected = NULL;
	struct qs_rules *rules = rcu_dereference(qs_current);
	struct q1000k_gwan_binding binding;
	int score = -1, ret;
	size_t i;
	u16 vid;
	u8 pcp;
	bool tagged;

	if (!READ_ONCE(qs_enabled) || READ_ONCE(qs_changing) || q1000k_protocol_status())
		return -ENOLINK;
	ret = qs_tag(skb, &tagged, &vid, &pcp);
	if (ret)
		return ret;
	if (!rules)
		return -ENODATA;
	for (i = 0; i < rules->count; i++) {
		const struct omci_service_config *s = &rules->rule[i];
		int rank = (s->vlan_valid ? 2 : 0) + s->pcp_valid;

		if (!READ_ONCE(qs_uni[qs_uni_index(s->uni_entity_id)]) ||
		    s->direction == OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI ||
		    (s->vlan_valid && (!tagged || s->vlan_id != vid)) ||
		    (s->pcp_valid && (!tagged || s->pcp != pcp)))
			continue;
		if (s->default_service) rank = 0;
		if (rank > score) { score = rank; selected = s; }
	}
	if (!selected)
		return -ENOENT;
	if (skb_shared(skb) || skb_is_gso(skb))
		return -EOPNOTSUPP;
	if (skb_linearize(skb))
		return -ENOMEM;
	if (skb_vlan_tag_present(skb)) {
		ret = __vlan_insert_tag(skb, skb->vlan_proto, skb_vlan_tag_get(skb));
		if (ret)
			return ret;
		skb->protocol = skb->vlan_proto;
		__vlan_hwaccel_clear_tag(skb);
	}
	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb))
		return -EINVAL;
	skb->ip_summed = CHECKSUM_NONE;
	if (skb->len > 2000)
		return -EMSGSIZE;
	ret = __skb_put_padto(skb, ETH_ZLEN, false);
	if (ret)
		return ret;
	ret = q1000k_gwan_binding(selected->gem_port_id, true, &binding);
	if (ret)
		return ret;
	if (binding.alloc_id != selected->alloc_id)
		return -ESTALE;
	return q1000k_transport_xmit(skb, ((u32)binding.gem << 14) |
		((u32)binding.channel << 3) | selected->queue,
		0x7f2007ff | ((u32)binding.channel << 15));
}

int q1000k_services_rx(struct sk_buff *skb, u16 gem)
{
	struct qs_rules *rules = rcu_dereference(qs_current);
	struct q1000k_gwan_binding binding;
	size_t i;
	u16 vid;
	u8 pcp;
	bool tagged;
	int ret;

	if (!READ_ONCE(qs_enabled) || READ_ONCE(qs_changing) || q1000k_protocol_status())
		return -ENOLINK;
	ret = qs_tag(skb, &tagged, &vid, &pcp);
	if (ret)
		return ret;
	ret = q1000k_gwan_binding(gem, false, &binding);
	if (ret)
		return ret;
	if (rules)
		for (i = 0; i < rules->count; i++) {
			const struct omci_service_config *s = &rules->rule[i];

			if (s->gem_port_id == gem && s->alloc_id == binding.alloc_id &&
			    s->direction != OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI &&
			    (!s->vlan_valid || (tagged && vid == s->vlan_id)) &&
			    (!s->pcp_valid || (tagged && pcp == s->pcp)) &&
			    READ_ONCE(qs_uni[qs_uni_index(s->uni_entity_id)]))
				return 0;
		}
	return -ENOENT;
}
