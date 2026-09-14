// SPDX-License-Identifier: GPL-2.0-only
/* OMCI entities and UNI-side service selection for the native PON datapath. */
#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include "common/q1000k_services.h"
#include "common/q1000k_vlan.h"
#include "common/q1000k_gwan.h"
#include "common/q1000k_protocol.h"
#include "common/q1000k_transport.h"

#define QS_TCONT_BASE 0x8000
#define QS_TCONTS 31
#define QS_MAX 256
struct qs_gem { u16 entity, gem, tcont; u8 direction, key_ring; bool valid, seeded; };
struct qs_rules { size_t count; u8 channels[QS_MAX]; u16 vlan_group[QS_MAX];
	struct q1000k_vlan_program vlan[QS_MAX]; struct omci_service_config rule[]; };
struct qs_scheduler { u8 policy, weight[8]; };
static u16 qs_alloc[QS_TCONTS];
static bool qs_seeded_alloc[QS_TCONTS];
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
	memset(qs_seeded_alloc, 0, sizeof(qs_seeded_alloc));
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
	memset(qs_seeded_alloc, 0, sizeof(qs_seeded_alloc));
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
	if (qs_alloc[index] == alloc) {
		qs_seeded_alloc[index] = false;
		goto leave;
	}
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
	if (!ret) {
		qs_alloc[index] = alloc;
		qs_seeded_alloc[index] = false;
	}
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
		       u8 direction, bool valid, u8 key_ring)
{
	struct q1000k_gwan_table *tables;
	int token, ret, slot = -1, record = -1;
	unsigned int i;
	bool was_changing;
	u16 alloc = 0xffff;

	if (valid && (!gem || direction < 1 || direction > 3 ||
		      tcont < QS_TCONT_BASE || tcont >= QS_TCONT_BASE + QS_TCONTS))
		return -EINVAL;
	if (key_ring > 3) return -EINVAL;
	if (key_ring == 2) return -EOPNOTSUPP;
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
			.encrypted = key_ring == 1, .rx_encrypted = key_ring != 0,
		};
	}
	was_changing = READ_ONCE(qs_changing);
	WRITE_ONCE(qs_changing, true);
	ret = q1000k_gwan_apply(&tables[0], &tables[1]);
	if (!ret) {
		qs_gems[slot] = (struct qs_gem) {
			.valid = valid, .entity = entity, .gem = gem, .tcont = tcont, .direction = direction,
			.key_ring = key_ring,
		};
		if (valid)
			qs_seeded_alloc[tcont - QS_TCONT_BASE] = false;
	}
	if (ret && ret != -EUCLEAN)
		WRITE_ONCE(qs_changing, was_changing);
free:
	kfree(tables);
leave:
	q1000k_protocol_leave(token);
	return ret;
}

