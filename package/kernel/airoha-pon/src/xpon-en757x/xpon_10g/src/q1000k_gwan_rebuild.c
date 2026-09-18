// SPDX-License-Identifier: GPL-2.0-only
/* Complete physical replacement of the legacy data service records. */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <q1000k_trace.h>
#include <q1000k_phy_api.h>
#include "common/q1000k_gwan.h"
#include "common/q1000k_mac_cold.h"
#include "common/q1000k_gem.h"
#include "common/q1000k_tcont.h"
#include "common/q1000k_protocol.h"
#include "common/q1000k_pipeline.h"
#include "common/q1000k_transport.h"

/* One image can compare the former full-drain path with each independent
 * live addition and classifier update. Resource rebinding/removal always
 * uses full retirement.
 */
static unsigned int bench_live_add = 15;
module_param(bench_live_add, uint, 0400);
MODULE_PARM_DESC(bench_live_add, "Live operations: bit 0 GEM, bit 1 T-CONT, bit 2 first service, bit 3 classifier on unchanged bindings and queues; 0 full-drain control");

struct q1000k_gwan_transaction {
	struct q1000k_gwan_table old, next;
	struct airoha_pon_qos qos[Q1000K_GWAN_CHANNELS];
	u8 closed[Q1000K_GWAN_CHANNELS];
	u32 channels;
	u32 added_channels, added_gems;
	bool registration, cold, tx_enabled, initial_service, classifier_update;
	int (*install)(void *arg);
	void *install_arg;
};

