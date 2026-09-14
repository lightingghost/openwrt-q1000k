// SPDX-License-Identifier: GPL-2.0-only
/* Appended to a scratch copy of the actual OMCI agent in a disposable UML guest. */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/utsname.h>
#include "../internal.h"
#ifndef CONFIG_UML
#error This fixture must never run on a physical device.
#endif

static unsigned int fixture_tx, fixture_stops, fixture_batch_calls;
static int fixture_batch_error, fixture_start_error;
static u8 fixture_key_ring;
static int fixture_gem_mode, fixture_gem_read_error;
static int fixture_gem_error = -EOPNOTSUPP, fixture_gem_calls;
static int fixture_undo_error, fixture_undo_call, fixture_tcont_calls;
static unsigned int fixture_faults;
static size_t fixture_service_count;
static u64 fixture_auth_epoch;
static int fixture_rekey_result;
static DECLARE_COMPLETION(fixture_rekeyed);
static bool fixture_hold_tx;
static DECLARE_COMPLETION(fixture_tx_entered);
static DECLARE_COMPLETION(fixture_tx_release);
static DECLARE_COMPLETION(fixture_tx_done);
static int fixture_tx_result;
static int fixture_rekey_thread(void *arg)
{
	fixture_rekey_result = omci_device_set_auth_epoch(arg, 0);
	kthread_complete_and_exit(&fixture_rekeyed, 0);
}
static DECLARE_COMPLETION(fixture_stopped);

static struct sk_buff *fixture_packet(unsigned int id, bool fragmented)
{
	u8 pdu[48] = {};
	struct sk_buff *skb;

	put_unaligned_be16(id, pdu);
	pdu[2] = 0x49; /* Get, acknowledgement requested. */
	pdu[3] = OMCI_BASELINE_DEV_ID;
	put_unaligned_be16(OMCI_CLASS_ONU_G, pdu + 4);
	put_unaligned_be16(0x8000, pdu + 8);
	put_unaligned_be32(40, pdu + 40);
	skb = alloc_skb(sizeof(pdu), GFP_KERNEL);
	if (!skb)
		return NULL;
	if (fragmented) {
		struct page *page = alloc_page(GFP_KERNEL);

		if (!page) {
			kfree_skb(skb);
			return NULL;
		}
		skb_put_data(skb, pdu, 4);
		memcpy(page_address(page), pdu + 4, sizeof(pdu) - 4);
		skb_add_rx_frag(skb, 0, page, 0, sizeof(pdu) - 4, PAGE_SIZE);
	} else {
		skb_put_data(skb, pdu, sizeof(pdu));
	}
	return skb;
}

static int fixture_start(struct omci_device *odev)
{
	omci_device_set_channel(odev, 7, true);
	if (omci_device_set_auth_epoch(odev, ++fixture_auth_epoch))
		return -EIO;
	/* Startup RX must not touch the MIB even if startup subsequently fails. */
	omci_device_receive(odev, fixture_packet(1, false), 7, OMCI_F_MIC_VALID, fixture_auth_epoch);
	return fixture_start_error;
}

static void fixture_stop(struct omci_device *odev)
{
	fixture_stops++;
}

static int fixture_xmit(struct omci_device *odev, struct sk_buff *skb, u16 gem, u64 auth_epoch)
{
	if (gem != 7 || skb->len != 44 || skb_is_nonlinear(skb) ||
	    !auth_epoch || auth_epoch != fixture_auth_epoch)
		return -EINVAL;
	if (fixture_hold_tx) {
		complete(&fixture_tx_entered);
		wait_for_completion(&fixture_tx_release);
	}
	fixture_tx++;
	kfree_skb(skb);
	return 0;
}

static int fixture_tx_thread(void *arg)
{
	struct sk_buff *skb = fixture_packet(6000, false);

	fixture_tx_result = skb ? omci_device_xmit(arg, skb->data, skb->len) : -ENOMEM;
	kfree_skb(skb);
	kthread_complete_and_exit(&fixture_tx_done, 0);
}

static int fixture_topology(struct omci_device *odev,
			    struct omci_ani_topology *topology)
{
	*topology = (struct omci_ani_topology) {
		.tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 32, .queues_per_tcont = 8,
		.maximum_queue_size = 16, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1,
	};
	return 0;
}

static int fixture_tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid)
{
	return ++fixture_tcont_calls == fixture_undo_call ? fixture_undo_error : fixture_gem_error;
}

static int fixture_gem(struct omci_device *odev, u16 entity, u16 gem, u16 tcont,
		       u8 direction, bool valid, u8 key_ring)
{
	fixture_key_ring = key_ring;
	return ++fixture_gem_calls == fixture_undo_call ? fixture_undo_error : fixture_gem_error;
}

static int fixture_gem_encryption(struct omci_device *odev, u16 entity, u8 *mode)
{
	if (fixture_gem_read_error) return fixture_gem_read_error;
	*mode = fixture_gem_mode;
	return 0;
}

static int fixture_uni(struct omci_device *odev, u16 entity, bool enable)
{
	return -EOPNOTSUPP;
}

static int fixture_batch(struct omci_device *odev,
			 const struct omci_service_config *services, size_t count)
{
	fixture_batch_calls++;
	if ((!services && count) || (services && !count))
		return -EINVAL;
	if (fixture_batch_error)
		return fixture_batch_error;
	fixture_service_count = count;
	return 0;
}