int q1000k_services_gem_key_ring(u16 entity, u8 *key_ring)
{
	unsigned int i;

	if (!q1000k_protocol_owned()) return -EPERM;
	if (!key_ring) return -EINVAL;
	for (i = 0; i < QS_MAX; i++)
		if (qs_gems[i].valid && qs_gems[i].entity == entity) {
			*key_ring = qs_gems[i].key_ring;
			return 0;
		}
	return -ENOENT;
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

		if (channel >= 32 || installed[channel])
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

struct qs_replacement {
	struct q1000k_gwan_table before, after;
	u16 alloc[QS_TCONTS];
	bool seeded_alloc[QS_TCONTS];
	struct qs_gem gems[QS_MAX];
};

static int qs_prepare_replacement(struct qs_replacement *p)
{
	unsigned int i;
	int ret = q1000k_gwan_snapshot(&p->before);

	if (ret)
		return ret;
	p->after = p->before;
	memcpy(p->alloc, qs_alloc, sizeof(p->alloc));
	memcpy(p->seeded_alloc, qs_seeded_alloc, sizeof(p->seeded_alloc));
	memcpy(p->gems, qs_gems, sizeof(p->gems));
	/* Profile-created resources follow the complete normalized service set.
	 * Explicit OMCI-created GEMs remain until their own Delete transaction.
	 */
	for (i = 0; i < QS_MAX; i++) {
		int record;

		if (!p->gems[i].valid || !p->gems[i].seeded)
			continue;
		record = qs_record(&p->after, p->gems[i].gem);
		if (record < 0 || !p->after.gem[record].channel)
			return -ESTALE;
		p->after.gem[record].valid = false;
		p->gems[i].valid = false;
	}
	for (i = 0; i < QS_TCONTS; i++)
		if (p->seeded_alloc[i]) {
			p->alloc[i] = 0xffff;
			p->seeded_alloc[i] = false;
		}
	return 0;
}

static int qs_prepare_service(struct qs_replacement *p,
			      const struct omci_service_config *s, u8 *channel)
{
	unsigned int i, index = s->tcont_entity_id - QS_TCONT_BASE;
	int slot = -1, record;
	struct qs_gem *gem;

	if (s->encryption_key_ring > 3) return -EINVAL;
	if (s->encryption_key_ring == 2) return -EOPNOTSUPP;
	if (!s->gem_port_id || s->gem_port_id == 0xffff || s->alloc_id > 0x3fff ||
	    s->gem_port_id == p->after.alloc_id[0] || s->alloc_id == p->after.alloc_id[0])
		return -EINVAL;
	if (p->alloc[index] != 0xffff && p->alloc[index] != s->alloc_id)
		return -ESTALE;
	for (i = 0; i < QS_TCONTS; i++)
		if (i != index && p->alloc[i] == s->alloc_id)
			return -EEXIST;
	if (p->alloc[index] == 0xffff) {
		p->alloc[index] = s->alloc_id;
		p->seeded_alloc[index] = true;
	}
	for (i = 0; i < QS_MAX; i++) {
		if (p->gems[i].valid && p->gems[i].entity == s->gem_ctp_entity_id) {
			slot = i;
			break;
		}
		if (!p->gems[i].valid && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -ENOSPC;
	gem = &p->gems[slot];
	if (gem->valid) {
		if (gem->gem != s->gem_port_id || gem->tcont != s->tcont_entity_id ||
		    gem->direction != s->direction || gem->key_ring != s->encryption_key_ring)
			return -ESTALE;
	} else {
		for (i = 0; i < QS_MAX; i++)
			if (p->gems[i].valid && p->gems[i].gem == s->gem_port_id)
				return -EEXIST;
		*gem = (struct qs_gem) {
			.entity = s->gem_ctp_entity_id, .gem = s->gem_port_id,
			.tcont = s->tcont_entity_id, .direction = s->direction,
			.key_ring = s->encryption_key_ring,
			.valid = true, .seeded = true,
		};
	}
	*channel = qs_channel(&p->after, s->alloc_id);
	record = qs_record(&p->after, s->gem_port_id);
	if (record >= 0) {
		const struct q1000k_gwan_entry *e = &p->after.gem[record];

		if (!e->channel || e->alloc_id != s->alloc_id || e->channel != *channel ||
		    e->multicast || e->encrypted != (s->encryption_key_ring == 1) ||
		    e->rx_encrypted != (s->encryption_key_ring != 0) || e->ani != 1)
			return -ESTALE;
		return 0;
	}
	if (!gem->seeded)
		return -ESTALE;
	for (i = 0; i < QS_MAX; i++)
		if (!p->after.gem[i].valid) {
			p->after.gem[i] = (struct q1000k_gwan_entry) {
				.valid = true, .gem = s->gem_port_id, .alloc_id = s->alloc_id,
				.ani = 1, .channel = *channel,
				.encrypted = s->encryption_key_ring == 1,
				.rx_encrypted = s->encryption_key_ring != 0,
			};
			return 0;
		}
	return -ENOSPC;
}

int q1000k_services_replace(struct omci_device *odev,
		const struct omci_service_config *services, size_t count)
{
	struct qs_replacement *replacement;
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
	replacement = kzalloc(sizeof(*replacement), GFP_KERNEL);
	if (!replacement) { kfree(next); return -ENOMEM; }
	next->count = count;
	if (count)
		memcpy(next->rule, services, count * sizeof(*services));
	token = q1000k_protocol_enter();
	if (token < 0) { kfree(next); kfree(replacement); return token; }
	was_changing = READ_ONCE(qs_changing);
	ret = qs_prepare_replacement(replacement);
	if (ret)
		goto free;
	memset(closed, 255, sizeof(closed));
	for (i = 0; i < count; i++) {
		const struct omci_service_config *s = &services[i];
		int uni = qs_uni_index(s->uni_entity_id);

		if (uni < 0 || s->queue > 7 || (s->pcp_valid && s->pcp > 7) ||
		    s->tcont_entity_id < QS_TCONT_BASE || s->tcont_entity_id >= QS_TCONT_BASE + QS_TCONTS ||
		    (s->vlan_valid && s->vlan_id > 4094) || s->direction < 1 || s->direction > 3) {
			ret = -EINVAL; goto free;
		}
		if (s->default_service && (s->vlan_valid || s->pcp_valid || s->vlan_treatment_valid ||
		    s->vlan_filter[0].valid || s->vlan_filter[1].valid)) { ret = -EINVAL; goto free; }
		if (s->multicast || s->multicast_ani_valid) {
			ret = -EOPNOTSUPP; goto free;
		}
		next->vlan_group[i] = i;
		for (j = 0; j < 2; j++) {
			ret = q1000k_vlan_filter_validate(&s->vlan_filter[j]);
			if (ret) goto free;
		}
		if (s->vlan_treatment_valid) {
			if (s->vlan_valid) { ret = -EINVAL; goto free; }
			ret = q1000k_vlan_compile(s, &next->vlan[i]);
			if (ret) goto free;
		}
		ret = qs_prepare_service(replacement, s, &next->channels[i]);
		if (ret)
			goto free;
		for (j = 0; j < i; j++) {
			const struct omci_service_config *p = &services[j];

			if (s->vlan_treatment_valid && p->vlan_treatment_valid &&
			    s->vlan_entity_id == p->vlan_entity_id &&
			    s->uni_entity_id == p->uni_entity_id && s->vlan_ani_side == p->vlan_ani_side) {
				if (s->vlan_input_tpid != p->vlan_input_tpid ||
				    s->vlan_output_tpid != p->vlan_output_tpid ||
				    s->vlan_downstream_mode != p->vlan_downstream_mode ||
				    (!memcmp(s->vlan_rule.raw, p->vlan_rule.raw, 8) &&
				     memcmp(&s->vlan_rule, &p->vlan_rule, sizeof(s->vlan_rule)))) {
					ret = -EINVAL; goto free;
				}
				next->vlan_group[i] = next->vlan_group[j];
			}
			/* Equal specificity with an overlapping match is ambiguous on
			 * this single ANI netdevice, including across logical UNIs.
			 */
			if (s->cookie == p->cookie ||
			    (s->default_service && p->default_service) ||
			    (s->vlan_treatment_valid == p->vlan_treatment_valid &&
			     !memcmp(s->vlan_filter, p->vlan_filter, sizeof(s->vlan_filter)) &&
			     (!s->vlan_treatment_valid || !memcmp(s->vlan_rule.raw, p->vlan_rule.raw, 8)) &&
			     s->vlan_valid == p->vlan_valid && s->pcp_valid == p->pcp_valid &&
			     (!s->vlan_valid || s->vlan_id == p->vlan_id) &&
			     (!s->pcp_valid || s->pcp == p->pcp))) {
				ret = -EEXIST; goto free;
			}
		}
		if (next->channels[i] < 32 && s->direction != OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)
			closed[next->channels[i]] &= ~BIT(s->queue);
	}
	WRITE_ONCE(qs_changing, true);
	/* Retire old classifier-selected traffic, including native retries and
	 * downstream frames, even when the GEM record set itself is identical.
	 */
	ret = q1000k_gwan_apply_install(&replacement->before, &replacement->after,
				       qs_service_qos_install, next);
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
	memcpy(qs_alloc, replacement->alloc, sizeof(qs_alloc));
	memcpy(qs_seeded_alloc, replacement->seeded_alloc, sizeof(qs_seeded_alloc));
	memcpy(qs_gems, replacement->gems, sizeof(qs_gems));
	old = rcu_replace_pointer(qs_current, next, q1000k_protocol_owned());
	next = NULL;
	synchronize_rcu();
	kfree(old);
	WRITE_ONCE(qs_changing, false);
free:
	kfree(replacement);
	kfree(next);
	q1000k_protocol_leave(token);
	return ret;
}

/* Decode the complete L2 header, including an offloaded outer VLAN tag. */
static int qs_frame(const struct sk_buff *skb, struct q1000k_vlan_frame *frame)
{
	u8 bytes[14];
	unsigned int offset = 14;
	u16 type;

	memset(frame, 0, sizeof(*frame));
	if (skb_copy_bits(skb, 0, bytes, 14)) return -EMSGSIZE;
	type = get_unaligned_be16(bytes + 12);
	if (skb_vlan_tag_present(skb)) {
		frame->tag[0].tpid = ntohs(skb->vlan_proto);
		frame->tag[0].tci = skb_vlan_tag_get(skb);
		frame->count = 1;
	}
	while (type == 0x8100 || type == 0x88a8 || type == 0x9100) {
		if (frame->count == 2) return -EOPNOTSUPP;
		if (skb_copy_bits(skb, offset, bytes, 4)) return -EMSGSIZE;
		frame->tag[frame->count].tpid = type;
		frame->tag[frame->count++].tci = get_unaligned_be16(bytes);
		type = get_unaligned_be16(bytes + 2);
		offset += 4;
	}
	frame->ethertype = type;
	return 0;
}

static int qs_rewrite(struct sk_buff *skb, const struct q1000k_vlan_frame *input,
		       const struct q1000k_vlan_frame *output)
{
	unsigned int i, inline_tags = input->count - !!skb_vlan_tag_present(skb);
	u16 ignored;
	int ret;

	if (skb_shared(skb) || skb_is_gso(skb)) return -EOPNOTSUPP;
	if (skb_linearize(skb)) return -ENOMEM;
	if (skb->ip_summed == CHECKSUM_PARTIAL && skb_checksum_help(skb)) return -EINVAL;
	skb->ip_summed = CHECKSUM_NONE;
	skb_reset_mac_header(skb);
	__vlan_hwaccel_clear_tag(skb);
	for (i = 0; i < inline_tags; i++) {
		ret = __skb_vlan_pop(skb, &ignored);
		if (ret) return ret;
	}
	for (i = output->count; i; i--) {
		ret = __vlan_insert_tag(skb, htons(output->tag[i - 1].tpid), output->tag[i - 1].tci);
		if (ret) return ret;
	}
	skb->protocol = htons(output->count ? output->tag[0].tpid : output->ethertype);
	return 0;
}

static bool qs_precedes(const struct omci_service_config *s,
			const struct omci_service_config *selected)
{
	return s->vlan_treatment_valid && selected && selected->vlan_treatment_valid &&
		memcmp(s->vlan_rule.raw, selected->vlan_rule.raw, 8) < 0;
}

/* Select each class 171 table's first matching row BEFORE mapper/bridge
 * filtering. Otherwise a rejected treatment can fall through to a later
 * wildcard row. Group indices are precomputed during replacement; both
 * packet passes are linear in the bounded service count.
 */
static void qs_vlan_winners(const struct qs_rules *rules, bool upstream,
		const struct q1000k_vlan_frame *input, u16 winners[QS_MAX])
{
	struct q1000k_vlan_frame output;
	size_t i;

	memset(winners, 0xff, QS_MAX * sizeof(*winners));
	for (i = 0; i < rules->count; i++) {
		const struct omci_service_config *s = &rules->rule[i];
		u16 group = rules->vlan_group[i], old = winners[group];
		int ret;

		if (!s->vlan_treatment_valid) continue;
		ret = q1000k_vlan_apply(&rules->vlan[i], upstream, input, &output);
		if (ret && ret != -EPERM) continue;
		if (old == 0xffff ||
		    (rules->vlan[old].fallback && !rules->vlan[i].fallback) ||
		    (rules->vlan[old].fallback == rules->vlan[i].fallback &&
		     qs_precedes(s, &rules->rule[old]))) winners[group] = i;
	}
}

static int qs_service_frame(const struct qs_rules *rules, size_t i, bool upstream,
		const struct q1000k_vlan_frame *input, struct q1000k_vlan_frame *output,
		const u16 winners[QS_MAX])
{
	const struct omci_service_config *s = &rules->rule[i];
	const struct q1000k_vlan_frame *bridge = input;
	bool transform_first = s->vlan_treatment_valid && (upstream != s->vlan_ani_side);
	int ret;

	if (s->vlan_treatment_valid) {
		u16 first = winners[rules->vlan_group[i]];

		if (first == 0xffff || memcmp(s->vlan_rule.raw, rules->rule[first].vlan_rule.raw, 8))
			return -ENOENT;
	}
	*output = *input;
	if (transform_first) {
		ret = q1000k_vlan_apply(&rules->vlan[i], upstream, input, output);
		if (ret) return ret;
		bridge = output;
	}
	/* UNI ingress / ANI egress upstream; ANI ingress / UNI egress downstream. */
	ret = q1000k_vlan_filter_apply(&s->vlan_filter[0], upstream, bridge);
	if (ret) return ret;
	ret = q1000k_vlan_filter_apply(&s->vlan_filter[1], !upstream, bridge);
	if (ret) return ret;
	if (s->pcp_valid && (!bridge->count ||
	    s->pcp != bridge->tag[bridge->count - 1].tci >> 13)) return -ENOENT;
	if (s->vlan_valid && (!bridge->count ||
	    s->vlan_id != (bridge->tag[0].tci & VLAN_VID_MASK))) return -ENOENT;
	if (s->vlan_treatment_valid && !transform_first)
		return q1000k_vlan_apply(&rules->vlan[i], upstream, input, output);
	return 0;
}

int q1000k_services_tx(struct sk_buff *skb)
{
	const struct omci_service_config *selected = NULL;
	struct qs_rules *rules = rcu_dereference(qs_current);
	struct q1000k_gwan_binding binding;
	struct q1000k_vlan_frame input, output, selected_output = {};
	int score = -1, ret, selected_result = 0;
	unsigned int bytes;
	size_t i;
	bool ambiguous = false;
	u16 winners[QS_MAX];

	if (!READ_ONCE(qs_enabled) || READ_ONCE(qs_changing) || q1000k_protocol_status())
		return -ENOLINK;
	if (!rules)
		return -ENODATA;
	ret = qs_frame(skb, &input);
	if (ret) return ret;
	qs_vlan_winners(rules, true, &input, winners);
	for (i = 0; i < rules->count; i++) {
		const struct omci_service_config *s = &rules->rule[i];
		int rank = (s->vlan_valid ? 2 : 0) + s->pcp_valid;

		if (!READ_ONCE(qs_uni[qs_uni_index(s->uni_entity_id)]) ||
		    s->direction == OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)
			continue;
		ret = qs_service_frame(rules, i, true, &input, &output, winners);
		if (ret && ret != -EPERM) continue;
		if (s->vlan_treatment_valid) rank = rules->vlan[i].fallback ? 4 : 8;
		else if (s->default_service) rank = 0;
		if (rank > score || (rank == score && qs_precedes(s, selected))) {
			score = rank; selected = s; selected_result = ret; ambiguous = false;
			if (!ret) selected_output = output;
		} else if (rank == score && !qs_precedes(selected, s) && !ret && !selected_result &&
			   (s->gem_port_id != selected->gem_port_id || s->queue != selected->queue ||
			    s->uni_entity_id != selected->uni_entity_id ||
			    memcmp(&output, &selected_output, sizeof(output)))) {
			ambiguous = true;
		}
	}
	if (!selected) return -ENOENT;
	if (selected_result) return selected_result;
	if (ambiguous) return -EEXIST;
	if (selected->vlan_treatment_valid) {
		ret = qs_rewrite(skb, &input, &selected_output);
		if (ret) return ret;
	}
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
	bytes = skb->len;
	ret = q1000k_transport_xmit(skb, ((u32)binding.gem << 14) |
		((u32)binding.channel << 3) | selected->queue,
		0x7f2007ff | ((u32)binding.channel << 15));
	if (!ret)
		q1000k_gwan_account(binding.gem, true, bytes);
	return ret;
}

int q1000k_services_rx(struct sk_buff *skb, u16 gem)
{
	struct qs_rules *rules = rcu_dereference(qs_current);
	const struct omci_service_config *selected = NULL;
	struct q1000k_gwan_binding binding;
	struct q1000k_vlan_frame input, output, selected_output = {};
	unsigned int bytes = skb->len;
	size_t i;
	bool ambiguous = false;
	u16 winners[QS_MAX];
	int ret;

	if (!READ_ONCE(qs_enabled) || READ_ONCE(qs_changing) || q1000k_protocol_status())
		return -ENOLINK;
	ret = qs_frame(skb, &input);
	if (ret) return ret;
	ret = q1000k_gwan_binding(gem, false, &binding);
	if (ret) return ret;
	if (rules) qs_vlan_winners(rules, false, &input, winners);
	if (rules)
		for (i = 0; i < rules->count; i++) {
			const struct omci_service_config *s = &rules->rule[i];

			if (s->gem_port_id != gem || s->alloc_id != binding.alloc_id ||
			    s->direction == OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI ||
			    !READ_ONCE(qs_uni[qs_uni_index(s->uni_entity_id)])) continue;
			ret = qs_service_frame(rules, i, false, &input, &output, winners);
			if (ret) continue;
			if (!selected || qs_precedes(s, selected)) {
				selected = s; selected_output = output; ambiguous = false;
			} else if (!qs_precedes(selected, s) &&
				   (s->uni_entity_id != selected->uni_entity_id ||
				    memcmp(&output, &selected_output, sizeof(output)))) {
				ambiguous = true;
			}
		}
	if (!selected) return -ENOENT;
	if (ambiguous) return -EEXIST;
	if (selected->vlan_treatment_valid) {
		ret = qs_rewrite(skb, &input, &selected_output);
		if (ret) return ret;
	}
	q1000k_gwan_account(gem, false, bytes);
	return 0;
}
