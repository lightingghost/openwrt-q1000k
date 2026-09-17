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
	bool corrupt, wrong_id, start_seen, md32_a0, output_mismatch, output_unrelated_mismatch, drop_tx_guard;
	bool tx_readback_mismatch, tx_readback_all_ones;
	unsigned int diagnostics;
	const char *diagnostic_stage;
	u8 diagnostic_device;
	u16 diagnostic_reg;
	int diagnostic_error;
	u32 diagnostic_actual, diagnostic_expected, diagnostic_mask;
};

static void diagnostic(void *ctx, const char *stage, u8 device, u16 reg,
		       int error, u32 actual, u32 expected, u32 mask)
{
	struct model *m = ctx;

	assert(error);
	m->diagnostics++;
	m->diagnostic_stage = stage;
	m->diagnostic_device = device;
	m->diagnostic_reg = reg;
	m->diagnostic_error = error;
	m->diagnostic_actual = actual;
	m->diagnostic_expected = expected;
	m->diagnostic_mask = mask;
}

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
	if (reg == 0x3008 || reg == 0x3014) {
		assert(dev == 0x50);
		bool pm = reg == 0x3008;
		u32 addr = m->regs[(pm ? 0x3004 : 0x3010) / 4];
		assert(addr <= (pm ? sizeof(m->pm) : sizeof(m->dm)) - 4);
		memcpy(buf, (pm ? m->pm : m->dm) + addr, 4);
		if (m->corrupt) buf[0] ^= 1;
		m->readbacks++;
		return 0;
	}
	assert(dev == (m->md32_a0 && reg >= 0x3000 && reg <= 0x3018 ? 0x50 : 0x51) && !(reg & 3) && reg < 0x4000);
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
	if (reg == 0x3008 || reg == 0x3014) {
		assert(dev == 0x50);
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
	assert(dev == (m->md32_a0 && reg >= 0x3000 && reg <= 0x3018 ? 0x50 : 0x51) && !(reg & 3) && reg < 0x4000);
	if (reg == EN7573_MCU_ENABLE && (unpack(buf) & 1)) {
		assert(m->readbacks == (EN7573_PM_SIZE + EN7573_DM_SIZE) / 4);
		m->start_seen = true;
	}
	m->regs[reg / 4] = unpack(buf);
	if (reg == EN7573_TX_CONTROL && m->tx_readback_mismatch)
		m->regs[reg / 4] ^= EN7573_TX_DISABLE;
	if (reg == EN7573_TX_CONTROL && m->tx_readback_all_ones)
		m->regs[reg / 4] = ~0U;
	if (m->output_mismatch && reg == 0x114)
		m->regs[reg / 4] ^= 0x100;
	if (m->output_unrelated_mismatch && reg == 0x114)
		m->regs[reg / 4] ^= 0x80000000;
	if (m->drop_tx_guard && reg == 0x114)
		m->regs[EN7573_TX_CONTROL/4] &= ~EN7573_TX_DISABLE;
	return 0;
}

static void delay(void *ctx, unsigned int ms) { (void)ctx; assert(ms == 100 || ms == 500); }

static void test_read_only_state(void)
{
	struct model m = {0};
	/* Any attempted write or delay would call a NULL function pointer. */
	struct en7573_io io = { .ctx = &m, .read = rd };
	struct en7573_state state;
	unsigned int mcu, tx, fail;

	for (mcu = 0; mcu < 2; mcu++) {
		for (tx = 0; tx < 2; tx++) {
			m.regs[EN7573_MCU_ENABLE / 4] = mcu;
			m.regs[EN7573_TX_CONTROL / 4] = tx ? EN7573_TX_DISABLE : 0;
			assert(!en7573_sample_state(&io, &state));
			assert(state.md32_enabled == (int)mcu);
			assert(state.tx_disabled == (int)tx);
		}
	}
	for (fail = 1; fail <= 2; fail++) {
		m.calls = 0;
		m.fail_at = fail;
		state.md32_enabled = state.tx_disabled = 1;
		assert(en7573_sample_state(&io, &state) == -EREMOTEIO);
		assert(state.md32_enabled == -1 && state.tx_disabled == -1);
		assert(m.calls == fail);
		assert(m.regs[EN7573_MCU_ENABLE / 4] == 1);
		assert(m.regs[EN7573_TX_CONTROL / 4] == EN7573_TX_DISABLE);
	}
}