static int fixture_scheduler_error;
static struct omci_priority_queue_config fixture_queue_value;
static struct omci_traffic_scheduler_config fixture_scheduler_value;
static int fixture_queue(struct omci_device *odev, u16 entity,
                         const struct omci_priority_queue_config *q)
{
	fixture_queue_value = *q;
	return fixture_scheduler_error;
}
static int fixture_scheduler(struct omci_device *odev, u16 entity,
                             const struct omci_traffic_scheduler_config *s)
{
	fixture_scheduler_value = *s;
	return fixture_scheduler_error;
}

static void fixture_service_fault(struct omci_device *odev, int error)
{
	WARN_ON(error != -EUCLEAN || odev->agent.operational);
	fixture_faults++;
}

static const struct omci_device_ops fixture_ops = {
	.start = fixture_start, .stop = fixture_stop, .xmit = fixture_xmit,
	.get_ani_topology = fixture_topology, .set_tcont = fixture_tcont,
	.set_gem_port = fixture_gem, .get_gem_encryption = fixture_gem_encryption, .set_uni = fixture_uni,
	.replace_services = fixture_batch,
	.set_priority_queue = fixture_queue, .set_traffic_scheduler = fixture_scheduler,
	.service_fault = fixture_service_fault,
};

static int fixture_stop_thread(void *arg)
{
	omci_device_stop(arg);
	kthread_complete_and_exit(&fixture_stopped, 0);
}

#define CHECK(condition) do { if (!(condition)) { \
	pr_err("Q1000K_OMCI_CORE_FAIL line=%u: %s\n", __LINE__, #condition); \
	ret = -EINVAL; goto out; } } while (0)

