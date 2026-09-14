/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_AUTH_H_
#define _Q1000K_AUTH_H_

#include <linux/types.h>
struct crypto_lskcipher;
struct sk_buff;

#define Q1000K_REGISTRATION_ID_LEN 36
#define Q1000K_OMCI_MAX_LEN 1980
#define Q1000K_OMCI_DOWNSTREAM 1
#define Q1000K_OMCI_UPSTREAM 2

struct q1000k_auth_keys {
	u8 msk[16];
	u8 session[16];
	u8 omci[16];
	u8 ploam[16];
	u8 kek[16];
};

/* Caller owns the transform's lifetime. Output is unchanged on failure.
 * Temporary keys are erased; caller must erase its returned key set as well.
 * These helpers derive/verify bytes, never establish a registration session,
 * authenticate a descriptor flag, or program hardware keys.
 */
int q1000k_auth_derive(struct crypto_lskcipher *tfm, const u8 msk[16],
		       const u8 serial[8], const u8 pon_tag[8],
		       struct q1000k_auth_keys *keys);
int q1000k_auth_registration(struct crypto_lskcipher *tfm,
			    const u8 registration[Q1000K_REGISTRATION_ID_LEN],
			    const u8 serial[8], const u8 pon_tag[8],
			    struct q1000k_auth_keys *keys);
/* Compute/verify full baseline or extended PDU, including baseline trailer.
 * has_mic selects an exact additional 4-byte MIC; unknown trailing bytes fail.
 * Scattered skbs are copied safely. No skb mutation or DMA operation occurs.
 */
int q1000k_auth_omci_mic(struct crypto_lskcipher *tfm, const u8 key[16],
			const struct sk_buff *skb, bool has_mic, u8 direction,
			u8 mic[4]);
int q1000k_auth_omci_verify(struct crypto_lskcipher *tfm, const u8 key[16],
			   const struct sk_buff *skb);
/* Exactly 40 PLOAM message bytes followed by the 8-byte MIC. Any device
 * FIFO trailer is outside this authenticated wire message and must be omitted.
 */
int q1000k_auth_ploam_verify(struct crypto_lskcipher *tfm, const u8 key[16],
			    const u8 *message, size_t length);
#endif
