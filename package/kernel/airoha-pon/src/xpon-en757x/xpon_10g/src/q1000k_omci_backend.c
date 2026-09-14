// SPDX-License-Identifier: GPL-2.0-only
/* Software-authenticated OMCI transport and ordered XGS registration owner. */
#include <crypto/skcipher.h>
#include <an7581_xpon.h>
#include <q1000k_phy_api.h>
#include <linux/err.h>
#include <linux/rcupdate.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>
#include "common/xpon_global.h"
#include "common/phy_if_wrapper.h"
#include "common/q1000k_mac_cold.h"
#include "common/q1000k_identity.h"
#include "common/q1000k_mac_keys.h"
#include "common/q1000k_key_exchange.h"
#include "common/q1000k_gwan.h"
#include "common/q1000k_protocol.h"
#include "common/q1000k_transport.h"
#include "common/q1000k_services.h"
#include "common/q1000k_omci_backend.h"
#include "gpon/gpon_act.h"
#include "gpon/gpon_ploam.h"

#define QOMCI_ACKS 64
extern int snSendInO23Cnt;
struct qomci_key_request {
	bool pending, confirm;
	u8 index, sequence;
};
struct qomci_request {
	u64 generation;
	struct qomci_key_request data;
	u16 onu;
	u8 tag[8], state, index;
	u8 ack[QOMCI_ACKS], acks;
	bool profile, assign, reset, reset_phy, emergency;
	bool ranging;
	u32 delay;
	struct q1000k_pon_profile burst[4];
	u8 burst_mask;
};
struct qomci_backend {
	struct xpon_device *xpon;
	struct omci_device *omci;
	struct crypto_lskcipher *cipher, *ecb_cipher;
	spinlock_t auth_lock;
	struct q1000k_mac_keys keys;
	struct q1000k_key_state data;
	struct qomci_request request;
	struct q1000k_pon_profile burst[4];
	u8 burst_mask;
	u8 serial[8], registration[36], index;
	u16 onu;
	u64 epoch, published;
	bool keys_valid, active, started, cold_started;
	bool ranged;
	u32 delay;
	int service_error;
};
static struct qomci_backend __rcu *qomci_current;

int q1000k_omci_ploam_verify(const u8 *message, size_t length)
{
	static const u8 broadcast_key[16] = { [0 ... 15] = 0x55 };
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	const u8 *key;
	u16 dest;

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b || !b->cold_started)
		return -ENODEV;
	if (!message || length != 48)
		return -EMSGSIZE;
	/* The top six bits are reserved, but remain covered by the MIC. */
	dest = ((u16)message[0] << 8 | message[1]) & 0x3ff;
	if (dest == 0x3fe && message[2] != 0x01)
		return -EINVAL; /* This broadcast address is for Burst_Profile only. */
	if (dest == 0x3ff || dest == 0x3fe) {
		key = broadcast_key;
	} else {
		if (dest != b->onu || dest > 1020)
			return -ENOKEY;
		/* Deactivate and Request_Registration always use the default key,
		 * even when keys are established or the peers no longer agree.
		 */
		if (message[2] == 0x05 || message[2] == 0x09) {
			key = broadcast_key;
		} else {
			if (!b->keys_valid || b->request.reset ||
			    (GPON_CURR_STATE != GPON_10G_STATE_O4 && GPON_CURR_STATE != GPON_10G_STATE_O5))
				return -ENOKEY;
			/* Mutual-authentication rekey is not yet supported. No implicit
			 * fallback or key selection from the untrusted FIFO trailer.
			 */
			key = b->keys.bank[0].ploam;
		}
	}
	return q1000k_auth_ploam_verify(b->cipher, key, message, length);
}

static void qomci_close(struct qomci_backend *b)
{
	spin_lock_bh(&b->auth_lock);
	b->active = false;
	spin_unlock_bh(&b->auth_lock);
	q1000k_services_enable(false);
}

static int qomci_start(struct omci_device *odev)
{
	struct qomci_backend *b = omci_device_priv(odev);

	WRITE_ONCE(b->started, true);
	return 0;
}

static void qomci_stop(struct omci_device *odev)
{
	struct qomci_backend *b = omci_device_priv(odev);

	qomci_close(b);
	WRITE_ONCE(b->started, false);
	q1000k_transport_set_auth_epoch(0);
}

