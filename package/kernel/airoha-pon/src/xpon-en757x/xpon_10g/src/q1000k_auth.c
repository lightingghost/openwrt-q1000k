// SPDX-License-Identifier: GPL-2.0-only
/* ITU-T G.987.3 15.3/15.5; software CMAC never exposes a DMA buffer lifetime. */
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <crypto/algapi.h>
#include "common/q1000k_auth.h"

/* The existing synchronous AES helper serializes its shared transform. */
int gpon_aes_cmac_encrypt(struct crypto_lskcipher *tfm, const u8 *key,
			  const u8 *data, size_t data_len, u8 *mac);

int q1000k_auth_derive(struct crypto_lskcipher *tfm, const u8 msk[16],
		       const u8 serial[8], const u8 pon_tag[8],
		       struct q1000k_auth_keys *keys)
{
	struct q1000k_auth_keys result = {};
	u8 message[24];
	int ret;

	if (!tfm || !msk || !serial || !pon_tag || !keys)
		return -EINVAL;
	memcpy(result.msk, msk, sizeof(result.msk));
	memcpy(message, serial, 8);
	memcpy(message + 8, pon_tag, 8);
	memcpy(message + 16, "SessionK", 8);
	ret = gpon_aes_cmac_encrypt(tfm, msk, message, sizeof(message), result.session);
	if (!ret)
		ret = gpon_aes_cmac_encrypt(tfm, result.session,
			(const u8 *)"OMCIIntegrityKey", 16, result.omci);
	/* The normative hexadecimal label is PLOAMIntegrtyKey (16 bytes),
	 * including that spelling; the published golden vector confirms it.
	 */
	if (!ret)
		ret = gpon_aes_cmac_encrypt(tfm, result.session,
			(const u8 *)"PLOAMIntegrtyKey", 16, result.ploam);
	if (!ret)
		ret = gpon_aes_cmac_encrypt(tfm, result.session,
			(const u8 *)"KeyEncryptionKey", 16, result.kek);
	if (!ret)
		*keys = result;
	memzero_explicit(&result, sizeof(result));
	memzero_explicit(message, sizeof(message));
	return ret;
}

int q1000k_auth_registration(struct crypto_lskcipher *tfm,
			    const u8 registration[Q1000K_REGISTRATION_ID_LEN],
			    const u8 serial[8], const u8 pon_tag[8],
			    struct q1000k_auth_keys *keys)
{
	static const u8 default_key[16] = { [0 ... 15] = 0x55 };
	u8 msk[16] = {};
	int ret;

	if (!tfm || !registration || !serial || !pon_tag || !keys)
		return -EINVAL;
	ret = gpon_aes_cmac_encrypt(tfm, default_key, registration,
				    Q1000K_REGISTRATION_ID_LEN, msk);
	if (!ret)
		ret = q1000k_auth_derive(tfm, msk, serial, pon_tag, keys);
	memzero_explicit(msk, sizeof(msk));
	return ret;
}

int q1000k_auth_ploam_verify(struct crypto_lskcipher *tfm, const u8 key[16],
			    const u8 *message, size_t length)
{
	u8 digest[16] = {}, authenticated[41];
	int ret;

	if (!tfm || !key || !message)
		return -EINVAL;
	if (length != 48)
		return -EMSGSIZE;
	/* G.9807.1 C.15.6.2: direction byte followed by all 40 body bytes. */
	authenticated[0] = 0x01;
	memcpy(authenticated + 1, message, 40);
	ret = gpon_aes_cmac_encrypt(tfm, key, authenticated,
				    sizeof(authenticated), digest);
	if (!ret && crypto_memneq(digest, message + 40, 8))
		ret = -EBADMSG;
	memzero_explicit(digest, sizeof(digest));
	memzero_explicit(authenticated, sizeof(authenticated));
	return ret;
}

int q1000k_auth_omci_mic(struct crypto_lskcipher *tfm, const u8 key[16],
			const struct sk_buff *skb, bool has_mic, u8 direction,
			u8 mic[4])
{
	u8 header[10], trailer[4], digest[16] = {};
	u8 *message;
	unsigned int payload;
	int ret;

	if (!tfm || !key || !skb || !mic ||
	    (direction != Q1000K_OMCI_DOWNSTREAM && direction != Q1000K_OMCI_UPSTREAM))
		return -EINVAL;
	if (skb->len < sizeof(header) || skb->len > Q1000K_OMCI_MAX_LEN ||
	    skb_copy_bits(skb, 0, header, sizeof(header)))
		return -EMSGSIZE;
	if (header[3] == 0x0a) {
		payload = 44;
		if (skb_copy_bits(skb, 40, trailer, sizeof(trailer)) ||
		    memcmp(trailer, "\0\0\0\x28", 4))
			return -EBADMSG;
	} else if (header[3] == 0x0b) {
		payload = 10 + ((unsigned int)header[8] << 8) + header[9];
	} else {
		return -EPROTONOSUPPORT;
	}
	if (payload > Q1000K_OMCI_MAX_LEN - 4 ||
	    skb->len != payload + (has_mic ? 4 : 0))
		return -EMSGSIZE;
	message = kmalloc(payload + 1, GFP_ATOMIC);
	if (!message)
		return -ENOMEM;
	message[0] = direction;
	ret = skb_copy_bits(skb, 0, message + 1, payload);
	if (!ret)
		ret = gpon_aes_cmac_encrypt(tfm, key, message, payload + 1, digest);
	if (!ret)
		memcpy(mic, digest, 4);
	memzero_explicit(digest, sizeof(digest));
	kfree_sensitive(message);
	return ret;
}

int q1000k_auth_omci_verify(struct crypto_lskcipher *tfm, const u8 key[16],
			   const struct sk_buff *skb)
{
	u8 expected[4], received[4];
	int ret;

	ret = q1000k_auth_omci_mic(tfm, key, skb, true,
				  Q1000K_OMCI_DOWNSTREAM, expected);
	if (ret)
		return ret;
	ret = skb_copy_bits(skb, skb->len - 4, received, sizeof(received));
	if (!ret && crypto_memneq(expected, received, sizeof(received)))
		ret = -EBADMSG;
	memzero_explicit(expected, sizeof(expected));
	memzero_explicit(received, sizeof(received));
	return ret;
}
