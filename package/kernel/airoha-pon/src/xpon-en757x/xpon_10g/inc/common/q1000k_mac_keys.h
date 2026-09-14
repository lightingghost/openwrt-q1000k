/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_MAC_KEYS_H_
#define _Q1000K_MAC_KEYS_H_
#include "common/q1000k_auth.h"

struct q1000k_mac_keys {
	struct q1000k_auth_keys bank[2];
	u8 pon_tag[8];
};
/* Derive bank zero from the full registration ID, bank one from the default
 * MSK. Bank one's PLOAM key is the protocol-defined 0x55 constant. Output
 * remains unchanged on failure; callers must erase returned key material.
 */
int q1000k_mac_keys_derive(struct crypto_lskcipher *tfm,
		const u8 registration[Q1000K_REGISTRATION_ID_LEN],
		const u8 serial[8], const u8 pon_tag[8], struct q1000k_mac_keys *keys);
/* Only the owning drained pipeline INSTALL callback may program key banks.
 * Preserve MAC key-index selection policy and unrelated debug fields. Both
 * OMCI directions use software MICs. Caller publishes software keys/epochs
 * only after success; a failure must poison the containing transaction.
 */
int q1000k_mac_keys_install(const struct q1000k_mac_keys *keys);
/* Read the actual hardware selectors, preserving outputs on read failure.
 * These are observations, not authorization to authenticate with a key.
 */
int q1000k_mac_key_indices(u8 *ploam, u8 *omci);
/* ONU register ownership: valid assignment 0..1022; UNASSIGNED removes it. */
int q1000k_mac_onu_install(u16 onu_id);
#endif