static int qomci_xmit(struct omci_device *odev, struct sk_buff *skb, u16 gem, u64 epoch)
{
	struct qomci_backend *b = omci_device_priv(odev);
	struct sk_buff *copy;
	u8 mic[4], index;
	int ret;

	copy = skb_copy_expand(skb, 0, 4, GFP_ATOMIC);
	if (!copy)
		return -ENOMEM;
	spin_lock_bh(&b->auth_lock);
	index = b->index;
	if (!b->active || !b->keys_valid || epoch != b->published || gem != b->onu ||
	    q1000k_protocol_status())
		ret = -EKEYREJECTED;
	else
		ret = q1000k_auth_omci_mic(b->cipher, b->keys.bank[index].omci,
			copy, false, Q1000K_OMCI_UPSTREAM, mic);
	spin_unlock_bh(&b->auth_lock);
	if (!ret) {
		memcpy(skb_put(copy, sizeof(mic)), mic, sizeof(mic));
		copy->ip_summed = CHECKSUM_NONE;
		ret = q1000k_transport_xmit_omci(copy, gem, index, epoch);
	}
	memzero_explicit(mic, sizeof(mic));
	if (ret)
		dev_kfree_skb_any(copy);
	else
		dev_kfree_skb_any(skb);
	return ret;
}

static void qomci_config_changed(struct omci_device *odev, u16 key,
				 const struct omci_identity *identity)
{
	struct qomci_backend *b = omci_device_priv(odev);

	/* Registration identity belongs to the loader and remains fixed for
	 * this module lifetime. A changed core identity cannot share its keys.
	 */
	if (READ_ONCE(b->started) &&
	    (memcmp(identity->serial_number, b->serial, 8) ||
	     memcmp(identity->vendor_id, b->serial, 4))) {
		qomci_close(b);
		q1000k_protocol_fail(-ESTALE);
	}
}

static void qomci_service_fault(struct omci_device *odev, int error)
{
	struct qomci_backend *b = omci_device_priv(odev);

	/* The core holds its agent mutex. Close software ingress immediately;
	 * the protocol fault worker owns subsequent physical containment.
	 */
	qomci_close(b);
	WRITE_ONCE(b->service_error, error);
	q1000k_protocol_fail(error);
}

static const struct omci_device_ops qomci_ops = {
	.start = qomci_start, .stop = qomci_stop, .xmit = qomci_xmit,
	.get_ani_topology = q1000k_services_topology,
	.set_tcont = q1000k_services_tcont, .set_gem_port = q1000k_services_gem,
	.set_uni = q1000k_services_uni, .replace_services = q1000k_services_replace,
	.set_priority_queue = q1000k_services_queue,
	.set_traffic_scheduler = q1000k_services_scheduler,
	.config_changed = qomci_config_changed,
	.service_fault = qomci_service_fault,
};

void q1000k_omci_receive(struct sk_buff *skb, u16 gem, bool crc_error)
{
	struct qomci_backend *b = rcu_dereference(qomci_current);
	u64 epoch = 0;
	int ret = -ENOLINK;

	if (b && !crc_error) {
		spin_lock_bh(&b->auth_lock);
		if (b->active && b->keys_valid && gem == b->onu && !q1000k_protocol_status()) {
			epoch = b->published;
			ret = q1000k_auth_omci_verify(b->cipher, b->keys.bank[b->index].omci, skb);
		}
		spin_unlock_bh(&b->auth_lock);
	}
	if (ret) {
		dev_kfree_skb_any(skb);
		return;
	}
	/* A descriptor MIC-present bit is never used as authentication. Core
	 * admission compares this exact verified epoch, even across a rekey.
	 */
	omci_device_receive(b->omci, skb, gem, OMCI_F_MIC_VALID, epoch);
}

static int qomci_request(struct qomci_backend *b)
{
	int ret;

	qomci_close(b);
	if (b->request.generation == U64_MAX)
		ret = -EOVERFLOW;
	else {
		b->request.generation++;
		b->request.state = GPON_CURR_STATE;
		b->request.index = gpGponPriv->gponSecurity.omciIkIdx;
		ret = q1000k_protocol_control();
	}
	if (ret)
		q1000k_protocol_fail(ret);
	return ret;
}