static void test_tx_control(void)
{
    struct model m = {0};
    struct en7573_io io = { .ctx=&m, .read=rd, .write=wr };
    struct en7573_state state;
    unsigned int enable, fail;
    for (enable=0; enable<2; enable++) {
        m.regs[EN7573_TX_CONTROL/4]=0xa5a55a5a;
        assert(!en7573_set_tx(&io,enable));
        assert(m.regs[EN7573_TX_CONTROL/4]==
            ((0xa5a55a5a&~EN7573_TX_DISABLE)|(enable ? 0 : EN7573_TX_DISABLE)));
        for (fail=1;fail<=3;fail++) {
            m.calls=0; m.fail_at=fail;
            assert(en7573_set_tx(&io,enable)==-EREMOTEIO && m.calls==fail);
        }
        m.fail_at=0;
    }
    m.regs[EN7573_TX_CONTROL/4]=~0U;
    assert(en7573_set_tx(&io,false)==-EIO);
    assert(en7573_sample_state(&io,&state)==-EIO && state.tx_disabled==-1);
}

static void test_control_failure_diagnostics(void)
{
	const char *tx_stages[] = { "set-tx-read", "set-tx-write", "set-tx-readback" };
	const char *state_stages[] = { "state-mcu-read", "state-tx-read" };
	struct model m = {0};
	struct en7573_io io = { .ctx=&m, .read=rd, .write=wr, .diagnostic=diagnostic };
	struct en7573_state state;
	unsigned int fail, bank;

	for (bank=0; bank<2; bank++) {
		io.md32_a0 = m.md32_a0 = bank;
		for (fail=1; fail<=2; fail++) {
			m.calls=m.diagnostics=0; m.fail_at=fail;
			assert(en7573_sample_state(&io,&state)==-EREMOTEIO);
			assert(m.calls==fail && m.diagnostics==1);
			assert(!strcmp(m.diagnostic_stage,state_stages[fail-1]));
			assert(m.diagnostic_device==(bank && fail==1 ? 0x50 : 0x51));
			assert(m.diagnostic_reg==(fail==1 ? 0x3018 : 0x3e0));
			assert(m.diagnostic_error==-EREMOTEIO && !m.diagnostic_mask);
			assert(state.md32_enabled==-1 && state.tx_disabled==-1);
		}
	}
	m.fail_at=0; m.calls=m.diagnostics=0;
	m.regs[0x3018/4]=1; m.regs[0x3e0/4]=EN7573_TX_DISABLE;
	assert(!en7573_sample_state(&io,&state) && m.calls==2 && !m.diagnostics);
	for (fail=0; fail<2; fail++) {
		m.calls=m.diagnostics=0;
		m.regs[0x3018/4]=fail ? 1 : ~0U;
		m.regs[0x3e0/4]=fail ? ~0U : EN7573_TX_DISABLE;
		assert(en7573_sample_state(&io,&state)==-EIO && m.calls==2 && m.diagnostics==1);
		assert(!strcmp(m.diagnostic_stage,fail ? "state-tx-all-ones" : "state-mcu-all-ones"));
		assert(m.diagnostic_actual==~0U && m.diagnostic_mask==~0U);
	}
	m.regs[0x3e0/4]=EN7573_TX_DISABLE;
	for (fail=1; fail<=3; fail++) {
		m.calls=m.diagnostics=0; m.fail_at=fail;
		assert(en7573_set_tx(&io,false)==-EREMOTEIO && m.calls==fail && m.diagnostics==1);
		assert(!strcmp(m.diagnostic_stage,tx_stages[fail-1]));
		assert(m.diagnostic_device==0x51 && m.diagnostic_reg==0x3e0);
		assert(m.diagnostic_error==-EREMOTEIO && !m.diagnostic_mask);
	}
	m.fail_at=0; m.calls=m.diagnostics=0;
	m.regs[0x3e0/4]=~0U;
	assert(en7573_set_tx(&io,false)==-EIO && m.calls==1 && m.diagnostics==1);
	assert(!strcmp(m.diagnostic_stage,"set-tx-old-all-ones"));
	m.calls=m.diagnostics=0; m.regs[0x3e0/4]=0x12340000; m.tx_readback_mismatch=true;
	assert(en7573_set_tx(&io,false)==-EIO && m.calls==3 && m.diagnostics==1);
	assert(!strcmp(m.diagnostic_stage,"set-tx-readback-mismatch"));
	assert(m.diagnostic_actual==0x12340000 && m.diagnostic_expected==0x12340200);
	assert(m.diagnostic_mask==EN7573_TX_DISABLE);
	m.calls=m.diagnostics=0; m.tx_readback_mismatch=false; m.tx_readback_all_ones=true;
	assert(en7573_set_tx(&io,false)==-EIO && m.calls==3 && m.diagnostics==1);
	assert(!strcmp(m.diagnostic_stage,"set-tx-readback-all-ones"));
	m.calls=m.diagnostics=0; m.tx_readback_all_ones=false; m.regs[0x3e0/4]=0x12340000;
	assert(!en7573_set_tx(&io,false) && m.calls==3 && !m.diagnostics);
}

