// SPDX-License-Identifier: GPL-2.0-only
/* Verified XG PLOAM/OMCI integrity and key-encryption bank programming. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <an7581_xpon.h>
#include "common/q1000k_mac_keys.h"
#include "common/q1000k_pipeline.h"

#define QMAC_PIK0 0x5360
#define QMAC_OIK0 0x5380
#define QMAC_KEK0 0x53a0
#define QMAC_PON_TAG0 0x53c0
#define QMAC_PON_TAG1 0x53c4
#define QMAC_CUR_KIDX 0x5318
#define QMAC_CAP_SETTING 0x5800
#define QMAC_HW_OMCI_MIC (BIT(3) | BIT(4))

int q1000k_mac_keys_derive(struct crypto_lskcipher *tfm,
		const u8 registration[Q1000K_REGISTRATION_ID_LEN],
		const u8 serial[8], const u8 pon_tag[8], struct q1000k_mac_keys *keys)
{
	struct q1000k_mac_keys *candidate;
	u8 default_msk[16];
	int ret;

	if (!tfm || !registration || !serial || !pon_tag || !keys)
		return -EINVAL;
	candidate = kzalloc(sizeof(*candidate), GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	memset(default_msk, 0x55, sizeof(default_msk));
	ret = q1000k_auth_registration(tfm, registration, serial, pon_tag, &candidate->bank[0]);
	if (!ret)
		ret = q1000k_auth_derive(tfm, default_msk, serial, pon_tag, &candidate->bank[1]);
	if (!ret) {
		memset(candidate->bank[1].ploam, 0x55, sizeof(candidate->bank[1].ploam));
		memcpy(candidate->pon_tag, pon_tag, sizeof(candidate->pon_tag));
		*keys = *candidate;
	}
	memzero_explicit(default_msk, sizeof(default_msk));
	kfree_sensitive(candidate);
	return ret;
}

static int q1000k_mac_key_write(u32 reg, u32 value)
{
	u32 actual;
	int ret;

	set_xpon_data(reg, value);
	actual = get_xpon_data(reg);
	ret = an7581_xpon_status();
	/* All-ones is a legitimate 32-bit key word. The provider's read fault
	 * and exact comparison, not a sentinel test on key bytes, decide this.
	 */
	return ret ? ret : actual != value ? -EIO : 0;
}

static int q1000k_mac_key_bank(u32 base, const u8 key[16])
{
	unsigned int i;
	int ret;

	/* Hardware word zero contains the final four network-order key bytes. */
	for (i = 0; i < 4; i++) {
		ret = q1000k_mac_key_write(base + 4 * i, get_unaligned_be32(key + 12 - 4 * i));
		if (ret)
			return ret;
	}
	return 0;
}

int q1000k_mac_keys_install(const struct q1000k_mac_keys *keys)
{
	u32 config;
	unsigned int bank, i;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	if (!keys)
		return -EINVAL;
	for (i = 0; i < 16; i++)
		if (keys->bank[1].ploam[i] != 0x55)
			return -EINVAL;
	ret = an7581_xpon_status();
	if (ret)
		return ret;
	config = get_xpon_data(QMAC_CAP_SETTING);
	ret = an7581_xpon_status();
	if (ret || config == ~0U)
		return ret ?: -EIO;
	ret = q1000k_mac_key_write(QMAC_CAP_SETTING, config & ~QMAC_HW_OMCI_MIC);
	if (!ret)
		ret = q1000k_mac_key_write(QMAC_PON_TAG0, get_unaligned_be32(keys->pon_tag + 4));
	if (!ret)
		ret = q1000k_mac_key_write(QMAC_PON_TAG1, get_unaligned_be32(keys->pon_tag));
	if (ret)
		return ret;
	for (bank = 0; bank < 2; bank++) {
		ret = q1000k_mac_key_bank(QMAC_PIK0 + 16 * bank, keys->bank[bank].ploam);
		if (!ret)
			ret = q1000k_mac_key_bank(QMAC_OIK0 + 16 * bank, keys->bank[bank].omci);
		if (!ret)
			ret = q1000k_mac_key_bank(QMAC_KEK0 + 16 * bank, keys->bank[bank].kek);
		if (ret)
			return ret;
	}
	return 0;
}

int q1000k_mac_key_indices(u8 *ploam, u8 *omci)
{
	u32 value;
	int ret;

	if (!ploam || !omci)
		return -EINVAL;
	ret = an7581_xpon_status();
	if (ret)
		return ret;
	value = get_xpon_data(QMAC_CUR_KIDX);
	ret = an7581_xpon_status();
	if (ret || value == ~0U)
		return ret ?: -EIO;
	*ploam = !!(value & BIT(0));
	*omci = !!(value & BIT(16));
	return 0;
}

int q1000k_mac_onu_install(u16 onu_id)
{
	u32 value;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	if (onu_id > 1020 && onu_id != 0xffff)
		return -EINVAL;
	ret = an7581_xpon_status();
	if (ret)
		return ret;
	value = get_xpon_data(0x5014);
	ret = an7581_xpon_status();
	if (ret || value == ~0U)
		return ret ?: -EIO;
	value &= ~(BIT(15) | 0x3ff);
	value |= onu_id == 0xffff ? 0x3ff : BIT(15) | onu_id;
	return q1000k_mac_key_write(0x5014, value);
}