int q1000k_omci_reset(bool emergency, bool reset_phy)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	int ret;

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b || !b->cold_started)
		return -ENODEV;
	qomci_close(b);
	ret = q1000k_phy_set_tx(false);
	if (ret) {
		q1000k_protocol_fail(ret);
		return ret;
	}
	b->request.reset = true;
	b->request.reset_phy |= reset_phy;
	b->request.emergency = emergency;
	/* Latch policy as soon as TX is off, including multiple commands in
	 * one receive batch. The activation state changes after physical reset.
	 */
	gpGponPriv->emergencyState = emergency;
	b->request.onu = 0xffff;
	b->request.assign = b->request.profile = false;
	b->request.acks = 0;
	b->request.burst_mask = 0;
	b->request.ranging = false;
	memset(&b->request.data, 0, sizeof(b->request.data));
	return qomci_request(b);
}

int q1000k_omci_profile(const u8 tag[8], u8 sequence, bool acknowledge)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b || !tag)
		return -ENODEV;
	if (b->request.reset)
		return -EAGAIN;
	if (b->request.data.pending)
		return -EBUSY;
	/* Do not acknowledge a superseded tag with a different key. The OLT
	 * can retry a new profile after this ordered control boundary.
	 */
	if (b->request.acks && memcmp(tag, b->request.tag, 8))
		return -EBUSY;
	if (acknowledge) {
		if (b->request.acks == QOMCI_ACKS) {
			qomci_close(b); q1000k_protocol_fail(-ENOSPC); return -ENOSPC;
		}
	}
	if (!b->keys_valid || memcmp(tag, b->keys.pon_tag, 8) || b->request.profile) {
		if (b->request.profile && memcmp(tag, b->request.tag, 8))
			b->request.burst_mask = 0;
		b->request.profile = true;
	}
	memcpy(b->request.tag, tag, 8);
	if (acknowledge)
		b->request.ack[b->request.acks++] = sequence;
	if (!b->request.profile && !acknowledge)
		return 0;
	return qomci_request(b);
}

int q1000k_omci_burst_profile(const struct q1000k_pon_profile *p,
			    const u8 tag[8], u8 sequence, bool acknowledge)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	int ret;

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b)
		return -ENODEV;
	if (!q1000k_pon_profile_valid(p) || !tag)
		return -EINVAL;
	if ((b->request.burst_mask & BIT(p->index)) && b->request.acks &&
	    memcmp(p, &b->request.burst[p->index], sizeof(*p)))
		return -EBUSY;
	ret = q1000k_omci_profile(tag, sequence, acknowledge);
	if (ret)
		return ret;
	b->request.burst[p->index] = *p;
	b->request.burst_mask |= BIT(p->index);
	/* Same-tag broadcasts also need a physical profile install. */
	return qomci_request(b);
}

int q1000k_omci_assign(u16 onu)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b)
		return -ENODEV;
	if (onu > 1020)
		return -EINVAL;
	if (b->request.reset)
		return -EAGAIN;
	if (GPON_CURR_STATE == GPON_10G_STATE_O2_3) {
		b->request.onu = onu;
		b->request.assign = true;
	} else if (GPON_CURR_STATE == GPON_10G_STATE_O4 || GPON_CURR_STATE == GPON_10G_STATE_O5) {
		if (onu == gpGponPriv->gponCfg.onu_id)
			return 0;
		return q1000k_omci_reset(false, false);
	} else {
		return -EINVAL;
	}
	return qomci_request(b);
}

int q1000k_omci_registration_keys(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	GPON_Security_t *security;
	u8 ploam, omci;
	int ret;

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b)
		return -ENODEV;
	if (GPON_CURR_STATE != GPON_10G_STATE_O4 && GPON_CURR_STATE != GPON_10G_STATE_O5)
		return -EINVAL;
	security = &gpGponPriv->gponSecurity;
	if (!b->keys_valid || !b->burst_mask || b->request.reset || security->smaValid != GPON_SMA_INVALID)
		ret = -ENOKEY;
	else {
		ret = q1000k_mac_key_indices(&ploam, &omci);
		/* Ranging/registration uses the registration-derived bank zero.
		 * A partial switch cannot publish a usable integrity/KEK epoch.
		 */
		if (!ret && (ploam || omci))
			ret = -EKEYREJECTED;
	}
	if (ret) {
		qomci_close(b);
		q1000k_protocol_fail(ret);
		return ret;
	}
	if (!security->ploamIkIdx && !security->omciIkIdx && !security->kekIdx &&
	    security->registerIDState == GPON_REG_ID_REPORTED)
		return 0;
	qomci_close(b);
	security->ploamIkIdx = security->omciIkIdx = security->kekIdx = 0;
	security->registerIDState = GPON_REG_ID_REPORTED;
	return qomci_request(b);
}

