// SPDX-License-Identifier: GPL-2.0-only
/* AN7581 XGS discovery baseline, from the EN7581 vendor register definitions. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/unaligned.h>
#include <an7581_xpon.h>
#include "common/q1000k_mac_cold.h"
#include "common/q1000k_pipeline.h"
#include "common/q1000k_protocol.h"

/* Do not echo read-only status or command bits back into mixed registers.
 * readback_mask excludes only unrelated fields that hardware may change.
 */
static int qcold_write(u32 reg, u32 value, u32 readback_mask)
{
	u32 actual;
	int ret;

	set_xpon_data(reg, value);
	actual = get_xpon_data(reg);
	ret = an7581_xpon_status();
	return ret ? ret : ((actual ^ value) & readback_mask) ? -EIO : 0;
}

static int qcold_modify(u32 reg, u32 mask, u32 value, u32 omit, u32 verify)
{
	u32 old;
	int ret = an7581_xpon_status();

	if (ret)
		return ret;
	old = get_xpon_data(reg);
	ret = an7581_xpon_status();
	if (ret || old == ~0U)
		return ret ?: -EIO;
	return qcold_write(reg, (old & ~(mask | omit)) | value, verify);
}

static int qcold_update(u32 reg, u32 mask, u32 value, u32 omit)
{
	return qcold_modify(reg, mask, value, omit, mask);
}

int q1000k_mac_cold_release(void)
{
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR);

	/* Hardware table engines require the MAC's local reset released. The
	 * separate MPI/MBI stop controls still exclude all physical transfers.
	 */
	return ret ?: qcold_update(0x5000, BIT(0), BIT(0), 0);
}

int q1000k_mac_cold_install(const u8 serial[8], const u8 registration[36], bool emergency)
{
	static const struct { u32 reg, mask, value, omit; } defaults[] = {
		{ 0x5100, BIT(0), 0, 0 }, /* Hardware O2/3 and O4 PLOAM replies. */
		{ 0x5108, 0x3fff, 0x1600, 0 }, /* XGS response time. */
		{ 0x5868, 3, 3, BIT(2) }, /* Force downstream FEC; OC status is RO. */
		{ 0x5800, BIT(8) | BIT(7) | 0x1b, BIT(8) | BIT(7), 0 },
		/* No duplicate PLOAM filters or hardware OMCI MIC; O5.2 idle only. */
		{ 0x5284, 0xfffff011, (255U << 12) | BIT(0), BIT(8) },
		/* AN7581 uses 20 count bits at 12, SW trigger at 4 (not at 16). */
		{ 0x52f0, BIT(0), 0, 0 }, /* Software validates PLOAM MIC. */
		{ 0x582c, BIT(12) | BIT(8) | BIT(0), 0, BIT(31) },
		{ 0x5280, 0xffff, 0x120, 0 },
		{ 0x5500, BIT(8), BIT(8), 0 }, /* Ethernet MIB accounting. */
		{ 0x5200, BIT(31) | BIT(0), 0, 0 }, /* No valid upstream AES key. */
		{ 0x5204, 0xf, 0, 0 }, /* No valid downstream AES bank. */
		{ 0x511c, 0x11111111, 0, 0 }, /* Retire all four burst profiles. */
	};
	unsigned int i;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	if (!serial || !registration)
		return -EINVAL;
	ret = an7581_xpon_status();
	if (ret)
		return ret;
	/* All-ones can be legitimate identity data; exact readback is required. */
	ret = qcold_write(0x500c, get_unaligned_be32(serial), ~0U);
	if (!ret)
		ret = qcold_write(0x5010, get_unaligned_be32(serial + 4), ~0U);
	for (i = 0; !ret && i < 9; i++)
		ret = qcold_write(0x5018 + 4 * i, get_unaligned_be32(registration + 32 - 4 * i), ~0U);
	for (i = 0; !ret && i < sizeof(defaults) / sizeof(defaults[0]); i++)
		ret = qcold_update(defaults[i].reg, defaults[i].mask, defaults[i].value, defaults[i].omit);
	if (!ret)
		ret = qcold_write(0x5114, 0, ~0U); /* Old ranging delay must not survive. */
	if (!ret)
		ret = qcold_update(0x5014, BIT(15) | 0x3ff, 0x3ff, 0); /* ONU invalid. */
	if (!ret)
		ret = qcold_update(0x5104, 0xf, emergency ? 7 : 1, 0);
	return ret;
}

int q1000k_mac_cold_select_keys(void)
{
	unsigned int retry;
	u32 value;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	/* Command-enable bits may clear after selection. Verify the requested
	 * indices here and the resulting hardware indices below.
	 */
	ret = qcold_modify(0x53e8, 0x01010101, 0x01010101, 0, 0x101);
	if (ret)
		return ret;
	/* Select only after bank-one PLOAM=0x55 and both integrity banks exist. */
	for (retry = 0; retry < 3000; retry++) {
		value = get_xpon_data(0x5318);
		ret = an7581_xpon_status();
		if (ret || value == ~0U)
			return ret ?: -EIO;
		if ((value & 0x00010001) == 0x00010001)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

int q1000k_mac_activation_set(u8 state)
{
	if (!q1000k_protocol_owned())
		return -EPERM;
	if (state != 1 && state != 2 && state != 4 && state != 5 && state != 7)
		return -EOPNOTSUPP;
	return qcold_update(0x5104, 0xf, state, 0);
}

int q1000k_mac_cold_interrupts(u32 enables)
{
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	ret = an7581_xpon_status();
	if (ret)
		return ret;
	/* W1C status is not an ordinary readback register. Source enable is. */
	set_xpon_data(0x5044, ~0U);
	ret = an7581_xpon_status();
	return ret ?: qcold_write(0x5040, enables, ~0U);
}

int q1000k_mac_profiles_invalidate(void)
{
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	return ret ?: qcold_update(0x511c, 0x01010101, 0, 0);
}

int q1000k_mac_profile_install(u8 index, u8 version, u16 length)
{
	u32 shift;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	if (index > 3 || version > 15 || !length)
		return -EINVAL;
	shift = (index & 1) * 16;
	ret = qcold_update(0x5120 + 4 * (index / 2), 0xffffU << shift, (u32)length << shift, 0);
	/* Publish validity only after the caller verified the PHY and length. */
	shift = index * 8;
	return ret ?: qcold_update(0x511c, 0xf1U << shift, ((u32)version << 4 | 1) << shift, 0);
}
