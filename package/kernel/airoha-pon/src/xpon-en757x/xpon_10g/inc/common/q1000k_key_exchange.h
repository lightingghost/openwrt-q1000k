/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_KEY_EXCHANGE_H_
#define _Q1000K_KEY_EXCHANGE_H_
#include "q1000k_mac_keys.h"

struct q1000k_key_state {
	struct q1000k_mac_data_keys mac;
	u8 regenerating; /* Wire index 1/2, or zero when no handshake is pending. */
};
struct q1000k_key_update {
	struct q1000k_key_state next;
	u8 report[32], report_index;
	bool changed;
};
/* G.9807.1 15.5.3 and G Suppl.81 7.2.5. Pure software preparation;
 * output is unchanged on failure. Caller publishes only after a checked
 * physical install and report submission. Transforms must be distinct.
 */
int q1000k_key_prepare(struct crypto_lskcipher *ecb,
	struct crypto_lskcipher *cmac, const u8 kek[16],
	const struct q1000k_key_state *previous, bool confirm, u8 index,
	struct q1000k_key_update *update);
/* Protocol owner only. Input is the MAC's 4-byte integrity-key selector
 * followed by the 40-byte wire body. Hardware appends the upstream MIC.
 * No key or message contents are logged. A partial write is fatal.
 */
int q1000k_ploam_send(const u8 message[44]);
#endif