int q1000k_omci_ranged(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!q1000k_protocol_owned())
		return -EPERM;
	return b && b->ranged && !b->request.reset ? 0 : -EAGAIN;
}

int q1000k_omci_ranging(u32 delay, bool absolute, bool negative, u8 sequence, bool acknowledge)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	u32 previous, candidate;
	int ret;

	if (!q1000k_protocol_owned())
		return -EPERM;
	if (!b)
		return -ENODEV;
	if (GPON_CURR_STATE != GPON_10G_STATE_O4 && GPON_CURR_STATE != GPON_10G_STATE_O5)
		return -EINVAL;
	if (!absolute && GPON_CURR_STATE == GPON_10G_STATE_O4)
		return -EINVAL;
	if (b->request.reset || b->request.assign || b->request.profile)
		return -EAGAIN;
	if (!b->keys_valid || !b->burst_mask || b->onu == 0xffff)
		return -ENOKEY;
	previous = b->request.ranging ? b->request.delay : b->delay;
	if (delay > (~0U >> 2) || (!absolute && (negative ?
	    delay > previous : delay > (~0U >> 2) - previous)))
		return -ERANGE;
	candidate = absolute ? delay : negative ? previous - delay : previous + delay;
	if (b->request.ranging && b->request.acks && candidate != b->request.delay)
		return -EBUSY;
	ret = q1000k_omci_registration_keys();
	if (!ret)
		ret = q1000k_omci_profile(b->keys.pon_tag, sequence, acknowledge);
	if (ret)
		return ret;
	b->request.ranging = true;
	b->request.delay = candidate;
	return qomci_request(b);
}

void q1000k_omci_state(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!b)
		return;
	if (WARN_ON_ONCE(!q1000k_protocol_owned()))
		return;
	if (GPON_CURR_STATE == GPON_10G_STATE_O1 || GPON_CURR_STATE == GPON_10G_STATE_O7) {
		q1000k_omci_reset(GPON_CURR_STATE == GPON_10G_STATE_O7, false);
		return;
	}
	qomci_request(b);
}

int q1000k_omci_alloc_changed(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!q1000k_protocol_owned())
		return -EPERM;
	return b ? qomci_request(b) : -ENODEV;
}

int q1000k_omci_key_control(bool confirm, u8 index, u8 length, u8 sequence)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!q1000k_protocol_owned()) return -EPERM;
	if (!b || !b->cold_started) return -ENODEV;
	if (index < 1 || index > 2) return -EINVAL;
	if (!confirm && length != 16) return -EOPNOTSUPP;
	if (GPON_CURR_STATE != GPON_10G_STATE_O5 || !b->ranged || !b->keys_valid ||
	    b->onu > 1020 || b->index || gpGponPriv->gponSecurity.smaValid != GPON_SMA_INVALID)
		return -ENOKEY;
	if (b->request.reset || b->request.assign || b->request.profile ||
	    b->request.ranging || b->request.burst_mask || b->request.acks)
		return -EAGAIN;
	/* One authenticated request per control boundary. The OLT owns retry;
	 * do not overwrite a not-yet-reported key request from this RX batch.
	 */
	if (b->request.data.pending) return -EBUSY;
	if (!confirm && b->data.regenerating != index && !rng_is_initialized())
		return -EAGAIN;
	b->request.data = (struct qomci_key_request) {
		.pending = true, .confirm = confirm, .index = index, .sequence = sequence,
	};
	return qomci_request(b);
}

static int qomci_key_report(u16 onu, const struct qomci_key_request *request,
			   const struct q1000k_key_update *update)
{
	u8 message[44] = {};
	int ret;

	/* Registration-derived PIK bank zero, one 16-byte AES-128 fragment.
	 * The MAC computes the upstream PLOAM MIC over the 40-byte body.
	 */
	message[4] = onu >> 8;
	message[5] = onu;
	message[6] = 5;
	message[7] = request->sequence;
	message[8] = request->confirm;
	message[9] = update->report_index;
	memcpy(message + 12, update->report, 32);
	ret = q1000k_ploam_send(message);
	if (!ret) gpGponPriv->ploamMsgcounter.txPloamMsgCnt++;
	memzero_explicit(message, sizeof(message));
	return ret;
}

