// SPDX-License-Identifier: GPL-2.0-only
/* Minimal EN7573 MD32 transport. No vendor kernel APIs or background tasks.
 * The caller verifies firmware hashes and supplies this unit's calibration.
 * Every write, readback and control transition can fail; abort immediately.
 */
#include "en7573.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

int en7573_rx_power(struct en7573_io *io, u32 *nanowatts)
{
	u8 data[2];
	u32 raw;
	int ret;

	if (!io || !io->read || !nanowatts)
		return -EINVAL;
	ret = io->read(io->ctx, EN7573_CONTROL, 0x0068, data, sizeof(data));
	if (ret)
		return ret;
	raw = (u32)data[0] << 8 | data[1];
	if (!raw || raw == 0xffff)
		return -ENODATA;
	*nanowatts = raw * 100;
	return 0;
}

static u32 get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int en7573_read_control(struct en7573_io *io, u16 reg, u32 *value)
{
	u8 data[4];
	int ret = io->read(io->ctx, EN7573_CONTROL, reg, data, sizeof(data));
	if (!ret)
		*value = get_le32(data);
	return ret;
}

int en7573_sample_state(struct en7573_io *io, struct en7573_state *state)
{
	u32 mcu, tx;
	int ret;

	state->md32_enabled = -1;
	state->tx_disabled = -1;
	ret = en7573_read_control(io, EN7573_MCU_ENABLE, &mcu);
	if (ret)
		return ret;
	ret = en7573_read_control(io, EN7573_TX_CONTROL, &tx);
	if (ret)
		return ret;
	if (mcu == ~0U || tx == ~0U)
		return -EIO;
	state->md32_enabled = !!(mcu & 1);
	state->tx_disabled = !!(tx & EN7573_TX_DISABLE);
	return 0;
}

int en7573_sample_receiver(struct en7573_io *io, struct en7573_receiver *sample)
{
	/* OEM QKX001-06.00.44.00 uses A0 for MD32 control; the public loader
	 * uses A2. Observe both without changing either access path. The other
	 * registers are ordinary control/status words read by the OEM debug
	 * routines. No PM/DM data port, address selector, alarm clear, or FIFO.
	 */
	static const u16 registers[] = { 0x3018, 0x3018, 0x15c, 0x160,
					0x80, 0x43c, 0x488 };
	u32 values[7];
	u8 data[4];
	unsigned int i;
	int ret;

	for (i = 0; i < 7; i++) {
		ret = io->read(io->ctx, i ? EN7573_CONTROL : EN7573_MEMORY,
			       registers[i], data, sizeof(data));
		if (ret)
			return ret;
		values[i] = get_le32(data);
		if (values[i] == ~0U)
			return -EIO;
	}
	*sample = (struct en7573_receiver) {
		.mcu_a0 = values[0], .mcu_a2 = values[1], .apd = values[2],
		.ocp = values[3], .firmware = values[4], .los_control = values[5],
		.system_status = values[6],
	};
	return 0;
}

static int write_control(struct en7573_io *io, u16 reg, u32 value)
{
	u8 data[] = { value, value >> 8, value >> 16, value >> 24 };
	return io->write(io->ctx, EN7573_CONTROL, reg, data, sizeof(data));
}

static int update_control(struct en7573_io *io, u16 reg, u32 mask, u32 value)
{
	u32 old;
	int ret = en7573_read_control(io, reg, &old);
	return ret ? ret : write_control(io, reg, (old & ~mask) | (value & mask));
}

int en7573_set_tx(struct en7573_io *io, bool enable)
{
	u32 old, actual, expected;
	int ret = en7573_read_control(io, EN7573_TX_CONTROL, &old);

	if (ret)
		return ret;
	if (old == ~0U)
		return -EIO;
	expected = enable ? old & ~EN7573_TX_DISABLE : old | EN7573_TX_DISABLE;
	ret = write_control(io, EN7573_TX_CONTROL, expected);
	if (!ret)
		ret = en7573_read_control(io, EN7573_TX_CONTROL, &actual);
	return ret ? ret : actual == ~0U ||
		!!(actual & EN7573_TX_DISABLE) == enable ? -EIO : 0;
}

int en7573_identify(struct en7573_io *io, u16 *id)
{
	u8 data[2];
	int ret = io->read(io->ctx, EN7573_CONTROL, 0x408, data, sizeof(data));
	*id = 0;
	if (ret)
		return ret;
	*id = data[0] | ((u16)data[1] << 8);
	return *id == EN7573_ID ? 0 : -ENODEV;
}

