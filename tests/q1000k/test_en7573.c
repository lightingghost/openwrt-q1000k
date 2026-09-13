// SPDX-License-Identifier: GPL-2.0-only
/* Compile together with the real portable loader; no OEM firmware needed. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "en7573.h"

struct model {
	u32 regs[0x4000 / 4];
	u8 pm[EN7573_PM_SIZE], dm[EN7573_DM_SIZE];
	unsigned int calls, fail_at, readbacks;
	bool corrupt, wrong_id, start_seen;
};

static u32 unpack(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static int transfer(struct model *m)
{
	return ++m->calls == m->fail_at ? -EREMOTEIO : 0;
}

static int rd(void *ctx, u8 dev, u16 reg, u8 *buf, size_t len)
{
	struct model *m = ctx;
	u32 value;
	unsigned int i;
	int ret = transfer(m);
	if (ret) return ret;
	if (reg == 0x408) {
		assert(dev == 0x51 && len == 2);
		buf[0] = m->wrong_id ? 0x77 : 0x88; buf[1] = 0x13;
		return 0;
	}
	assert(len == 4);
	if (dev == 0x50) {
		assert(reg == 0x3008 || reg == 0x3014);
		bool pm = reg == 0x3008;
		u32 addr = m->regs[(pm ? 0x3004 : 0x3010) / 4];
		assert(addr <= (pm ? sizeof(m->pm) : sizeof(m->dm)) - 4);
		memcpy(buf, (pm ? m->pm : m->dm) + addr, 4);
		if (m->corrupt) buf[0] ^= 1;
		m->readbacks++;
		return 0;
	}
	assert(dev == 0x51 && !(reg & 3) && reg < 0x4000);
	value = m->regs[reg / 4];
	for (i = 0; i < 4; i++) buf[i] = value >> (8 * i);
	return 0;
}

static int wr(void *ctx, u8 dev, u16 reg, const u8 *buf, size_t len)
{
	struct model *m = ctx;
	int ret = transfer(m);
	if (ret) return ret;
	assert(len == 4);
	if (dev == 0x50) {
		assert(reg == 0x3008 || reg == 0x3014);
		bool pm = reg == 0x3008;
		u32 *addr = &m->regs[(pm ? 0x3004 : 0x3010) / 4];
		assert(!(m->regs[EN7573_MCU_ENABLE / 4] & 1));
		assert(m->regs[EN7573_TX_CONTROL / 4] & EN7573_TX_DISABLE);
		assert(m->regs[(pm ? 0x3000 : 0x300c) / 4] & 1);
		assert(*addr <= (pm ? sizeof(m->pm) : sizeof(m->dm)) - 4);
		memcpy((pm ? m->pm : m->dm) + *addr, buf, 4);
		*addr += 4;
		return 0;
	}
	assert(dev == 0x51 && !(reg & 3) && reg < 0x4000);
	if (reg == EN7573_MCU_ENABLE && (unpack(buf) & 1)) {
		assert(m->readbacks == (EN7573_PM_SIZE + EN7573_DM_SIZE) / 4);
		m->start_seen = true;
	}
	m->regs[reg / 4] = unpack(buf);
	return 0;
}

static void delay(void *ctx, unsigned int ms) { (void)ctx; assert(ms == 100 || ms == 500); }

int main(void)
{
	struct model m = {0};
	struct en7573_io io = { .ctx = &m, .read = rd, .write = wr, .delay_ms = delay };
	u8 pm[101], dm[7], cal[513];
	unsigned int i, calls;
	u16 id;
	for (i = 0; i < sizeof(pm); i++) pm[i] = i * 3 + 7;
	for (i = 0; i < sizeof(dm); i++) dm[i] = i + 9;
	for (i = 0; i < sizeof(cal); i++) cal[i] = i * 7 + 5;
	assert(!en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal));
	assert(!m.start_seen);
	assert(!memcmp(m.pm, pm, sizeof(pm)));
	for (i = sizeof(pm); i < sizeof(m.pm); i++) assert(m.pm[i] == 0);
	for (i = 0; i < sizeof(m.dm); i++) {
		u8 expected = i < sizeof(dm) ? dm[i] :
			i >= 0x600 && i < 0x800 ? cal[i - 0x600] : 0;
		assert(m.dm[i] == expected);
	}
	assert(!en7573_start_tx_disabled(&io) && m.start_seen);
	calls = m.calls;
	/* Every I2C error must propagate immediately, including startup errors. */
	for (i = 1; i <= calls; i++) {
		int ret;
		memset(&m, 0, sizeof(m)); m.fail_at = i;
		ret = en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal);
		if (!ret) ret = en7573_start_tx_disabled(&io);
		assert(ret == -EREMOTEIO && m.calls == i);
	}
	memset(&m, 0, sizeof(m)); m.corrupt = true;
	assert(en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal) == -EBADMSG);
	assert(!m.start_seen);
	memset(&m, 0, sizeof(m)); m.wrong_id = true;
	assert(en7573_identify(&io, &id) == -ENODEV && id == 0x1377);
	assert(en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal) == -ENODEV);
	assert(!m.start_seen);
	memset(&m, 0, sizeof(m));
	assert(en7573_load(&io, pm, EN7573_PM_SIZE + 1, dm, sizeof(dm), cal) == -EINVAL);
	assert(en7573_load(&io, pm, sizeof(pm), dm, EN7573_CAL_ADDRESS + 1, cal) == -EINVAL);
	assert(m.calls == 0);
	printf("EN7573 loader: layout, addressing, readback and %u I2C failure points passed\n", calls);
	return 0;
}