struct qomci_install {
	struct q1000k_mac_keys keys;
	struct q1000k_key_update data;
	bool key_switch;
	struct q1000k_pon_profile burst[4];
	u8 burst_mask;
	u16 onu;
	bool valid, registration, cold, reset_phy, emergency;
	bool ranging;
	u32 delay;
	const u8 *serial, *registration_id;
};
static int qomci_install(void *arg)
{
	struct qomci_install *install = arg;
	unsigned int i;
	int ret = 0;

	if (install->cold) {
		if (install->reset_phy)
			ret = q1000k_phy_configure(PHY_XGSPON_CONFIG);
		if (!ret)
			ret = XPON_PHY_SET_RX_ENABLE();
		if (!ret)
			ret = XPON_PHY_SET_RX_FEC(DS_FEC_SETTING_FORCE_ON);
		if (!ret)
			ret = q1000k_mac_cold_install(install->serial, install->registration_id,
				install->emergency);
	}
	if (!ret && (install->valid || install->cold))
		ret = q1000k_mac_keys_install(&install->keys);
	if (!ret && install->data.changed)
		ret = q1000k_mac_data_keys_install(&install->data.next.mac, &install->key_switch);
	if (!ret && install->registration)
		ret = q1000k_mac_onu_install(install->onu);
	if (!ret)
		ret = q1000k_mac_profiles_invalidate();
	if (!ret && install->ranging)
		ret = q1000k_mac_ranging_install(install->delay);
	for (i = 0; !ret && i < 4; i++) {
		const struct q1000k_pon_profile *p = &install->burst[i];

		if (!(install->burst_mask & BIT(i)))
			continue;
		ret = q1000k_phy_profile_set(p);
		if (!ret)
			ret = q1000k_mac_profile_install(i, p->version,
				(u16)p->preamble_len * p->repeat + p->delimiter_len);
	}
	if (!ret && install->cold) {
		ret = q1000k_mac_cold_select_keys();
		if (!ret) {
			gpon_INT_init();
			ret = q1000k_protocol_status();
		}
	}
	return ret;
}

static int qomci_ready(void *arg)
{
	struct qomci_install *install = arg;
	int ret = install->ranging ? q1000k_mac_ranging_ready() : 0;

	return ret ?: q1000k_mac_data_keys_ready(install->key_switch);
}

static void qomci_legacy_keys(const struct q1000k_mac_keys *keys)
{
	unsigned int i;

	memcpy(gpGponPriv->gponCfg.ponTag, keys->pon_tag, 8);
	memcpy(gpGponPriv->gponSecurity.msk, keys->bank[0].msk, 16);
	memcpy(gpGponPriv->gponSecurity.sk, keys->bank[0].session, 16);
	for (i = 0; i < 2; i++) {
		memcpy(gpGponPriv->gponSecurity.ploamIk[i], keys->bank[i].ploam, 16);
		memcpy(gpGponPriv->gponSecurity.omciIk[i], keys->bank[i].omci, 16);
		memcpy(gpGponPriv->gponSecurity.kek[i], keys->bank[i].kek, 16);
	}
}

static void qomci_reset_legacy(bool emergency)
{
	GPON_Security_t *security = &gpGponPriv->gponSecurity;
	u8 state = emergency ? GPON_10G_STATE_O7 : GPON_10G_STATE_O1;

	/* The cold transaction already installed this hardware state and
	 * canceled old timer/task jobs. Preserve crypto/timer allocations.
	 */
	snSendInO23Cnt = 0;
	memset(&gpGponPriv->prePloamMsg, 0, sizeof(gpGponPriv->prePloamMsg));
	gpGponPriv->gponCfg.eqd = 0;
	gpGponPriv->gponCfg.eqd_olt_absolute = 0;
	gpGponPriv->gponCfg.eqd_olt_init = 0;
	gpGponPriv->gponCfg.usOmciMicCtrl = XGPON_SW;
	gpGponPriv->gponCfg.dsOmciMicCtrl = XGPON_SW;
	gpGponPriv->typeBOnGoing = false;
	gpGponPriv->emergencyState = emergency;
	gpGponPriv->gpon_traffic_status = TRAFFIC_DOWN;
	gpGponPriv->gemUpAESMode = UPAES_MODE_NONE;
	memzero_explicit(security->aesUcKey, sizeof(security->aesUcKey));
	memzero_explicit(security->aesBcKey, sizeof(security->aesBcKey));
	security->aesUcKeyIdx = 0;
	security->ploamIkIdx = security->omciIkIdx = security->kekIdx = 1;
	security->smaValid = GPON_SMA_INVALID;
	security->registerIDState = GPON_REG_ID_NOT_REPORT;
	security->state = KEY_STATE_KN0;
	/* txKeyValid is the vendor policy flag, not hardware key validity. */
	security->txKeyValid = 1;
	GPON_CURR_STATE = state;
	xmcs_report_event(XMCS_EVENT_TYPE_GPON, XMCS_EVENT_GPON_STATE_CHANGE, state);
}

