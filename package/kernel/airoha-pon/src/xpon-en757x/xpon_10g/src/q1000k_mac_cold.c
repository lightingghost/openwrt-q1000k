#include <q1000k_trace.h>
// SPDX-License-Identifier: GPL-2.0-only
/* AN7581 XGS discovery baseline, from the EN7581 vendor register definitions. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/unaligned.h>
#include <an7581_xpon.h>
#include "common/q1000k_mac_cold.h"
#include "common/q1000k_rx_bench.h"
#include "common/q1000k_pipeline.h"
#include "common/q1000k_protocol.h"

/* Serialized by the protocol executor. Each cold install follows a MAC
 * reset; profile refreshes retain this counter epoch. */
static u32 qcold_generation;
u32 q1000k_mac_generation(void) { return qcold_generation; }

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

int q1000k_mac_ranging_install(u32 delay)
{
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	if (delay > (~0U >> 2))
		return -ERANGE;
	/* AN7581 XGS EqD units are four times the OLT value. A complete
	 * resynchronization uses the absolute value, without incremental wrap.
	 * Profile validity and all physical producers are closed by the owner.
	 */
	ret = qcold_write(0x5114, delay << 2, ~0U);
	if (!ret)
		ret = qcold_modify(0x582c, BIT(8) | BIT(0), BIT(8) | BIT(0),
			BIT(31), ~(u32)(BIT(31) | BIT(0)));
	return ret;
}

int q1000k_mac_ranging_ready(void)
{
	u32 value;
	unsigned int retry;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_ACTIVATE);

	if (ret)
		return ret;
	for (retry = 0; retry < 3000; retry++) {
		value = get_xpon_data(0x582c);
		ret = an7581_xpon_status();
		if (ret || value == ~0U)
			return ret ?: -EIO;
		if (value & BIT(31))
			return qcold_modify(0x582c, BIT(8) | BIT(0), 0, BIT(31), ~(u32)BIT(31));
		udelay(1);
	}
	return -ETIMEDOUT;
}

int q1000k_mac_ranging_bench(u32 delay, unsigned int mode)
{
	u32 profiles, state, stop, value;
	unsigned int retry;
	int ret;

	if (!q1000k_protocol_owned()) return -EPERM;
	if (!mode || mode > 3 || delay > (~0U >> 2)) return -EINVAL;
	state = get_xpon_data(0x5104);
	stop = get_xpon_data(0x5004);
	profiles = get_xpon_data(0x511c);
	ret = an7581_xpon_status();
	if (ret) return ret;
	/* This experiment never adjusts an operational link or replaces IDs,
	 * tables or keys. The backend also verifies unchanged live profiles.
	 */
	if (state == ~0U || stop == ~0U || profiles == ~0U) return -EIO;
	if ((state & 15) != 4 || (stop & 0x01010101) ||
	    !(profiles & 0x01010101)) return -EAGAIN;
	q1000k_trace(QT_CONTROL, 6, 0, delay, delay << 2, mode, profiles);
	q1000k_activation_snapshot(2, mode);
	if (mode == 3) {
		/* NAND xpon_10g.ko: O4 handler 0x9568 -> 0x98c4 calls
		 * gponDevSetEqdValue (0x17c00), then publishes O5 at 0x99fc.
		 * XGS shifts by two at 0x17c78; there is no O4 SW-resync call.
		 */
		ret = qcold_write(0x5114, delay << 2, ~0U);
		goto out;
	}
	/* The NAND resync helper pauses only upstream MBI/MPI and profiles.
	 * Preserve downstream RX, PHY clocks, controller state and DMA epochs.
	 * Wait for FIFO drain before stopping its output; no namespace reuse.
	 */
	ret = an7581_xpon_mac_stop(AN7581_XPON_MBI_TX_STOP, true);
	if (!ret) ret = qcold_write(0x511c, profiles & ~0x01010101U, ~0U);
	if (!ret) ret = an7581_xpon_mac_wait_tx_empty();
	if (!ret) ret = an7581_xpon_mac_stop(AN7581_XPON_MPI_TX_STOP, true);
	if (!ret) ret = qcold_write(0x5114, delay << 2, ~0U);
	if (!ret) ret = qcold_modify(0x582c, BIT(8) | BIT(0), BIT(8) | BIT(0),
		BIT(31), ~(u32)(BIT(31) | BIT(0)));
	if (!ret) ret = an7581_xpon_mac_stop(AN7581_XPON_MPI_TX_STOP, false);
	if (ret) goto out;
	for (retry = 0; retry < 3000; retry++) {
		value = get_xpon_data(0x582c);
		ret = an7581_xpon_status();
		if (ret || value == ~0U) { ret = ret ?: -EIO; goto out; }
		if (value & BIT(31)) break;
		udelay(1);
	}
	if (retry == 3000) { ret = -ETIMEDOUT; goto out; }
	if (mode == 1)
		ret = qcold_modify(0x582c, BIT(8) | BIT(0), 0, BIT(31), ~(u32)BIT(31));
	if (!ret) ret = qcold_write(0x511c, profiles, ~0U);
	if (!ret) ret = an7581_xpon_mac_stop(AN7581_XPON_MBI_TX_STOP, false);
out:
	q1000k_trace(QT_CONTROL, 7, ret, delay, get_xpon_data(0x5114), mode,
		get_xpon_data(0x582c));
	q1000k_activation_snapshot(3, mode);
	/* The owner contains any error. Never reopen uncertain timing here. */
	return ret ?: an7581_xpon_status();
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
	qcold_generation++;
	/* All-ones can be legitimate identity data; exact readback is required. */
	ret = qcold_write(0x500c, get_unaligned_be32(serial), ~0U);
	if (!ret)
		ret = qcold_write(0x5010, get_unaligned_be32(serial + 4), ~0U);
	for (i = 0; !ret && i < 9; i++)
		ret = qcold_write(0x5018 + 4 * i, get_unaligned_be32(registration + 32 - 4 * i), ~0U);
	q1000k_trace(QT_MAC_CONFIG, 0, ret, !ret, 0, 0, 0);
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
	q1000k_trace(QT_STATE, state, 0, 0, 0, 0, 0);
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
	return ret ?: qcold_write(0x5040, q1000k_rx_bench_enabled() ? 0 : enables, ~0U);
}

int q1000k_mac_profiles_invalidate(void)
{
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	return ret ?: qcold_update(0x511c, 0x01010101, 0, 0);
}

int q1000k_mac_profile_matches(u8 index, u8 version, u16 length)
{
 u32 info, size;
 int ret;
 if (!q1000k_protocol_owned()) return -EPERM;
 if (index > 3 || version > 15 || !length) return -EINVAL;
 info = get_xpon_data(0x511c);
 size = get_xpon_data(0x5120 + 4 * (index / 2));
 ret = an7581_xpon_status();
 if (ret || info == ~0U || size == ~0U) return ret ?: -EIO;
 return ((info >> (index * 8)) & 0xf1) == ((u32)version << 4 | 1) &&
  ((size >> ((index & 1) * 16)) & 0xffff) == length;
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
	if (!ret) ret = qcold_update(0x511c, 0xf1U << shift, ((u32)version << 4 | 1) << shift, 0);
	q1000k_trace(QT_MAC_PROFILE, index, ret, version, length, 0, 0);
	return ret;
}
