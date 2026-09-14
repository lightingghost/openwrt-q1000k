// SPDX-License-Identifier: GPL-2.0-only
/* Complete physical replacement of the legacy data service records. */
#include <linux/errno.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "common/q1000k_gwan.h"
#include "common/q1000k_gem.h"
#include "common/q1000k_tcont.h"
#include "common/q1000k_protocol.h"
#include "common/q1000k_pipeline.h"
#include "common/q1000k_transport.h"

struct q1000k_gwan_transaction {
	struct q1000k_gwan_table old, next;
	struct airoha_pon_qos qos[Q1000K_GWAN_CHANNELS];
	u8 closed[Q1000K_GWAN_CHANNELS];
	u32 channels;
	bool registration;
	int (*install)(void *arg);
	void *install_arg;
};

static bool q1000k_gwan_entry_equal(const struct q1000k_gwan_entry *a,
				   const struct q1000k_gwan_entry *b)
{
	return a->valid == b->valid && (!a->valid ||
		(a->gem == b->gem && a->alloc_id == b->alloc_id &&
		 a->ani == b->ani && a->channel == b->channel &&
		 a->multicast == b->multicast && a->encrypted == b->encrypted));
}

static bool q1000k_gwan_table_equal(const struct q1000k_gwan_table *a,
				   const struct q1000k_gwan_table *b)
{
	unsigned int i;

	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++)
		if (a->alloc_id[i] != b->alloc_id[i])
			return false;
	for (i = 0; i < Q1000K_GWAN_GEMS; i++)
		if (!q1000k_gwan_entry_equal(&a->gem[i], &b->gem[i]))
			return false;
	return true;
}

static int q1000k_gwan_validate(struct q1000k_gwan_transaction *tx)
{
	const struct q1000k_gwan_table *next = &tx->next;
	unsigned int i, j;

	if (!tx->registration && next->alloc_id[0] != tx->old.alloc_id[0])
		return -EPERM;
	tx->channels = BIT(0);
	for (i = 1; i < Q1000K_GWAN_CHANNELS; i++) {
		u16 alloc = next->alloc_id[i];

		if (alloc == Q1000K_GWAN_UNASSIGNED)
			continue;
		if (alloc > Q1000K_ALLOC_ID_MAX || alloc == next->alloc_id[0])
			return -EINVAL;
		for (j = 1; j < i; j++)
			if (alloc == next->alloc_id[j])
				return -EEXIST;
		tx->channels |= BIT(i);
	}
	for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
		const struct q1000k_gwan_entry *e = &next->gem[i];

		/* Channel zero and its OMCC are owned by registration. */
		if (!tx->registration && tx->old.gem[i].valid && tx->old.gem[i].channel == 0 &&
		    !q1000k_gwan_entry_equal(&tx->old.gem[i], e))
			return -EPERM;
		if (!e->valid)
			continue;
		if (!e->channel) {
			if (!tx->registration && !q1000k_gwan_entry_equal(&tx->old.gem[i], e))
				return -EPERM;
			if (e->gem != next->alloc_id[0] || e->gem > Q1000K_GEM_ID_MAX ||
			    e->ani != 0x1ff)
				return -EINVAL;
			continue;
		}
		if (!e->gem || e->gem > Q1000K_GEM_ID_MAX || e->ani > 256)
			return -EINVAL;
		if (e->gem == next->alloc_id[0])
			return -EINVAL;
		/* Multicast receive-channel ownership and encryption key timing
		 * are separate transactions; do not invent a working mapping.
		 */
		if (e->multicast || (e->encrypted &&
		    !q1000k_gwan_entry_equal(&tx->old.gem[i], e)))
			return -EOPNOTSUPP;
		if (e->alloc_id > Q1000K_ALLOC_ID_MAX ||
		    (e->channel >= Q1000K_GWAN_CHANNELS &&
		     e->channel != Q1000K_GWAN_UNKNOWN_CHANNEL))
			return -EINVAL;
		if (e->channel < Q1000K_GWAN_CHANNELS &&
		    next->alloc_id[e->channel] != e->alloc_id)
			return -ESTALE;
		for (j = 0; j < i; j++)
			if (next->gem[j].valid && e->gem == next->gem[j].gem)
				return -EEXIST;
	}
	return 0;
}