int q1000k_omci_core_test(void)
{
	struct device *parent = NULL;
	struct net_device *netdev = NULL;
	struct omci_device *odev = NULL;
	struct xpon_device xpon = {};
	struct omci_device_ops incomplete;
	struct omci_mib_object object = {};
	struct omci_service_config service = {};
	struct sk_buff *skb, *clone;
	struct task_struct *task;
	struct xarray desired;
	u8 enabled = 1;
	u8 serial[8] = { 'T', 'E', 'S', 'T', 0, 0, 0, 1 };
	unsigned int i, before, dropped;
	bool session_locked = false;
	bool array_initialized = false;
	int ret = 0;

	if (!strstr(init_utsname()->release, "-q1000k-omci-core-test"))
		return -EPERM;
	parent = root_device_register("q1000k-omci-fixture");
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	netdev = alloc_etherdev(0);
	CHECK(netdev);
	xpon.class_dev = parent;
	xpon.netdev = netdev;
	dev_set_drvdata(parent, &xpon);
	odev = omci_device_register(&xpon, OMCI_CAP_PROVIDER_MIC, &fixture_ops, NULL);
	if (IS_ERR(odev)) {
		ret = PTR_ERR(odev);
		odev = NULL;
		goto out;
	}
	CHECK(omci_device_start(odev) == -ENODATA);
	omci_device_set_identity(odev, serial, NULL);
	incomplete = fixture_ops;
	incomplete.replace_services = NULL;
	odev->ops = &incomplete;
	CHECK(omci_device_start(odev) == -EOPNOTSUPP);
	odev->ops = &fixture_ops;
	fixture_start_error = -ETIMEDOUT;
	CHECK(omci_device_start(odev) == -ETIMEDOUT);
	CHECK(!odev->started && !odev->channel_up && !fixture_tx);
	fixture_start_error = 0;
	CHECK(!omci_device_start(odev));
	CHECK(!fixture_tx);
	/* Extended content length must not wrap a u16 in TX validation. */
	skb = alloc_skb(13, GFP_KERNEL);
	CHECK(skb);
	memset(skb_put(skb, 13), 0, 13);
	skb->data[3] = OMCI_EXTENDED_DEV_ID;
	put_unaligned_be16(0xffff, skb->data + 8);
	i = omci_validate_tx(odev, skb);
	kfree_skb(skb);
	CHECK((int)i == -EMSGSIZE);
	CHECK(omci_agent_config_set(odev, OMCI_CONFIG_AGENT_FAKE_OMCI,
				    &enabled, 1) == -EOPNOTSUPP);
	CHECK(omci_agent_config_set(odev, OMCI_CONFIG_AGENT_PERMISSIVE,
				    &enabled, 1) == -EOPNOTSUPP);

	dropped = atomic64_read(&odev->rx_dropped);
	omci_device_receive(odev, fixture_packet(2, false), 7, 0, fixture_auth_epoch);
	omci_device_receive(odev, fixture_packet(2, false), 7, OMCI_F_MIC_PRESENT, fixture_auth_epoch);
	omci_device_receive(odev, fixture_packet(2, false), 7,
			    OMCI_F_MIC_VALID | OMCI_F_CRC_ERROR, fixture_auth_epoch);
	omci_device_receive(odev, fixture_packet(2, false), 8, OMCI_F_MIC_VALID, fixture_auth_epoch);
	CHECK(atomic64_read(&odev->rx_dropped) == dropped + 4);
	CHECK(!fixture_tx);
	/* Nonlinear cloned input must leave the original skb intact. */
	skb = fixture_packet(3, true);
	CHECK(skb);
	clone = skb_clone(skb, GFP_KERNEL);
	if (!clone) { kfree_skb(skb); ret = -ENOMEM; goto out; }
	omci_device_receive(odev, clone, 7, OMCI_F_MIC_VALID, fixture_auth_epoch);
	flush_work(&odev->rx_work);
	before = fixture_tx;
	i = skb->len;
	kfree_skb(skb);
	CHECK(before == 1 && i == 48);
	/* Corrupt the baseline length trailer: no MIB request or reply. */
	skb = fixture_packet(4, false);
	CHECK(skb);
	skb->data[43] = 41;
	omci_device_receive(odev, skb, 7, OMCI_F_MIC_VALID, fixture_auth_epoch);
	flush_work(&odev->rx_work);
	CHECK(fixture_tx == before);

	for (i = 0; i < 20; i++) {
		unsigned int n;
		/* Block real RX processing, then race stop against a full RX queue. */
		mutex_lock(&odev->session_lock);
		session_locked = true;
		omci_device_receive(odev, fixture_packet(10 + i, false), 7,
				    OMCI_F_MIC_VALID, fixture_auth_epoch);
		for (n = 0; n < 100 && skb_queue_len(&odev->rx_queue); n++)
			msleep(1);
		CHECK(!skb_queue_len(&odev->rx_queue));
		dropped = atomic64_read(&odev->rx_dropped);
		for (n = 0; n < 512; n++)
			omci_device_receive(odev, fixture_packet(n + 100, false),
					    7, OMCI_F_MIC_VALID, fixture_auth_epoch);
		CHECK(skb_queue_len(&odev->rx_queue) == OMCI_RX_QUEUE_LEN);
		CHECK(atomic64_read(&odev->rx_dropped) == dropped + 256);
		reinit_completion(&fixture_stopped);
		task = kthread_run(fixture_stop_thread, odev, "omci-stop-test");
		CHECK(!IS_ERR(task));
		for (n = 0; n < 100 && READ_ONCE(odev->started); n++)
			msleep(1);
		/* Always release the worker before checking a stop timeout. */
		mutex_unlock(&odev->session_lock);
		session_locked = false;
		wait_for_completion(&fixture_stopped);
		CHECK(!odev->started && !odev->channel_up);
		CHECK(!skb_queue_len(&odev->rx_queue) && fixture_tx == before);
		CHECK(!omci_device_start(odev));
	}
	CHECK(fixture_stops == 20);

	/* Rekey waits for the current transaction and invalidates queued work.
	 * It leaves the established MIB and channel intact, but rejects packets
	 * carrying the previous authentication epoch even if their MIC was valid.
	 */
	for (i = 0; i < 20; i++) {
		u64 old_epoch = fixture_auth_epoch;
		unsigned int mib_count = omci_mib_count_locked(&odev->agent);
		unsigned int n;

		mutex_lock(&odev->session_lock);
		session_locked = true;
		for (n = 0; n < 10; n++)
			omci_device_receive(odev, fixture_packet(2000 + n, false),
				7, OMCI_F_MIC_VALID, old_epoch);
		reinit_completion(&fixture_rekeyed);
		task = kthread_run(fixture_rekey_thread, odev, "omci-rekey-test");
		CHECK(!IS_ERR(task));
		msleep(5);
		n = completion_done(&fixture_rekeyed);
		mutex_unlock(&odev->session_lock);
		session_locked = false;
		wait_for_completion(&fixture_rekeyed);
		CHECK(!n && !fixture_rekey_result);
		flush_work(&odev->rx_work);
		CHECK(!odev->auth_epoch && odev->channel_up &&
		      !skb_queue_len(&odev->rx_queue));
		CHECK(omci_device_set_auth_epoch(odev, old_epoch) == -ESTALE);
		dropped = atomic64_read(&odev->rx_dropped);
		omci_device_receive(odev, fixture_packet(3000, false),
			7, OMCI_F_MIC_VALID, old_epoch);
		CHECK(atomic64_read(&odev->rx_dropped) == dropped + 1);
		CHECK(!omci_device_set_auth_epoch(odev, ++fixture_auth_epoch));
		CHECK(!omci_device_set_auth_epoch(odev, fixture_auth_epoch));
		omci_device_receive(odev, fixture_packet(3001, false),
			7, OMCI_F_MIC_VALID, old_epoch);
		CHECK(atomic64_read(&odev->rx_dropped) == dropped + 2);
		before = fixture_tx;
		omci_device_receive(odev, fixture_packet(4000 + i, false),
			7, OMCI_F_MIC_VALID, fixture_auth_epoch);
		flush_work(&odev->rx_work);
		CHECK(fixture_tx == before + 1);
		CHECK(omci_mib_count_locked(&odev->agent) == mib_count);
	}
	/* Closing authentication also waits for a provider TX already in flight. */
	fixture_hold_tx = true;
	reinit_completion(&fixture_tx_entered);
	reinit_completion(&fixture_tx_release);
	reinit_completion(&fixture_tx_done);
	task = kthread_run(fixture_tx_thread, odev, "omci-tx-test");
	CHECK(!IS_ERR(task));
	i = wait_for_completion_timeout(&fixture_tx_entered, HZ);
	if (!i) {
		complete(&fixture_tx_release);
		wait_for_completion(&fixture_tx_done);
		CHECK(i);
	}
	reinit_completion(&fixture_rekeyed);
	task = kthread_run(fixture_rekey_thread, odev, "omci-tx-rekey-test");
	if (IS_ERR(task)) {
		complete(&fixture_tx_release);
		wait_for_completion(&fixture_tx_done);
		CHECK(!IS_ERR(task));
	}
	msleep(5);
	i = completion_done(&fixture_rekeyed);
	complete(&fixture_tx_release);
	wait_for_completion(&fixture_tx_done);
	wait_for_completion(&fixture_rekeyed);
	fixture_hold_tx = false;
	CHECK(!i && !fixture_tx_result && !fixture_rekey_result && !odev->auth_epoch);
	CHECK(!omci_device_set_auth_epoch(odev, ++fixture_auth_epoch));

	/* A channel change invalidates authentication and cannot reuse an epoch. */
	omci_device_set_channel(odev, 8, true);
	CHECK(!odev->auth_epoch);
	CHECK(omci_device_set_auth_epoch(odev, fixture_auth_epoch) == -ESTALE);
	omci_device_set_channel(odev, 7, true);
	CHECK(!omci_device_set_auth_epoch(odev, ++fixture_auth_epoch));

	/* ONU reassignment cannot retain authenticated admission on the old OMCC. */
	omci_device_set_onu_id(odev, 123);
	CHECK(!odev->channel_up && !odev->auth_epoch && odev->onu_id == 123);
	CHECK(omci_device_set_auth_epoch(odev, ++fixture_auth_epoch) == -ENOLINK);
	omci_device_set_channel(odev, 7, true);
	CHECK(!omci_device_set_auth_epoch(odev, ++fixture_auth_epoch));

	/* A new ONU registration must not inherit the previous OLT's GEM MIB
	 * or a changed ONU-created T-CONT allocation. Rekey above preserved it.
	 */
	{
		struct omci_mib_object *stale;

		mutex_lock(&odev->agent.lock);
		stale = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 0x456, true);
		CHECK(stale);
		stale = omci_mib_lookup(&odev->agent, OMCI_CLASS_TCONT, 0x8000);
		CHECK(stale);
		put_unaligned_be16(123, stale->data);
		mutex_unlock(&odev->agent.lock);
		CHECK(!omci_device_reset_registration(odev));
		CHECK(!odev->channel_up && !odev->auth_epoch && odev->onu_id == 0xffff);
		CHECK(!omci_mib_lookup(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 0x456));
		stale = omci_mib_lookup(&odev->agent, OMCI_CLASS_TCONT, 0x8000);
		CHECK(stale && get_unaligned_be16(stale->data) == 0xffff);
		CHECK(!odev->agent.resetting_registration);
		omci_device_set_channel(odev, 7, true);
		CHECK(!omci_device_set_auth_epoch(odev, ++fixture_auth_epoch));
	}

	/* Real OMCI SET decoding must reach scheduling hardware and preserve the
	 * previous MIB when the backend rejects the write.
	 */
	{
		u8 request[] = { 0x01, 0x00, 17 };
		bool changed = false;
		struct omci_mib_object *q;

		CHECK(omci_agent_set_locked(odev, OMCI_CLASS_PRIORITY_QUEUE, 0x8003, OMCI_MSG_TYPE_SET,
			request, sizeof(request), &changed) == OMCI_RESULT_SUCCESS);
		q = omci_mib_lookup(&odev->agent, OMCI_CLASS_PRIORITY_QUEUE, 0x8003);
		CHECK(q && q->data[15] == 17 && fixture_queue_value.weight == 17);
		CHECK(fixture_queue_value.priority == 4 && fixture_queue_value.tcont_entity_id == 0x8000);
		CHECK(fixture_queue_value.maximum_size == 16 && fixture_queue_value.allocated_size == 4);
		CHECK(fixture_queue_value.backpressure_occur == 0xffff);
		fixture_scheduler_error = -EIO;
		request[2] = 29;
		CHECK(omci_agent_set_locked(odev, OMCI_CLASS_PRIORITY_QUEUE, 0x8003, OMCI_MSG_TYPE_SET,
			request, sizeof(request), &changed) == OMCI_RESULT_PROCESSING_ERROR);
		CHECK(q->data[15] == 17);
		fixture_scheduler_error = 0;
		request[0] = 0x20; request[1] = 0; request[2] = 2;
		CHECK(omci_agent_set_locked(odev, OMCI_CLASS_TRAFFIC_SCHEDULER, 0x8000, OMCI_MSG_TYPE_SET,
			request, sizeof(request), &changed) == OMCI_RESULT_SUCCESS);
		CHECK(fixture_scheduler_value.policy == 2 && fixture_scheduler_value.tcont_entity_id == 0x8000);
		request[0] = 0x80;
		CHECK(omci_agent_set_locked(odev, OMCI_CLASS_TRAFFIC_SCHEDULER, 0x8000, OMCI_MSG_TYPE_SET,
			request, sizeof(request), &changed) == OMCI_RESULT_PARAMETER_ERROR);
		q = omci_mib_lookup(&odev->agent, OMCI_CLASS_ONU2_G, 0);
		CHECK(q && get_unaligned_be16(q->data + 38) == BIT(3));
		fixture_scheduler_error = -EUCLEAN;
		request[0] = 0x20; request[2] = 1;
		CHECK(omci_agent_set_locked(odev, OMCI_CLASS_TRAFFIC_SCHEDULER, 0x8000, OMCI_MSG_TYPE_SET,
			request, sizeof(request), &changed) == OMCI_RESULT_PROCESSING_ERROR);
		CHECK(odev->agent.service_error == -EUCLEAN);
		fixture_scheduler_error = 0;
		odev->agent.service_error = 0; /* The fixture explicitly models a new port. */
	}

	/* Exercise real CREATE/SET/DELETE undo paths after service rejection.
	 * A successful undo leaves the original MIB usable; any failed undo
	 * closes the provider once and blocks all subsequent provisioning.
	 */
	{
		u8 request[] = { 0x80, 0x00, 0, 29 };
		u8 gem_create[14] = { 0, 99, 0x80, 0, 3 };
		bool changed = false;
		struct omci_mib_object *q;
		unsigned int faults;
		int failure, action;

		for (action = 0; action < 3; action++) {
			for (failure = 0; failure < 2; failure++) {
				service.cookie = 987;
				CHECK(!omci_agent_stage_service(odev->agent.services, &service));
				fixture_batch_error = action == 2 ? 0 : -ENOMEM;
				fixture_tcont_calls = fixture_gem_calls = 0;
				fixture_undo_call = 2;
				fixture_undo_error = failure ? -EIO : 0;
				fixture_gem_error = action == 2 ? -EIO : 0;
				faults = fixture_faults;
				odev->agent.operational = true;
				if (action == 0) {
					CHECK(omci_agent_set_locked(odev, OMCI_CLASS_TCONT, 0x8000,
						OMCI_MSG_TYPE_SET, request, sizeof(request), &changed) == OMCI_RESULT_PROCESSING_ERROR);
					q = omci_mib_lookup(&odev->agent, OMCI_CLASS_TCONT, 0x8000);
					CHECK(q && get_unaligned_be16(q->data) == 0xffff && fixture_tcont_calls == 2);
				} else if (action == 1) {
					CHECK(omci_agent_create_locked(odev, OMCI_CLASS_GEM_PORT_CTP, 99,
						gem_create, sizeof(gem_create)) == OMCI_RESULT_PROCESSING_ERROR);
					CHECK(!omci_mib_lookup(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 99));
					CHECK(fixture_gem_calls == 2);
				} else {
					q = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 99, true);
					CHECK(q);
					CHECK(omci_agent_delete_locked(odev, OMCI_CLASS_GEM_PORT_CTP, 99,
						&changed) == OMCI_RESULT_PROCESSING_ERROR);
					CHECK(omci_mib_lookup(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 99) == q);
					CHECK(!q->pending_delete && fixture_gem_calls == 2);
					kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_GEM_PORT_CTP, 99)));
				}
				CHECK(odev->agent.service_error == (failure ? -EUCLEAN : 0));
				CHECK(fixture_faults == faults + failure);
				if (failure) {
					CHECK(!odev->agent.operational);
					CHECK(omci_agent_set_locked(odev, OMCI_CLASS_PRIORITY_QUEUE, 0x8003,
						OMCI_MSG_TYPE_SET, request, sizeof(request), &changed) == OMCI_RESULT_PROCESSING_ERROR);
					CHECK(omci_agent_mib_reset(odev, true) == -EUCLEAN);
					CHECK(fixture_faults == faults + 1);
				}
				/* Only this synthetic provider models replacing a failed port. */
				odev->agent.service_error = odev->agent.reconcile_error = 0;
				fixture_batch_error = 0;
				CHECK(!omci_agent_clear_services_locked(odev));
			}
		}
		fixture_gem_error = -EOPNOTSUPP;
		fixture_undo_call = 0;
	}

	/* Explicit GEM upstream queue pointers override PCP fallback and must
	 * resolve to a real queue on the same T-CONT.
	 */
	{
		u8 queue = 6;
		struct omci_mib_object gem = {};

		CHECK(!omci_agent_service_queue_locked(odev, &gem, &queue) && queue == 6);
		put_unaligned_be16(0x8000, gem.data + 2);
		put_unaligned_be16(0x8003, gem.data + 5);
		CHECK(!omci_agent_service_queue_locked(odev, &gem, &queue) && queue == 3);
		put_unaligned_be16(0x8001, gem.data + 2);
		CHECK(omci_agent_service_queue_locked(odev, &gem, &queue) == -EINVAL);
		put_unaligned_be16(0x9000, gem.data + 5);
		CHECK(omci_agent_service_queue_locked(odev, &gem, &queue) == -ENOENT);
	}

	/* Class 171 stays a complete operation through the real MIB resolver.
	 * An empty configured table cannot fall back to an unrestricted service.
	 */
	{
		struct omci_mib_object *gem, *iwtp, *vlan, *filter, *tcont, *mapper;
		struct omci_mib_object lan = { .entity_id = 0xee0 };
		struct omci_service_state *state;
		bool fallback = false;
		unsigned long index;
		unsigned int count = 0;
		u8 raw[16] = { 0xf8, 0, 0, 0, 0xf8, 0, 0, 0, 0, 0x0f, 0, 0, 0, 0, 3, 0xdc };

		xa_init(&desired); array_initialized = true;
		gem = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 0xee1, true);
		iwtp = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_GEM_IWTP, 0xee2, true);
		vlan = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_EXTENDED_VLAN, 0xee3, true);
		tcont = omci_mib_lookup(&odev->agent, OMCI_CLASS_TCONT, 0x8000);
		mapper = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_8021P_MAPPER, 0xee5, true);
		CHECK(gem && iwtp && vlan && tcont && mapper);
		mapper->data[18] = 1; mapper->data[43] = 5;
		put_unaligned_be16(500, gem->data);
		put_unaligned_be16(0x8000, gem->data + 2); gem->data[4] = 3;
		put_unaligned_be16(0, gem->data + 5);
		put_unaligned_be16(0xee1, iwtp->data);
		put_unaligned_be16(200, tcont->data);
		vlan->extended_vlan.valid = true;
		vlan->extended_vlan.associated_me = lan.entity_id;
		vlan->extended_vlan.input_tpid = 0x88a8;
		vlan->extended_vlan.output_tpid = 0x8100;
		vlan->extended_vlan.rule_count = 0;
		CHECK(!omci_agent_stage_path_locked(odev, &desired, &lan, 1, 0xee4, 0xee5,
			0xee2, 0, false, false, 0, false, &fallback));
		CHECK(xa_empty(&desired) && !fallback);
		vlan->extended_vlan.rule_count = 1;
		omci_ext_vlan_parse_rule(&vlan->extended_vlan.rules[0], raw);
		CHECK(!omci_agent_stage_path_locked(odev, &desired, &lan, 1, 0xee4, 0xee5,
			0xee2, 0, false, false, 0, false, &fallback));
		xa_for_each(&desired, index, state) {
			CHECK(state->config.vlan_treatment_valid && !state->config.default_service);
			CHECK(!state->config.vlan_valid && !state->config.pcp_valid);
			CHECK(state->config.vlan_input_tpid == 0x88a8 && state->config.vlan_output_tpid == 0x8100);
			CHECK(!memcmp(state->config.vlan_rule.raw, raw, 16));
			CHECK(state->config.vlan_rule.filter_inner_pbit == 15 && state->config.vlan_rule.treat_inner_vid == 123);
			count++;
		}
		CHECK(count == 1 && !fallback);
		omci_agent_free_service_array(&desired);
		filter = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_VLAN_TAGGING_FILTER_DATA, lan.entity_id, true);
		CHECK(filter);
		filter->vlan_filter = (struct omci_vlan_tagging_filter) {
			.valid = true, .forward_operation = 0x10, .num_entries = 1,
			.entries = { { .tci = 123, .vid = 123 } },
		};
		CHECK(!omci_agent_stage_path_locked(odev, &desired, &lan, 1, 0xee4, 0xee5,
			0xee2, 0, false, false, 0, false, &fallback));
		count = 0;
		xa_for_each(&desired, index, state) {
			CHECK(state->config.vlan_entity_id == 0xee3 && !state->config.vlan_ani_side);
			CHECK(!memcmp(&state->config.vlan_filter[0], &filter->vlan_filter, sizeof(filter->vlan_filter)));
			CHECK(!state->config.vlan_filter[1].valid && !state->config.pcp_valid);
			count++;
		}
		CHECK(count == 1);
		omci_agent_free_service_array(&desired);
		/* Two priorities sharing one GEM retain all rows and unique cookies. */
		vlan->extended_vlan.rules[1] = vlan->extended_vlan.rules[0];
		vlan->extended_vlan.rules[1].raw[5] = 1;
		vlan->extended_vlan.rule_count = 2;
		CHECK(!omci_agent_stage_path_locked(odev, &desired, &lan, 1, 0xee4, 0xee5,
			0xee2, 0, true, false, 0, false, &fallback));
		CHECK(!omci_agent_stage_path_locked(odev, &desired, &lan, 1, 0xee4, 0xee5,
			0xee2, 1, true, false, 0, false, &fallback));
		count = 0;
		xa_for_each(&desired, index, state) count++;
		CHECK(count == 4);
		omci_agent_free_service_array(&desired);
		vlan->extended_vlan.rule_count = 1;
		/* The identical number in a different associated class is unrelated. */
		vlan->extended_vlan.association_type = 2;
		CHECK(omci_agent_vlan_side(vlan, &lan, 1, NULL, 0xee4, 0xee5, 0xee2) == -ENOENT);
		lan.data[3] = OMCI_BRIDGE_TP_PPTP_ETH_UNI;
		vlan->extended_vlan.associated_me = 1;
		CHECK(!omci_agent_vlan_side(vlan, &lan, 1, NULL, 0xee4, 0xee5, 0xee2));
		vlan->extended_vlan.association_type = 10;
		CHECK(omci_agent_vlan_side(vlan, &lan, 1, NULL, 0xee4, 0xee5, 0xee2) == -ENOENT);
		lan.data[3] = OMCI_BRIDGE_TP_VEIP;
		CHECK(!omci_agent_vlan_side(vlan, &lan, 1, NULL, 0xee4, 0xee5, 0xee2));
		vlan->extended_vlan.association_type = 0;
		vlan->extended_vlan.associated_me = 0xee4;
		CHECK(omci_agent_vlan_side(vlan, &lan, 1, NULL, 0xee4, 0xee5, 0xee2) == 1);
		CHECK(!omci_agent_stage_path_locked(odev, &desired, &lan, 1, 0xee4, 0xee5,
			0xee2, 5, true, false, 0, false, &fallback));
		xa_for_each(&desired, index, state) {
			CHECK(state->config.vlan_ani_side && state->config.pcp_valid && state->config.pcp == 5);
			CHECK(state->config.mapper_valid && state->config.mapper_unmarked_pcp == 5);
		}
		omci_agent_free_service_array(&desired);
		{
			struct omci_mib_object wan = {};
			wan.data[3] = OMCI_BRIDGE_TP_8021P_MAPPER;
			vlan->extended_vlan.association_type = 1;
			vlan->extended_vlan.associated_me = 0xee5;
			CHECK(omci_agent_vlan_side(vlan, &lan, 1, &wan, 0xee4, 0xee5, 0xee2) == 1);
			wan.data[3] = OMCI_BRIDGE_TP_GEM_IWTP;
			CHECK(omci_agent_vlan_side(vlan, &lan, 1, &wan, 0xee4, 0xee5, 0xee2) == -ENOENT);
			vlan->extended_vlan.association_type = 5;
			vlan->extended_vlan.associated_me = 0xee2;
			CHECK(omci_agent_vlan_side(vlan, &lan, 1, &wan, 0xee4, 0xee5, 0xee2) == 1);
		}
		for (i = 0; i < 8; i++) {
			unsigned int bit;
			mapper->data[18] = 0;
			memset(mapper->data + 19, 0, 24);
			for (bit = 0; bit < 192; bit++)
				if (i & BIT(2 - bit % 3)) mapper->data[19 + bit / 8] |= BIT(7 - bit % 8);
			CHECK(omci_agent_mapper_unmarked(mapper) == i);
			mapper->data[42] ^= 1;
			CHECK(omci_agent_mapper_unmarked(mapper) == -EOPNOTSUPP);
			mapper->data[18] = 1; mapper->data[43] = 0xf8 | i;
			CHECK(omci_agent_mapper_unmarked(mapper) == i);
		}
		mapper->data[18] = 2;
		CHECK(omci_agent_mapper_unmarked(mapper) == -EINVAL);
		kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_8021P_MAPPER, 0xee5)));
		/* Counts and modes are validated, not silently clamped. */
		filter->data[24] = 0x10; filter->data[25] = 12;
		CHECK(!omci_vlan_filter_parse_create(filter, filter->data));
		filter->data[25] = 13;
		CHECK(omci_vlan_filter_parse_create(filter, filter->data) == -EINVAL);
		filter->data[25] = 0; filter->data[24] = 0x22;
		CHECK(omci_vlan_filter_parse_create(filter, filter->data) == -EINVAL);
		filter->data[24] = 0x10;
		CHECK(!omci_vlan_filter_parse_create(filter, filter->data));
		{
			u8 count_value = 255;
			CHECK(omci_vlan_filter_parse_set(filter, OMCI_VLAN_FILTER_COUNT_MASK, &count_value, 1) == -EINVAL);
		}
		kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_GEM_PORT_CTP, 0xee1)));
		kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_GEM_IWTP, 0xee2)));
		kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_EXTENDED_VLAN, 0xee3)));
		kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_VLAN_TAGGING_FILTER_DATA, lan.entity_id)));
		put_unaligned_be16(0xffff, tcont->data);
		xa_destroy(&desired); array_initialized = false;
	}

	/* The last GEM CTP attribute is a one-byte set-by-create key ring. */
	{
		const struct omci_me_desc *desc = omci_me_lookup(&odev->agent, OMCI_CLASS_GEM_PORT_CTP);
		struct omci_mib_object candidate = { .class_id = OMCI_CLASS_GEM_PORT_CTP, .entity_id = 0xee8 };
		struct omci_mib_object *stored;
		u8 create[14] = {0, 99, 0x80, 0, 3};
		u8 request[3] = {0, 0x40, 0}, answer[32];
		size_t written = 0;

		CHECK(desc && desc->data_len == 16);
		fixture_gem_error = fixture_undo_call = 0;
		for (i = 0; i < 4; i++) {
			create[13] = i;
			CHECK(!omci_me_decode_create(desc, &candidate, create, sizeof(create)));
			CHECK(candidate.data[15] == i);
			CHECK(!omci_agent_hw_update(odev, &candidate, OMCI_MSG_TYPE_CREATE, create));
			CHECK(fixture_key_ring == i);
		}
		CHECK(!omci_me_decode_set(desc, &candidate, BIT(6), request + 2, 1));
		CHECK(!candidate.data[15]);
		stored = omci_get_or_create_locked(&odev->agent, OMCI_CLASS_GEM_PORT_CTP, 0xee8, true);
		CHECK(stored); *stored = candidate;
		put_unaligned_be16(BIT(8), request);
		for (i = 0; i < 2; i++) {
			fixture_gem_mode = i;
			CHECK(omci_agent_get_locked(odev, candidate.class_id, candidate.entity_id, request, 2,
				answer, sizeof(answer), &written) == OMCI_RESULT_SUCCESS);
			CHECK(written == 4 && answer[3] == i);
		}
		fixture_gem_read_error = -EIO;
		CHECK(omci_agent_get_locked(odev, candidate.class_id, candidate.entity_id, request, 2,
			answer, sizeof(answer), &written) == OMCI_RESULT_PROCESSING_ERROR);
		fixture_gem_read_error = 0;
		candidate.data[15] = 4;
		CHECK(omci_mib_parse_create(&candidate, candidate.data) == -EINVAL);
		kfree(xa_erase(&odev->agent.mib, omci_mib_key(OMCI_CLASS_GEM_PORT_CTP, 0xee8)));
		fixture_gem_error = -EOPNOTSUPP;
	}

	/* Missing provisioning callbacks cannot produce success. */
	incomplete = fixture_ops;
	incomplete.set_tcont = NULL;
	incomplete.set_gem_port = NULL;
	incomplete.set_uni = NULL;
	incomplete.set_priority_queue = NULL;
	incomplete.set_traffic_scheduler = NULL;
	odev->ops = &incomplete;
	object.class_id = OMCI_CLASS_PRIORITY_QUEUE;
	CHECK(omci_agent_hw_update(odev, &object, OMCI_MSG_TYPE_SET, NULL) == -EOPNOTSUPP);
	object.class_id = OMCI_CLASS_TRAFFIC_SCHEDULER;
	CHECK(omci_agent_hw_update(odev, &object, OMCI_MSG_TYPE_SET, NULL) == -EOPNOTSUPP);
	object.class_id = OMCI_CLASS_TCONT;
	CHECK(omci_agent_hw_update(odev, &object, OMCI_MSG_TYPE_SET, NULL) == -EOPNOTSUPP);
	object.class_id = OMCI_CLASS_GEM_PORT_CTP;
	CHECK(omci_agent_hw_update(odev, &object, OMCI_MSG_TYPE_CREATE, NULL) == -EOPNOTSUPP);
	object.class_id = OMCI_CLASS_VEIP;
	CHECK(omci_agent_hw_update(odev, &object, OMCI_MSG_TYPE_SET, NULL) == -EOPNOTSUPP);
	odev->ops = &fixture_ops;

	/* Real xarray snapshots survive atomic backend rejection and MIB reset. */
	xa_init(&desired);
	array_initialized = true;
	for (i = 1; i <= 32; i++) {
		service.cookie = i;
		service.gem_port_id = i;
		CHECK(!omci_agent_stage_service(&desired, &service));
	}
	CHECK(omci_agent_stage_service(&desired, &service) == -EEXIST);
	CHECK(!omci_agent_apply_services_locked(odev, &desired));
	CHECK(fixture_service_count == 32);
	/* Publish the already built snapshot, just as reconciliation does. */
	for (i = 1; i <= 32; i++) {
		struct omci_service_state *state = xa_load(&desired, i);
		CHECK(!omci_agent_stage_service(odev->agent.services, &state->config));
	}
	fixture_batch_error = -ETIMEDOUT;
	CHECK(omci_agent_clear_services_locked(odev) == -ETIMEDOUT);
	CHECK(xa_load(odev->agent.services, 32) && fixture_service_count == 32);
	before = omci_mib_count_locked(&odev->agent);
	CHECK(omci_agent_mib_reset(odev, true) == -ETIMEDOUT);
	CHECK(omci_mib_count_locked(&odev->agent) == before);
	CHECK(xa_load(odev->agent.services, 32));
	fixture_batch_error = 0;
	CHECK(!omci_agent_clear_services_locked(odev));
	CHECK(xa_empty(odev->agent.services) && !fixture_service_count);
	for (i = 33; i <= 257; i++) {
		service.cookie = i;
		CHECK(!omci_agent_stage_service(&desired, &service));
	}
	before = fixture_batch_calls;
	CHECK(omci_agent_apply_services_locked(odev, &desired) == -ENOSPC);
	CHECK(fixture_batch_calls == before);
	kfree(xa_erase(&desired, 257));
	fixture_batch_error = -EUCLEAN;
	CHECK(omci_agent_apply_services_locked(odev, &desired) == -EUCLEAN);
	fixture_batch_error = 0;
	before = fixture_batch_calls;
	CHECK(omci_agent_apply_services_locked(odev, &desired) == -EUCLEAN);
	CHECK(fixture_batch_calls == before);
	/* The final representable generation is usable once; it never wraps. */
	spin_lock_bh(&odev->state_lock);
	odev->generation = U64_MAX - 1;
	spin_unlock_bh(&odev->state_lock);
	CHECK(!omci_device_set_auth_epoch(odev, 0));
	CHECK(odev->generation == U64_MAX);
	CHECK(omci_device_set_auth_epoch(odev, ++fixture_auth_epoch) == -EOVERFLOW);
	CHECK(odev->generation == U64_MAX && odev->session_exhausted && !odev->channel_up);
	omci_device_set_channel(odev, 7, true);
	CHECK(!odev->channel_up && !odev->auth_epoch);
	pr_info("Q1000K_OMCI_CORE_PASS: authenticated RX, nonlinear skb, bounded queue, 20 stop races, 20 rekey races, generation saturation, atomic service failures\n");
out:
	if (session_locked)
		mutex_unlock(&odev->session_lock);
	if (array_initialized) {
		omci_agent_free_service_array(&desired);
		xa_destroy(&desired);
	}
	fixture_batch_error = 0;
	if (odev) {
		odev->ops = &fixture_ops;
		omci_device_unregister(odev);
	}
	if (netdev)
		free_netdev(netdev);
	root_device_unregister(parent);
	return ret;
}