static bool q1000k_gwan_entry_equal(const struct q1000k_gwan_entry *a,
				   const struct q1000k_gwan_entry *b)
{
	return a->valid == b->valid && (!a->valid ||
		(a->gem == b->gem && a->alloc_id == b->alloc_id &&
		 a->ani == b->ani && a->channel == b->channel &&
		 a->multicast == b->multicast && a->encrypted == b->encrypted &&
		 a->rx_encrypted == b->rx_encrypted));
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
		/* Broadcast key distribution and multicast channel ownership are
		 * not part of the supported unicast key-ring path.
		 */
		if (e->multicast)
			return -EOPNOTSUPP;
		if (e->encrypted && !e->rx_encrypted)
			return -EINVAL; /* G.988 has no upstream-only encryption ring. */
		/* A GEM exists independently of an upstream allocation. A missing
		 * allocation is legal only with the unusable channel sentinel; the
		 * native binding guard keeps RX/TX closed until a service is ready.
		 */
		if ((e->alloc_id > Q1000K_ALLOC_ID_MAX &&
		     !(e->alloc_id == Q1000K_GWAN_UNASSIGNED &&
		       e->channel == Q1000K_GWAN_UNKNOWN_CHANNEL)) ||
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
	if (tx->cold) {
		ret = q1000k_protocol_reset_jobs();
		if (!ret)
			ret = q1000k_mac_cold_release();
		if (!ret)
			ret = q1000k_gem_clear_namespace(Q1000K_GWAN_UNASSIGNED);
		return ret ?: q1000k_tcont_clear_namespace();
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

static unsigned int q1000k_gwan_append_kind(struct q1000k_gwan_transaction *tx)
{
	unsigned int i, kind = 0;

	if (tx->initial_service)
		return 4;
	if (tx->classifier_update)
		return 8;
	if (tx->registration || tx->install || tx->old.alloc_id[0] == Q1000K_GWAN_UNASSIGNED)
		return 0;
	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
		if (tx->old.alloc_id[i] == tx->next.alloc_id[i])
			continue;
		if (!i || tx->old.alloc_id[i] != Q1000K_GWAN_UNASSIGNED ||
		    tx->next.alloc_id[i] == Q1000K_GWAN_UNASSIGNED)
			return 0;
		tx->added_channels |= BIT(i);
		kind |= 2;
	}
	for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
		if (q1000k_gwan_entry_equal(&tx->old.gem[i], &tx->next.gem[i]))
			continue;
		/* Including software-only changes: an old queued frame must not
		 * inherit a new binding, encryption policy or multicast role.
		 */
		if (tx->old.gem[i].valid || !tx->next.gem[i].valid)
			return 0;
		tx->added_gems++;
		kind |= 1;
	}
	return (kind & ~bench_live_add) ? 0 : kind;
}

static int q1000k_gwan_append_install(void *arg)
{
	struct q1000k_gwan_transaction *tx = arg;
	const struct q1000k_gem_value empty = {};
	unsigned int i;
	int ret;
	u8 closed;

	/* This callback only checks queue masks. The service owner publishes
	 * its immutable classifier after this unchanged-namespace transaction.
	 * Neither QoS nor the old packet metadata is reinterpreted.
	 */
	if (tx->classifier_update)
		return tx->install(tx->install_arg);
	if (tx->initial_service) {
		/* Verify every data bank before any QoS command. Channel zero is
		 * the live OMCC and is deliberately excluded from all writes.
		 */
		for (i = 1; i < Q1000K_GWAN_CHANNELS; i++) {
			ret = q1000k_transport_get_queue_close(i, &closed);
			if (ret || closed != 0xff)
				return ret ?: -EBUSY;
		}
		return tx->install(tx->install_arg);
	}
	for (i = 1; i < Q1000K_GWAN_CHANNELS; i++) {
		if (!(tx->added_channels & BIT(i)))
			continue;
		ret = q1000k_transport_get_queue_close(i, &closed);
		if (ret || closed != 0xff)
			return ret ?: -EBUSY;
		ret = q1000k_tcont_install(i, tx->next.alloc_id[i], tx->next.alloc_id[0]);
		if (ret)
			return ret;
	}
	for (i = 0; i < Q1000K_GWAN_GEMS; i++) {
		const struct q1000k_gwan_entry *e = &tx->next.gem[i];
		struct q1000k_gem_value value = {
			.valid = 1, .multicast = e->multicast, .encrypted = e->encrypted,
		};

		if (tx->old.gem[i].valid || !e->valid)
			continue;
		/* Compare invalid -> valid in hardware, then verify readback. */
		ret = q1000k_gem_replace(e->gem, &empty, &value);
		if (ret)
			return ret;
	}
	return 0;
}

enum q1000k_gwan_edit { Q1000K_GWAN_APPLY, Q1000K_GWAN_DELETE_GEM,
	Q1000K_GWAN_DELETE_TCONT, Q1000K_GWAN_ADD_TCONT, Q1000K_GWAN_REFRESH, Q1000K_GWAN_REGISTER,
	Q1000K_GWAN_COLD, Q1000K_GWAN_INITIAL_SERVICE, Q1000K_GWAN_CLASSIFIER };

static int q1000k_gwan_rebuild(const struct q1000k_gwan_table *expected,
			      const struct q1000k_gwan_table *desired,
			      enum q1000k_gwan_edit edit, u16 id, bool all,
			      int (*install)(void *), void *install_arg, int (*ready)(void *))
{
	struct q1000k_gwan_transaction *tx;
	struct q1000k_pipeline_ops ops = q1000k_gwan_pipeline_ops;
	unsigned int i, append;
	bool found = all;
	int ret, token;

	if (bench_live_add & ~15U)
		return -EINVAL;
	if (edit == Q1000K_GWAN_INITIAL_SERVICE && !(bench_live_add & 4))
		return -EAGAIN;
	if (edit == Q1000K_GWAN_CLASSIFIER && !(bench_live_add & 8))
		return -EAGAIN;
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
	tx->cold = edit == Q1000K_GWAN_COLD;
	tx->registration = edit == Q1000K_GWAN_REGISTER || tx->cold;
	ops.reset_mac = tx->cold;
	tx->install = install;
	tx->install_arg = install_arg;
	tx->initial_service = edit == Q1000K_GWAN_INITIAL_SERVICE;
	tx->classifier_update = edit == Q1000K_GWAN_CLASSIFIER;
	ret = q1000k_gwan_snapshot(&tx->old);
	if (ret)
		goto free;
	if (edit == Q1000K_GWAN_APPLY || tx->initial_service || tx->classifier_update) {
		if (!q1000k_gwan_table_equal(expected, &tx->old)) {
			ret = -ESTALE;
			goto free;
		}
		tx->next = *desired;
	} else {
		tx->next = tx->old;
		if (tx->registration) {
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
	if ((tx->initial_service || tx->classifier_update) &&
	    (!q1000k_gwan_table_equal(&tx->old, &tx->next) ||
	     tx->old.alloc_id[0] == Q1000K_GWAN_UNASSIGNED)) {
		ret = -EAGAIN; /* No mutation; caller uses the full transaction. */
		goto free;
	}
	append = q1000k_gwan_append_kind(tx);
	if (append) {
		/* Protocol producers are serialized and data binding acquisition
		 * is closed. Existing entries and OMCC keep their native epochs;
		 * no pending authenticated reply or existing DMA owner is retired.
		 */
		q1000k_trace(QT_GWAN_APPEND, 1, 0, append, tx->added_channels,
			     tx->added_gems, tx->channels);
		q1000k_activation_snapshot(20, append);
		ret = q1000k_pipeline_append(q1000k_gwan_append_install, tx, tx->channels);
		q1000k_activation_snapshot(21, append);
		if (!ret)
			ret = q1000k_protocol_status();
		q1000k_trace(QT_GWAN_APPEND, 2, ret, append, tx->added_channels,
			     tx->added_gems, tx->channels);
		if (ret)
			goto failed;
		q1000k_gwan_table_publish(&tx->next);
		goto free;
	}
	/* Preserve discovery as well as operational TX across profile/QoS
	 * refreshes. ONU assignment alone cannot describe the O2/3 TX state.
	 * Bootstrap and ONU removal always resume with the transmitter off.
	 */
	if (!tx->cold && !(tx->registration && id == Q1000K_GWAN_UNASSIGNED)) {
		ret = q1000k_phy_get_tx(&tx->tx_enabled);
		if (ret)
			goto failed;
	}
	for (i = 0; i < Q1000K_GWAN_CHANNELS; i++) {
		ret = q1000k_transport_get_queue_close(i, &tx->closed[i]);
		if (ret)
			goto free;
	}
	/* begin closed binding acquisition. Wait for a TX/RX producer that
	 * already copied an old binding to finish before physical retirement;
	 * otherwise it could submit old metadata with the next native epoch.
	 */
	q1000k_trace(QT_CONTROL, 4, 0, tx->registration, tx->cold, tx->channels, 0);
	synchronize_rcu();
	q1000k_trace(QT_CONTROL, 5, 0, tx->registration, tx->cold, tx->channels, 0);
	q1000k_activation_snapshot(22, edit);
	ret = q1000k_pipeline_reconfigure(&ops, tx, tx->channels);
	if (ret)
		goto failed;
	/* Receive DMA is still closed. Publish a complete record set before
	 * reactivation; admission stays blocked by the transaction guard.
	 */
	ret = q1000k_protocol_status();
	if (ret)
		goto failed;
	q1000k_gwan_table_publish(&tx->next);
	ret = ready ? q1000k_pipeline_activate_checked(tx->tx_enabled, ready, install_arg) :
		tx->tx_enabled ? q1000k_pipeline_activate() : q1000k_pipeline_activate_receive_only();
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
	q1000k_activation_snapshot(23, edit);
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
	return q1000k_gwan_rebuild(expected, desired, Q1000K_GWAN_APPLY, 0, false, NULL, NULL, NULL);
}

int q1000k_gwan_apply_install(const struct q1000k_gwan_table *expected,
			    const struct q1000k_gwan_table *desired,
			    int (*install)(void *), void *arg)
{
	if (!expected || !desired || !install)
		return -EINVAL;
	return q1000k_gwan_rebuild(expected, desired, Q1000K_GWAN_APPLY, 0, false, install, arg, NULL);
}

int q1000k_gwan_initial_service(const struct q1000k_gwan_table *expected,
			       const struct q1000k_gwan_table *desired,
			       int (*install)(void *), void *arg)
{
	if (!expected || !desired || !install)
		return -EINVAL;
	return q1000k_gwan_rebuild(expected, desired, Q1000K_GWAN_INITIAL_SERVICE,
				   0, false, install, arg, NULL);
}

int q1000k_gwan_classifier(const struct q1000k_gwan_table *expected,
			  const struct q1000k_gwan_table *desired,
			  int (*check)(void *), void *arg)
{
	if (!expected || !desired || !check)
		return -EINVAL;
	return q1000k_gwan_rebuild(expected, desired, Q1000K_GWAN_CLASSIFIER,
				   0, false, check, arg, NULL);
}

int q1000k_gwan_delete_gem(u16 gem, bool all)
{
	if (!all && gem > Q1000K_GEM_ID_MAX)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_DELETE_GEM, gem, all, NULL, NULL, NULL);
}

int q1000k_gwan_delete_tcont(u16 alloc_id, bool all)
{
	if (!all && alloc_id > Q1000K_ALLOC_ID_MAX)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_DELETE_TCONT, alloc_id, all, NULL, NULL, NULL);
}

int q1000k_gwan_add_tcont(u16 alloc_id)
{
	if (alloc_id > Q1000K_ALLOC_ID_MAX)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_ADD_TCONT, alloc_id, false, NULL, NULL, NULL);
}

int q1000k_gwan_refresh(int (*install)(void *arg), void *arg)
{
	if (!install)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_REFRESH, 0, true, install, arg, NULL);
}

int q1000k_gwan_register(u16 onu_id, int (*install)(void *arg), void *arg)
{
	if ((onu_id > 1020 && onu_id != Q1000K_GWAN_UNASSIGNED) || !install)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_REGISTER, onu_id, true, install, arg, NULL);
}

int q1000k_gwan_cold_reset(int (*install)(void *arg), void *arg)
{
	if (!install)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_COLD,
		Q1000K_GWAN_UNASSIGNED, true, install, arg, NULL);
}

int q1000k_gwan_refresh_checked(int (*install)(void *), int (*ready)(void *), void *arg)
{
	if (!install || !ready)
		return -EINVAL;
	return q1000k_gwan_rebuild(NULL, NULL, Q1000K_GWAN_REFRESH, 0, true, install, arg, ready);
}