static int q1000k_gwan_clear(void *arg)
{
	struct q1000k_gwan_transaction *tx = arg;
	const struct q1000k_gem_value empty = {};
	unsigned int i;
	int ret;

	/* QoS commands require the complete CPU/FE drain established before
	 * this callback. Preserve the real scheduler state for replay.
	 */
	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
		ret = q1000k_transport_get_qos(i, &tx->qos[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
		const struct q1000k_gwan_entry *e = &tx->old.gem[i];
		struct q1000k_gem_value expected = {
			.valid = 1, .multicast = e->multicast, .encrypted = e->encrypted,
		};

		if (!e->valid || (!e->channel && !tx->registration))
			continue;
		ret = q1000k_gem_replace(e->gem, &expected, &empty);
		if (ret)
			return ret;
	}
	return q1000k_tcont_clear_namespace();
}

static int q1000k_gwan_install(void *arg)
{
	struct q1000k_gwan_transaction *tx = arg;
	const struct q1000k_gem_value empty = {};
	unsigned int i;
	int ret;

	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
		if (!(tx->channels & BIT(i)))
			continue;
		if (i) {
			ret = q1000k_tcont_install(i, tx->next.alloc_id[i], tx->next.alloc_id[0]);
			if (ret)
				return ret;
		}
		ret = q1000k_transport_set_qos(i, &tx->qos[i]);
		if (ret)
			return ret;
	}
	if (tx->install) {
		ret = tx->install(tx->install_arg);
		if (ret)
			return ret;
	}
	for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
		const struct q1000k_gwan_entry *e = &tx->next.gem[i];
		struct q1000k_gem_value value = {
			.valid = 1, .multicast = e->multicast, .encrypted = e->encrypted,
		};

		if (!e->valid || (!e->channel && !tx->registration))
			continue;
		ret = q1000k_gem_replace(e->gem, &empty, &value);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct q1000k_pipeline_ops q1000k_gwan_pipeline_ops = {
	.clear = q1000k_gwan_clear,
	.install = q1000k_gwan_install,
};

enum q1000k_gwan_edit { Q1000K_GWAN_APPLY, Q1000K_GWAN_DELETE_GEM,
	Q1000K_GWAN_DELETE_TCONT, Q1000K_GWAN_ADD_TCONT, Q1000K_GWAN_REFRESH, Q1000K_GWAN_REGISTER };

static int q1000k_gwan_rebuild(const struct q1000k_gwan_table *expected,
			      const struct q1000k_gwan_table *desired,
			      enum q1000k_gwan_edit edit, u16 id, bool all,
			      int (*install)(void *), void *install_arg)
{
	struct q1000k_gwan_transaction *tx;
	unsigned int i;
	bool found = all;
	int ret, token;

	/* Enter also supports a PLOAM callback already owning the executor:
	 * no self-cancel/flush and no recursion into an OMCI session barrier.
	 */
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	ret = q1000k_gwan_table_begin();
	if (ret)
		goto leave;
	tx = kzalloc(sizeof(*tx), GFP_KERNEL);
	if (!tx) {
		ret = -ENOMEM;
		goto end;
	}
	tx->registration = edit == Q1000K_GWAN_REGISTER;
	tx->install = install;
	tx->install_arg = install_arg;
	ret = q1000k_gwan_snapshot(&tx->old);
	if (ret)
		goto free;
	if (edit == Q1000K_GWAN_APPLY) {
		if (!q1000k_gwan_table_equal(expected, &tx->old)) {
			ret = -ESTALE;
			goto free;
		}
		tx->next = *desired;
	} else {
		tx->next = tx->old;
		if (edit == Q1000K_GWAN_REGISTER) {
			memset(&tx->next, 0, sizeof(tx->next));
			for (i = 0; i < Q1000K_GWAN_CHANNELS; i++)
				tx->next.alloc_id[i] = Q1000K_GWAN_UNASSIGNED;
			if (id != Q1000K_GWAN_UNASSIGNED) {
				tx->next.alloc_id[0] = id;
				tx->next.gem[0] = (struct q1000k_gwan_entry) {
					.valid = true, .gem = id, .alloc_id = id, .ani = 0x1ff,
				};
			}
			found = true;
		} else if (edit == Q1000K_GWAN_REFRESH) {
			found = true;
		} else if (edit == Q1000K_GWAN_ADD_TCONT) {
			unsigned int channel = 0;

			for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
				if (tx->next.alloc_id[i] == id) {
					ret = -EEXIST;
					goto free;
				}
				if (i && !channel && tx->next.alloc_id[i] == Q1000K_GWAN_UNASSIGNED)
					channel = i;
			}
			if (!channel) {
				ret = -ENOSPC;
				goto free;
			}
			tx->next.alloc_id[channel] = id;
			for (i = 0; i < Q1000K_GWAN_GEMS; i++)
				if (tx->next.gem[i].valid && tx->next.gem[i].alloc_id == id &&
				    tx->next.gem[i].channel == Q1000K_GWAN_UNKNOWN_CHANNEL)
					tx->next.gem[i].channel = channel;
			found = true;
		} else if (edit == Q1000K_GWAN_DELETE_GEM) {
			for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
				struct q1000k_gwan_entry *e = &tx->next.gem[i];

				if (e->valid && e->channel && (all || e->gem == id)) {
					e->valid = false;
					found = true;
				}
			}
		} else {
			for (i = 1; i < Q1000K_GWAN_CHANNELS; i++)
				if (tx->next.alloc_id[i] != Q1000K_GWAN_UNASSIGNED &&
				    (all || tx->next.alloc_id[i] == id)) {
					tx->next.alloc_id[i] = Q1000K_GWAN_UNASSIGNED;
					found = true;
				}
			for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
				struct q1000k_gwan_entry *e = &tx->next.gem[i];

				if (e->valid && e->channel && (all || e->alloc_id == id))
					e->channel = Q1000K_GWAN_UNKNOWN_CHANNEL;
			}
		}
		if (!found) {
			ret = -ENOENT;
			goto free;
		}
	}
	ret = q1000k_gwan_validate(tx);
	if (ret || (!install && !tx->registration && q1000k_gwan_table_equal(&tx->old, &tx->next)))
		goto free;
	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
		ret = q1000k_transport_get_queue_close(i, &tx->closed[i]);
		if (ret)
			goto free;
	}
	/* begin closed binding acquisition. Wait for a TX/RX producer that
	 * already copied an old binding to finish before physical retirement;
	 * otherwise it could submit old metadata with the next native epoch.
	 */
	synchronize_rcu();
	ret = q1000k_pipeline_reconfigure(&q1000k_gwan_pipeline_ops, tx, tx->channels);
	if (ret)
		goto failed;
	/* Receive DMA is still closed. Publish a complete record set before
	 * reactivation; admission stays blocked by the transaction guard.
	 */
	ret = q1000k_protocol_status();
	if (ret)
		goto failed;
	q1000k_gwan_table_publish(&tx->next);
	ret = tx->registration && tx->next.alloc_id[0] == Q1000K_GWAN_UNASSIGNED ?
		q1000k_pipeline_activate_receive_only() : q1000k_pipeline_activate();
	if (ret)
		goto failed;
	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
		if (!(tx->channels & BIT(i)))
			continue;
		/* New/reassigned T-CONTs require explicit provisioning to open. */
		if (tx->registration || tx->old.alloc_id[i] != tx->next.alloc_id[i])
			continue;
		ret = q1000k_transport_set_queue_close(i, tx->closed[i]);
		if (ret)
			goto failed;
	}
	ret = q1000k_protocol_status();
	if (ret)
		goto failed;
	goto free;
