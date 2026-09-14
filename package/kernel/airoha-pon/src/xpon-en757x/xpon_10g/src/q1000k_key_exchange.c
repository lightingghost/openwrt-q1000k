// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <an7581_xpon.h>
#include "common/q1000k_key_exchange.h"
#include "common/q1000k_protocol.h"

int q1000k_key_prepare(struct crypto_lskcipher *ecb,
	struct crypto_lskcipher *cmac, const u8 kek[16],
	const struct q1000k_key_state *previous, bool confirm, u8 index,
	struct q1000k_key_update *update)
{
	struct q1000k_key_update candidate = {};
	u8 active, regen, mask;
	int ret = 0;

	if (!ecb || !cmac || ecb == cmac || !kek || !previous || !update ||
	    index < 1 || index > 2)
		return -EINVAL;
	active = previous->mac.tx_index;
	regen = previous->regenerating;
	if (active > 2 || regen > 2 || (active && active == regen))
		return -EUCLEAN;
	mask = (active ? BIT(active - 1) : 0) | (regen ? BIT(regen - 1) : 0);
	if (previous->mac.rx_valid != mask)
		return -EUCLEAN;
	candidate.next = *previous;
	candidate.report_index = index;
	if (confirm) {
		if (!(mask & BIT(index - 1))) {
			/* Invalid-key consistency inquiry: swapped index, arbitrary
			 * key name, no state change (Suppl.81 Table 7-4, AS).
			 */
			candidate.report_index = 3 - index;
			goto done;
		}
		if (regen == index) {
			candidate.next.regenerating = 0;
			candidate.next.mac.tx_index = index;
			candidate.next.mac.rx_valid = BIT(index - 1);
			memzero_explicit(candidate.next.mac.key[2 - index], 16);
			candidate.changed = true;
		}
	} else if (regen != index) {
		/* Generate on the active index is confusion recovery: retain
		 * neither that old key nor an unfinished opposite-index key.
		 * Only an opposite, already active key survives regeneration.
		 */
		if (!rng_is_initialized()) { ret = -EAGAIN; goto done; }
		candidate.next.mac.tx_index = active == index ? 0 : active;
		candidate.next.regenerating = index;
		candidate.next.mac.rx_valid = BIT(index - 1) |
			(candidate.next.mac.tx_index ? BIT(active - 1) : 0);
		if (!(candidate.next.mac.rx_valid & BIT(2 - index)))
			memzero_explicit(candidate.next.mac.key[2 - index], 16);
		get_random_bytes(candidate.next.mac.key[index - 1], 16);
		candidate.changed = true;
	}
	/* Duplicate Generate re-reports the same pending key; sequence numbers
	 * do not determine key validity. No ONU-side retransmission timers.
	 */
	ret = q1000k_auth_key_report(confirm ? cmac : ecb, kek,
		candidate.next.mac.key[index - 1], confirm, candidate.report);
done:
	if (!ret)
		*update = candidate;
	memzero_explicit(&candidate, sizeof(candidate));
	return ret;
}

static int qkey_fifo_status(u32 *status)
{
	int ret = q1000k_protocol_status();

	if (!ret) ret = an7581_xpon_status();
	if (ret) return ret;
	*status = get_xpon_data(0x5300);
	ret = an7581_xpon_status();
	if (!ret && (*status == ~0U || (*status & BIT(31)))) ret = -EIO;
	return ret;
}

int q1000k_ploam_send(const u8 message[44])
{
	u32 status;
	unsigned int i, retry;
	int ret;

	if (!q1000k_protocol_owned()) return -EPERM;
	if (!message || message[0] || message[1] || message[2] || message[3] > 1)
		return -EINVAL;
	for (retry = 0; retry < 3000; retry++) {
		ret = qkey_fifo_status(&status);
		if (ret) return ret;
		if ((status & 0xff) >= 11) break;
		udelay(1);
	}
	if (retry == 3000) return -ETIMEDOUT;
	/* All PLOAM writers share the protocol owner. FIFO reads consume data,
	 * so verify provider status and overrun, never read back WDATA.
	 */
	for (i = 0; i < 11; i++) {
		set_xpon_data(0x5304, get_unaligned_be32(message + 4 * i));
		ret = an7581_xpon_status();
		if (ret) return ret;
	}
	return qkey_fifo_status(&status);
}