static void expected_word(u8 word[4], const u8 *data, size_t size,
			  size_t offset, const u8 *cal)
{
	memset(word, 0, 4);
	if (offset < size)
		memcpy(word, data + offset, size - offset < 4 ? size - offset : 4);
	if (cal && offset >= EN7573_CAL_ADDRESS &&
	    offset < EN7573_CAL_ADDRESS + EN7573_CAL_SIZE)
		memcpy(word, cal + offset - EN7573_CAL_ADDRESS, 4);
}

static int memory_load(struct en7573_io *io, u16 cfg, u16 address, u16 port,
		       const u8 *data, size_t size, size_t capacity, const u8 *cal)
{
	u8 word[4], actual[4];
	size_t offset;
	int ret;

	ret = update_control(io, cfg, 1, 1);
	if (ret)
		return ret;
	ret = write_control(io, address, 0);
	if (ret)
		return ret;
	/* The write port auto-increments by a word. Write all padding as well. */
	for (offset = 0; offset < capacity; offset += 4) {
		expected_word(word, data, size, offset, cal);
		ret = io->write(io->ctx, EN7573_MEMORY, port, word, sizeof(word));
		if (ret)
			return ret;
	}
	/* Set each read address explicitly; do not assume read auto-increment. */
	for (offset = 0; offset < capacity; offset += 4) {
		ret = write_control(io, address, offset);
		if (ret)
			return ret;
		ret = io->read(io->ctx, EN7573_MEMORY, port, actual, sizeof(actual));
		if (ret)
			return ret;
		expected_word(word, data, size, offset, cal);
		if (memcmp(word, actual, sizeof(word)))
			return -EBADMSG;
	}
	return 0;
}

int en7573_load(struct en7573_io *io, const u8 *pm, size_t pm_size,
		const u8 *dm, size_t dm_size, const u8 *cal)
{
	u16 id;
	u32 state;
	int ret;
	if (!pm || !pm_size || pm_size > EN7573_PM_SIZE || !dm ||
	    !dm_size || dm_size > EN7573_CAL_ADDRESS || !cal)
		return -EINVAL;
	ret = en7573_identify(io, &id);
	if (ret)
		return ret;
	/* Hold transmission disabled throughout reset, download and MCU startup. */
	ret = update_control(io, EN7573_TX_CONTROL, EN7573_TX_DISABLE, EN7573_TX_DISABLE);
	if (ret)
		return ret;
	ret = update_control(io, EN7573_MCU_ENABLE, 1, 0);
	if (ret)
		return ret;
	ret = update_control(io, 0x160, 1U << 30, 0); /* OCP, OEM reset sequence */
	if (ret)
		return ret;
	ret = update_control(io, 0x15c, 1U << 8, 0); /* APD */
	if (ret)
		return ret;
	io->delay_ms(io->ctx, 100);
	ret = update_control(io, 0x200, 3U << 30, 0);
	if (ret)
		return ret;
	ret = update_control(io, 0x200, 3U << 30, 3U << 30);
	if (ret)
		return ret;
	ret = update_control(io, EN7573_TX_CONTROL, EN7573_TX_DISABLE, EN7573_TX_DISABLE);
	if (ret)
		return ret;
	ret = en7573_read_control(io, EN7573_MCU_ENABLE, &state);
	if (ret || (state & 1))
		return ret ? ret : -EIO;
	ret = memory_load(io, 0x3000, 0x3004, 0x3008, pm, pm_size, EN7573_PM_SIZE, NULL);
	if (ret)
		return ret;
	return memory_load(io, 0x300c, 0x3010, 0x3014, dm, dm_size, EN7573_DM_SIZE, cal);
}

int en7573_start_tx_disabled(struct en7573_io *io)
{
	u32 value;
	int ret = update_control(io, EN7573_TX_CONTROL, EN7573_TX_DISABLE, EN7573_TX_DISABLE);
	if (ret)
		return ret;
	ret = update_control(io, EN7573_MCU_ENABLE, 1, 1);
	if (ret)
		return ret;
	io->delay_ms(io->ctx, 500);
	ret = en7573_read_control(io, EN7573_TX_CONTROL, &value);
	if (ret || !(value & EN7573_TX_DISABLE))
		return ret ? ret : -EIO;
	ret = en7573_read_control(io, EN7573_MCU_ENABLE, &value);
	return ret ? ret : (value & 1) ? 0 : -EIO;
}