struct receiver_model { unsigned int calls, fail_at, erased_at; };

static int receiver_read(void *ctx, u8 dev, u16 reg, u8 *data, size_t len)
{
	struct receiver_model *m = ctx;
	const u16 expected[] = { 0x3018, 0x3018, 0x15c, 0x160, 0x80, 0x43c, 0x488,
		0x110, 0x114, 0x3e4, 0x60, 0x62, 0xae, 0xf2, 0xf6 };
	unsigned int i = m->calls++;
	u32 value = 0x10203040 + i;

	assert(i < 15 && reg == expected[i] && dev == (i ? 0x51 : 0x50) && len == (i < 10 ? 4 : 2));
	if (m->calls == m->fail_at)
		return -EREMOTEIO;
	if (m->calls == m->erased_at)
		value = ~0U;
	for (i = 0; i < len; i++)
		data[i] = value >> (i * 8);
	return 0;
}

static void test_receiver_read_only(void)
{
	struct receiver_model m = {0};
	/* Any register write or delay would call a NULL function pointer. */
	struct en7573_io io = { .ctx = &m, .read = receiver_read };
	struct en7573_receiver sample, before;
	unsigned int fail;

	assert(!en7573_sample_receiver(&io, &sample) && m.calls == 15);
	assert(sample.mcu_a0 == 0x10203040 && sample.mcu_a2 == 0x10203041);
	assert(sample.apd == 0x10203042 && sample.ocp == 0x10203043);
	assert(sample.firmware == 0x10203044 && sample.los_control == 0x10203045);
	assert(sample.system_status == 0x10203046);
	assert(sample.rx_output_control == 0x10203047 && sample.rx_output_shape == 0x10203048);
	assert(sample.ocp_status == 0x10203049);
	assert(sample.temperature_raw == 0x4a30 && sample.supply_raw == 0x4b30);
	assert(sample.apd_voltage_raw == 0x304c && sample.rssi_adc == 0x304d);
	assert(sample.rssi_current_raw == 0x304e);
	before = sample;
	for (fail = 1; fail <= 15; fail++) {
		m = (struct receiver_model) { .fail_at = fail };
		assert(en7573_sample_receiver(&io, &sample) == -EREMOTEIO && m.calls == fail);
		assert(!memcmp(&sample, &before, sizeof(sample)));
		if (fail > 10) continue;
		m = (struct receiver_model) { .erased_at = fail };
		assert(en7573_sample_receiver(&io, &sample) == -EIO && m.calls == fail);
		assert(!memcmp(&sample, &before, sizeof(sample)));
	}
}