failed:
	q1000k_gwan_table_failed(ret);
	q1000k_protocol_fail(ret);
	ret = -EUCLEAN;
free:
	kfree(tx);
end:
	q1000k_gwan_table_end();
leave:
	q1000k_protocol_leave(token);
	return ret;
}

int q1000k_gwan_apply(const struct q1000k_gwan_table *expected,
		      const struct q1000k_gwan_table *desired)
{
	if (!expected || !desired)
		return -EINVAL;
	return q1000k_gwan_rebuild(expected, desired, Q1000K_GWAN_APPLY, 0, false, NULL, NULL);
}

int q1000k_gwan_apply_install(const struct q1000k_gwan_table *expected,
			    const struct q1000k_gwan_table *desired,
			    int (*install)(void *), void *arg)
{
	if (!expected || !desired || !install)
		return -EINVAL;
	return q1000k_gwan_rebuild(expected, desired, Q1000K_GWAN_APPLY, 0, false, install, arg);
}

int q1000k_gwan_delete_gem(u16 gem, bool all)
{
	if (!all && gem > Q1000K_GEM_ID_MAX)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_DELETE_GEM, gem, all, NULL, NULL);
}

int q1000k_gwan_delete_tcont(u16 alloc_id, bool all)
{
	if (!all && alloc_id > Q1000K_ALLOC_ID_MAX)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_DELETE_TCONT, alloc_id, all, NULL, NULL);
}

int q1000k_gwan_add_tcont(u16 alloc_id)
{
	if (alloc_id > Q1000K_ALLOC_ID_MAX)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_ADD_TCONT, alloc_id, false, NULL, NULL);
}

int q1000k_gwan_refresh(int (*install)(void *arg), void *arg)
{
	if (!install)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_REFRESH, 0, true, install, arg);
}

int q1000k_gwan_register(u16 onu_id, int (*install)(void *arg), void *arg)
{
	if ((onu_id >= 1023 && onu_id != Q1000K_GWAN_UNASSIGNED) || !install)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_REGISTER, onu_id, true, install, arg);
}