void q1000k_omci_control(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	struct qomci_install *install;
	struct qomci_request request;
	u64 epoch;
	unsigned int i;
	bool up;
	int token, ret = 0;

	if (!b)
		return;
	install = kzalloc(sizeof(*install), GFP_KERNEL);
	if (!install) { q1000k_protocol_fail(-ENOMEM); return; }
again:
	memzero_explicit(install, sizeof(*install));
	token = q1000k_protocol_enter();
	if (token < 0) { ret = token; goto failed; }
	request = b->request;
	memcpy(install->burst, b->burst, sizeof(install->burst));
	install->burst_mask = b->burst_mask;
	if (request.reset || (request.profile && memcmp(request.tag, b->keys.pon_tag, 8)))
		install->burst_mask = 0;
	for (i = 0; i < 4; i++)
		if (!request.reset && (request.burst_mask & BIT(i))) {
			install->burst[i] = request.burst[i];
			install->burst_mask |= BIT(i);
		}
	install->keys = b->keys;
	install->valid = b->keys_valid;
	install->registration = request.assign || request.reset;
	install->data.next = b->data;
	if (install->registration) {
		memzero_explicit(&install->data.next, sizeof(install->data.next));
		install->data.changed = true;
	}
	install->cold = request.reset;
	install->reset_phy = request.reset_phy;
	install->emergency = request.emergency;
	install->serial = b->serial;
	install->registration_id = b->registration;
	install->ranging = request.ranging && !request.reset;
	install->delay = request.delay;
	if (install->cold)
		install->valid = false;
	install->onu = request.reset ? 0xffff : request.assign ? request.onu : b->onu;
	qomci_close(b);
	q1000k_protocol_leave(token);

	/* Never hold the executor while waiting for the core. Its active RX
	 * transaction may be inside a provisioning callback waiting to enter.
	 */
	ret = omci_device_set_auth_epoch(b->omci, 0);
	if (!ret)
		ret = q1000k_transport_set_auth_epoch(0);
	if (ret)
		goto failed;
	if (install->registration) {
		ret = omci_device_reset_registration(b->omci);
		if (ret)
			goto failed;
	}
	if (request.profile || request.reset) {
		const u8 empty_tag[8] = {};

		ret = q1000k_mac_keys_derive(b->cipher, b->registration, b->serial,
			request.reset ? empty_tag : request.tag, &install->keys);
		if (ret)
			goto failed;
		install->valid = !request.reset;
	}
	if (request.data.pending) {
		struct q1000k_key_state prior = install->data.next;

		ret = q1000k_key_prepare(b->ecb_cipher, b->cipher, install->keys.bank[0].kek,
			&prior, request.data.confirm, request.data.index, &install->data);
		memzero_explicit(&prior, sizeof(prior));
		if (ret) goto failed;
	}
	if (request.assign && (!install->valid || !install->burst_mask)) { ret = -ENOKEY; goto failed; }
	if (request.index > 1) { ret = -EINVAL; goto failed; }
	token = q1000k_protocol_enter();
	if (token < 0) { ret = token; goto failed; }
	if (request.generation != b->request.generation) {
		q1000k_protocol_leave(token); goto again;
	}
	up = request.state == GPON_10G_STATE_O5 && install->onu != 0xffff &&
		install->valid && install->burst_mask && b->ranged;
	if (install->cold)
		ret = q1000k_gwan_cold_reset(qomci_install, install);
	else if (install->registration)
		ret = q1000k_gwan_register(install->onu, qomci_install, install);
	else if (install->ranging || install->data.changed)
		ret = q1000k_gwan_refresh_checked(qomci_install, qomci_ready, install);
	else if (request.profile || request.burst_mask || up)
		ret = q1000k_gwan_refresh(qomci_install, install);
	if (ret) {
		q1000k_protocol_leave(token); goto failed;
	}
	if (request.data.pending) {
		ret = qomci_key_report(install->onu, &request.data, &install->data);
		if (ret) { q1000k_protocol_leave(token); goto failed; }
	}
	/* Publish only after physical completion and successful report enqueue. */
	b->data = install->data.next;
	memset(&b->request.data, 0, sizeof(b->request.data));
	if (install->registration) {
		q1000k_services_reset();
		b->onu = install->onu;
		gpGponPriv->gponCfg.onu_id = install->onu == 0xffff ? GPON_UNASSIGN_ONU_ID : install->onu;
		gpGponPriv->gponCfg.omcc = gpGponPriv->gponCfg.onu_id;
		b->request.assign = b->request.reset = false;
	}
	if (install->cold) {
		b->ranged = false;
		b->delay = 0;
		qomci_reset_legacy(request.emergency);
		request.state = GPON_CURR_STATE;
		request.index = 1;
		b->request.state = request.state;
		b->request.index = 1;
		b->request.reset_phy = false;
		b->service_error = 0;
	}
	if (install->valid || install->cold)
		qomci_legacy_keys(&install->keys);
	memcpy(b->burst, install->burst, sizeof(b->burst));
	b->burst_mask = install->burst_mask;
	b->request.burst_mask = 0;
	spin_lock_bh(&b->auth_lock);
	b->keys = install->keys;
	b->keys_valid = install->valid;
	b->index = request.index;
	spin_unlock_bh(&b->auth_lock);
	b->request.profile = false;
	b->request.acks = 0;
	if (install->ranging) {
		b->ranged = true;
		b->delay = install->delay;
		gpGponPriv->gponCfg.eqd_olt_absolute = install->delay;
		gpGponPriv->gponCfg.eqd_olt_init = install->delay;
		gpGponPriv->gponCfg.eqd = install->delay << 2;
		b->request.ranging = false;
		if (GPON_CURR_STATE == GPON_10G_STATE_O4)
			gpon_act_change_state(GPON_10G_STATE_O5);
		ret = q1000k_protocol_status();
		if (ret) { q1000k_protocol_leave(token); goto failed; }
		request.state = b->request.state = GPON_CURR_STATE;
		request.generation = b->request.generation;
		up = request.state == GPON_10G_STATE_O5;
		snSendInO23Cnt = 0;
	}
	for (i = 0; i < request.acks; i++)
		ploam_send_acknowledge_msg(request.ack[i], XGPON_PLOAM_ACK_OK);
	if (request.assign) {
		gpon_act_change_state(GPON_10G_STATE_O4);
		if (gpGponPriv->gponCfg.ploamCtrl == XGPON_SW)
			q1000k_protocol_task_schedule(&gpGponPriv->swreplyploam_task);
	}
	if (b->epoch == U64_MAX) { ret = -EOVERFLOW; q1000k_protocol_leave(token); goto failed; }
	epoch = ++b->epoch;
	q1000k_protocol_leave(token);

	omci_device_set_onu_id(b->omci, install->onu);
	omci_device_set_state(b->omci, request.state);
	omci_device_set_channel(b->omci, install->onu, up);
	/* An Alloc-ID notification does not change the OMCC ID. Reconcile the
	 * MIB explicitly so dormant/profile-seeded GEMs use the new physical
	 * channel and scheduler before their queues open. Ordinary preparation
	 * errors remain repairable through OMCI; uncertain hardware is fatal.
	 */
	if (up) {
		ret = omci_device_reconcile_services(b->omci);
		WRITE_ONCE(b->service_error, ret);
		if (ret == -EUCLEAN)
			goto failed;
	}
	xpon_device_report_registration(b->xpon, up ? XPON_REGISTRATION_OPERATIONAL :
		install->onu == 0xffff ? XPON_REGISTRATION_DISCOVERY : XPON_REGISTRATION_REGISTERING);
	xpon_device_report_carrier(b->xpon, up);
	if (up) {
		ret = q1000k_transport_set_auth_epoch(epoch);
		if (!ret)
			ret = omci_device_set_auth_epoch(b->omci, epoch);
		if (ret)
			goto failed;
	}
	token = q1000k_protocol_enter();
	if (token < 0) { ret = token; goto failed; }
	if (request.generation != b->request.generation) {
		q1000k_protocol_leave(token); goto again;
	}
	if (up) {
		ret = q1000k_transport_set_queue_close(0, 0xfe);
		if (!ret) {
			spin_lock_bh(&b->auth_lock);
			b->published = epoch;
			b->active = READ_ONCE(b->started);
			spin_unlock_bh(&b->auth_lock);
			q1000k_services_enable(READ_ONCE(b->started));
		}
	}
	q1000k_protocol_leave(token);
	if (ret)
		goto failed;
	kfree_sensitive(install);
	return;
failed:
	qomci_close(b);
	omci_device_set_auth_epoch(b->omci, 0);
	q1000k_transport_set_auth_epoch(0);
	q1000k_protocol_fail(ret);
	kfree_sensitive(install);
}