static int sensor_read(void *ctx,u8 dev,u16 reg,u8 *data,size_t len)
{
    assert(dev==0x51 && reg==0x0068 && len==2);
    int raw=*(int *)ctx;
    if(raw<0) return raw;
    data[0]=raw>>8; data[1]=raw; return 0;
}
static void test_rx_power(void)
{
    int raw=0;
    struct en7573_io io={.ctx=&raw,.read=sensor_read};
    u32 power;
    /* Exercise every published word, including endian-sensitive values. */
    for(raw=0;raw<=65535;raw++) {
        power=0xdeadbeef;
        int ret=en7573_rx_power(&io,&power);
        if(!raw || raw==65535) assert(ret==-ENODATA && power==0xdeadbeef);
        else assert(!ret && power==(u32)raw*100);
    }
    raw=-EREMOTEIO; power=123;
    assert(en7573_rx_power(&io,&power)==-EREMOTEIO && power==123);
    assert(en7573_rx_power(NULL,&power)==-EINVAL);
    assert(en7573_rx_power(&io,NULL)==-EINVAL);
    io.read=NULL; assert(en7573_rx_power(&io,&power)==-EINVAL);
}

static void test_loader(bool oem_a0)
{
	struct model m = { .md32_a0 = oem_a0 };
	struct en7573_io io = { .ctx = &m, .md32_a0 = oem_a0, .read = rd, .write = wr, .delay_ms = delay };
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
		memset(&m, 0, sizeof(m)); m.md32_a0 = oem_a0; m.fail_at = i;
		ret = en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal);
		if (!ret) ret = en7573_start_tx_disabled(&io);
		assert(ret == -EREMOTEIO && m.calls == i);
	}
	memset(&m, 0, sizeof(m)); m.md32_a0 = oem_a0; m.corrupt = true;
	assert(en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal) == -EBADMSG);
	assert(!m.start_seen);
	memset(&m, 0, sizeof(m)); m.md32_a0 = oem_a0; m.wrong_id = true;
	assert(en7573_identify(&io, &id) == -ENODEV && id == 0x1377);
	assert(en7573_load(&io, pm, sizeof(pm), dm, sizeof(dm), cal) == -ENODEV);
	assert(!m.start_seen);
	memset(&m, 0, sizeof(m)); m.md32_a0 = oem_a0;
	assert(en7573_load(&io, pm, EN7573_PM_SIZE + 1, dm, sizeof(dm), cal) == -EINVAL);
	assert(en7573_load(&io, pm, sizeof(pm), dm, EN7573_CAL_ADDRESS + 1, cal) == -EINVAL);
	assert(m.calls == 0);

	printf("EN7573 loader: layout, addressing, readback and %u I2C failure points passed\n", calls);
	printf("EN7573 transport: %s passed\n", oem_a0 ? "OEM A0" : "public A2");
}

static void test_oem_post(void)
{
    struct model m = {0};
    struct en7573_io io = {.ctx=&m,.read=rd,.write=wr};
    struct en7573_oem_post original = {0};
    unsigned int calls, fail;
    m.regs[0x3018/4]=1; m.regs[0x3e0/4]=EN7573_TX_DISABLE;
    m.regs[0x110/4]=0x01002c1f;
    assert(!en7573_oem_post_init(&io,&original,false));
    assert(original.saved && original.control==0x01002c1f && m.regs[0x110/4]==0x01002d1f);
    assert(m.regs[0x3e0/4]==EN7573_TX_DISABLE);
    calls=m.calls;
    assert(en7573_oem_post_init(&io,&original,false)==-EBUSY);
    m.regs[0x110/4]^=0x80000000;
    assert(!en7573_oem_post_init(&io,&original,true) && !original.saved);
    assert(m.regs[0x110/4]==0x81002c1f);
    for(fail=1;fail<=calls;fail++) {
        memset(&m,0,sizeof(m)); memset(&original,0,sizeof(original));
        m.regs[0x3018/4]=1; m.regs[0x3e0/4]=EN7573_TX_DISABLE; m.regs[0x110/4]=0x01002c1f;
        m.fail_at=fail;
        assert(en7573_oem_post_init(&io,&original,false)==-EREMOTEIO && m.calls==fail);
        m.fail_at=0;
        assert(!en7573_oem_post_init(&io,&original,true));
        assert(m.regs[0x110/4]==0x01002c1f && m.regs[0x3e0/4]==EN7573_TX_DISABLE);
    }
    m.regs[0x3e0/4]=0;
    assert(en7573_oem_post_init(&io,&original,false)==-EACCES && !original.saved);
    m.regs[0x3e0/4]=EN7573_TX_DISABLE; m.regs[0x3018/4]=0;
    assert(en7573_oem_post_init(&io,&original,false)==-EACCES && !original.saved);
}

static void test_rx_output(void)
{
	const u32 mask = 0x3f1f3f08, shapes[] = {0,0x14001400,0x1e001e00,0x36083208};
	struct model m;
	struct en7573_rx_output original;
	struct en7573_io io = { .ctx=&m, .read=rd, .write=wr };
	unsigned int profile, fail, calls;

	for (profile=1; profile<=3; profile++) {
		memset(&m,0,sizeof(m)); memset(&original,0,sizeof(original));
		m.regs[0x3018/4]=1; m.regs[0x3e0/4]=EN7573_TX_DISABLE;
		m.regs[0x110/4]=0xaabbccdd; m.regs[0x114/4]=0x12345678;
		assert(!en7573_apply_rx_output(&io,profile,&original));
		assert(original.saved && original.control==0xaabbccdd && original.shape==0x12345678);
		assert(m.regs[0x114/4]==((0x12345678&~mask)|shapes[profile]));
		assert(m.regs[0x110/4]==((0xaabbccdd&~0x40)|(profile==3 ? 0 : 0x40)));
		calls=m.calls;
		assert(en7573_apply_rx_output(&io,profile,&original)==-EBUSY);
		/* Restore only owned masks, retaining concurrent unrelated changes. */
		m.regs[0x114/4]^=0x80000000; m.regs[0x110/4]^=0x80000000;
		assert(!en7573_restore_rx_output(&io,&original) && !original.saved);
		assert(m.regs[0x114/4]==(0x12345678^0x80000000));
		assert(m.regs[0x110/4]==(0xaabbccdd^0x80000000));
		for(fail=1; fail<=calls; fail++) {
			memset(&m,0,sizeof(m)); memset(&original,0,sizeof(original));
			m.regs[0x3018/4]=1; m.regs[0x3e0/4]=EN7573_TX_DISABLE;
			m.regs[0x110/4]=0xaabbccdd; m.regs[0x114/4]=0x12345678;
			m.fail_at=fail;
			assert(en7573_apply_rx_output(&io,profile,&original)==-EREMOTEIO && m.calls==fail);
			m.fail_at=0;
			assert(!en7573_restore_rx_output(&io,&original));
			assert(m.regs[0x110/4]==0xaabbccdd && m.regs[0x114/4]==0x12345678);
		}
	}
	memset(&m,0,sizeof(m)); memset(&original,0,sizeof(original));
	assert(!en7573_apply_rx_output(&io,0,&original) && !m.calls);
	assert(en7573_apply_rx_output(&io,4,&original)==-EINVAL && !m.calls);
	assert(en7573_apply_rx_output(&io,1,&original)==-EACCES);
	m.regs[0x3018/4]=1;
	assert(en7573_apply_rx_output(&io,1,&original)==-EACCES);
	m.regs[0x3e0/4]=EN7573_TX_DISABLE; m.output_mismatch=true;
	assert(en7573_apply_rx_output(&io,1,&original)==-EIO && original.saved);
	m.output_mismatch=false;
	assert(!en7573_restore_rx_output(&io,&original));
	m.output_unrelated_mismatch=true;
	assert(en7573_apply_rx_output(&io,1,&original)==-EIO && original.saved);
	m.output_unrelated_mismatch=false;
	assert(!en7573_restore_rx_output(&io,&original));
	m.drop_tx_guard=true;
	assert(en7573_apply_rx_output(&io,1,&original)==-EACCES && original.saved);
	assert(en7573_restore_rx_output(&io,&original)==-EACCES && original.saved);
	m.drop_tx_guard=false; m.regs[0x3e0/4]=EN7573_TX_DISABLE;
	assert(!en7573_restore_rx_output(&io,&original));
	/* Restoration failure leaves the saved record for power-off containment. */
	assert(!en7573_apply_rx_output(&io,1,&original));
	m.fail_at=m.calls+1;
	assert(en7573_restore_rx_output(&io,&original)==-EREMOTEIO && original.saved);
	m.fail_at=0;
	assert(!en7573_restore_rx_output(&io,&original));
}