int q1000k_omci_cold_start(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);
	int token, ret;

	if (!b)
		return -ENODEV;
	token = q1000k_protocol_enter();
	if (token < 0)
		return token;
	if (b->cold_started) {
		q1000k_protocol_leave(token);
		return -EALREADY;
	}
	b->cold_started = true;
	b->request.reset = b->request.reset_phy = true;
	b->request.state = GPON_10G_STATE_O1;
	b->request.index = 1;
	b->request.generation = 1;
	q1000k_protocol_leave(token);
	/* Module initialization has not started the protocol worker yet. */
	q1000k_omci_control();
	ret = q1000k_protocol_status();
	return ret;
}

int q1000k_omci_backend_init(struct net_device *dev)
{
	struct qomci_backend *b;
	struct xpon_device_desc desc = { .netdev = dev, .mode = XPON_MODE_XGSPON,
		.modes = XPON_MODE_CAP(XPON_MODE_XGSPON) };
	struct omci_identity identity = {};
	int ret;

	if (!dev || rcu_access_pointer(qomci_current))
		return -EINVAL;
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	spin_lock_init(&b->auth_lock);
	b->onu = b->request.onu = 0xffff;
	ret = q1000k_pon_get_serial(b->serial, 8);
	if (!ret)
		ret = q1000k_pon_get_registration(b->registration, 36);
	if (ret)
		goto free;
	b->cipher = crypto_alloc_lskcipher("ecb(aes)", 0, 0);
	if (IS_ERR(b->cipher)) { ret = PTR_ERR(b->cipher); goto free; }
	/* The imported ECB and CMAC helpers use separate locks. Never share
	 * their transform while OMCI RX/TX can compute a MIC concurrently.
	 */
	b->ecb_cipher = crypto_alloc_lskcipher("ecb(aes)", 0, 0);
	if (IS_ERR(b->ecb_cipher)) { ret = PTR_ERR(b->ecb_cipher); goto cmac; }
	q1000k_services_init();
	b->xpon = xpon_device_register(get_xpon_dev(), &desc);
	if (IS_ERR(b->xpon)) { ret = PTR_ERR(b->xpon); goto cipher; }
	b->omci = omci_device_register(b->xpon, OMCI_CAP_PROVIDER_MIC, &qomci_ops, b);
	if (IS_ERR(b->omci)) { ret = PTR_ERR(b->omci); goto xpon; }
	identity.serial_source = identity.vendor_source = OMCI_CONFIG_SOURCE_DRIVER;
	identity.valid = OMCI_IDENTITY_F_SERIAL_NUMBER | OMCI_IDENTITY_F_VENDOR_ID | OMCI_IDENTITY_F_EQUIPMENT_ID;
	identity.equipment_source = OMCI_CONFIG_SOURCE_DRIVER;
	memcpy(identity.equipment_id, "Q1000K", 6);
	memcpy(identity.serial_number, b->serial, 8);
	memcpy(identity.vendor_id, b->serial, 4);
	omci_device_set_identity_info(b->omci, &identity);
	ret = omci_device_start(b->omci);
	if (ret)
		goto omci;
	rcu_assign_pointer(qomci_current, b);
	return 0;
omci:
	omci_device_unregister(b->omci);
xpon:
	xpon_device_unregister(b->xpon);
cipher:
	crypto_free_lskcipher(b->ecb_cipher);
cmac:
	crypto_free_lskcipher(b->cipher);
free:
	kfree_sensitive(b);
	return ret;
}

void q1000k_omci_backend_cleanup(void)
{
	struct qomci_backend *b = rcu_access_pointer(qomci_current);

	if (!b)
		return;
	qomci_close(b);
	RCU_INIT_POINTER(qomci_current, NULL);
	synchronize_rcu();
	omci_device_unregister(b->omci);
	xpon_device_unregister(b->xpon);
	q1000k_services_destroy();
	crypto_free_lskcipher(b->ecb_cipher);
	crypto_free_lskcipher(b->cipher);
	kfree_sensitive(b);
}