static unsigned int tx_reads, tx_fail, tx_saturated;
static int tx_read(void *ctx, u8 device, u16 reg, u8 *data, size_t length)
{
 unsigned int i = tx_reads++;
 const struct en7573_tx_field *f = &en7573_tx_fields[i];
 assert(device == 0x51 && reg == f->reg && length == f->width);
 if (tx_fail == i + 1) return -EREMOTEIO;
 if (tx_saturated == i + 1) { memset(data, 0xff, length); return 0; }
 if (length == 2) { data[0] = 0x12; data[1] = 0x34; }
 else { data[0] = 0x78; data[1] = 0x56; data[2] = 0x34; data[3] = 0x12; }
 if (!i) memset(data, 0, length); /* Zero must not become unavailable. */
 return 0;
}
static void test_transmitter(void)
{
 struct en7573_io io = { .read = tx_read }; /* No write or delay callback. */
 struct en7573_transmitter s;
 for (unsigned int fail = 0; fail <= EN7573_TX_FIELDS; fail++) {
  tx_reads=0; tx_fail=fail; tx_saturated=0;
  assert(!en7573_sample_transmitter(&io,&s) && tx_reads==EN7573_TX_FIELDS);
  for (unsigned int i=0; i<EN7573_TX_FIELDS; i++) {
   assert(s.error[i] == (fail==i+1 ? -EREMOTEIO : 0));
   if (fail!=i+1) assert(s.raw[i] == (!i ? 0 : i<6 ? 0x1234 : 0x12345678));
  }
 }
 tx_fail=0;
 for (unsigned int i=0; i<EN7573_TX_FIELDS; i++) {
  tx_reads=0; tx_saturated=i+1;
  assert(!en7573_sample_transmitter(&io,&s));
  assert(s.error[i]==-ENODATA && s.raw[i]==(i<6 ? 0xffff : ~0U));
 }
 assert(en7573_sample_transmitter(NULL,&s)==-EINVAL);
}
int main(void)
{
	test_transmitter();
	test_loader(false); test_loader(true);
	test_rx_power(); test_read_only_state(); test_tx_control();
	test_control_failure_diagnostics();
	test_receiver_read_only(); test_rx_output();
	test_oem_post();
	puts("EN7573 state, RX output guards/restore, observations and failures passed");
	return 0;
}
