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

/* Q1000K OEM bob_info and EN7572 public controller. Deliberately exclude
 * ddmi_tx/mpd_current command sequences: they alter TX, ADC muxes and gain.
 * No read-clear alarms, FIFO ports, calibration mailboxes or measurement
 * triggers. CSR words are LE32; the six published DDMI words are BE16.
 */
const struct en7573_tx_field en7573_tx_fields[EN7573_TX_FIELDS] = {
 { "temperature", "mC", 0x60, 2, 0 },
 { "supply", "uV", 0x62, 2, 100 },
 { "bias", "uA", 0x64, 2, 2 },
 { "tx_power", "nW", 0x66, 2, 100 },
 { "rx_power", "nW", 0x68, 2, 100 },
 { "modulation", "uA", 0x6a, 2, 2 },
 { "ben_control", "raw", 0x100, 4, 1 },
 { "tx_peak", "raw", 0x108, 4, 1 },
 { "apc_dac", "raw", 0x124, 4, 1 },
 { "erc_control", "raw", 0x128, 4, 1 },
 { "monitor_tia", "raw", 0x130, 4, 1 },
 { "monitor_pga", "raw", 0x13c, 4, 1 },
 { "loop_control", "raw", 0x208, 4, 1 },
 { "current_force", "raw", 0x210, 4, 1 },
 { "bias_limit", "raw", 0x214, 4, 1 },
 { "modulation_limit", "raw", 0x248, 4, 1 },
 { "bias_code", "raw", 0x3c4, 4, 1 },
 { "modulation_code", "raw", 0x3c8, 4, 1 },
 { "tx_control", "raw", 0x3e0, 4, 1 },
 { "ocp_status", "raw", 0x3e4, 4, 1 },
 { "ben_status", "raw", 0x488, 4, 1 },
 /* OEM read1Byte(0xe8); 0xff is a valid eight-bit feature bitmap. */
 { "host_function_flags", "raw", 0xe8, 1, 1 },
};
int en7573_sample_transmitter(struct en7573_io *io, struct en7573_transmitter *sample)
{
 unsigned int i;
 if (!io || !io->read || !sample) return -EINVAL;
 memset(sample, 0, sizeof(*sample));
 for (i = 0; i < EN7573_TX_FIELDS; i++) {
  const struct en7573_tx_field *f = &en7573_tx_fields[i];
  u8 data[4];
  int ret = io->read(io->ctx, EN7573_CONTROL, f->reg, data, f->width);
  sample->error[i] = ret;
  if (ret) continue;
  sample->raw[i] = f->width == 1 ? data[0] : f->width == 2 ? (u32)data[0] << 8 | data[1] : get_le32(data);
  if (f->width != 1 && sample->raw[i] == (f->width == 2 ? 0xffff : ~0U))
   sample->error[i] = -ENODATA;
 }
 return 0;
}

static u8 control_device(struct en7573_io *io, u16 reg)
{
	/* OEM QKX001-06.00.44.00 uses A0 for these MD32 words. The
	 * published loader uses A2; expose the difference as a bench trial.
	 */
	return io->md32_a0 && reg >= 0x3000 && reg <= 0x3018 ?
		EN7573_MEMORY : EN7573_CONTROL;
}

static int control_failure(struct en7573_io *io, const char *stage, u16 reg,
			   int error, u32 actual, u32 expected, u32 mask)
{
	if (io->diagnostic)
		io->diagnostic(io->ctx, stage, control_device(io, reg), reg,
			       error, actual, expected, mask);
	return error;
}

int en7573_read_control(struct en7573_io *io, u16 reg, u32 *value)
{
	u8 data[4];
	int ret = io->read(io->ctx, control_device(io, reg), reg, data, sizeof(data));
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
		return control_failure(io, "state-mcu-read", EN7573_MCU_ENABLE,
				       ret, 0, 0, 0);
	ret = en7573_read_control(io, EN7573_TX_CONTROL, &tx);
	if (ret)
		return control_failure(io, "state-tx-read", EN7573_TX_CONTROL,
				       ret, 0, 0, 0);
	if (mcu == ~0U)
		return control_failure(io, "state-mcu-all-ones", EN7573_MCU_ENABLE,
				       -EIO, mcu, 0, ~0U);
	if (tx == ~0U)
		return control_failure(io, "state-tx-all-ones", EN7573_TX_CONTROL,
				       -EIO, tx, 0, ~0U);
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
		0x80, 0x43c, 0x488, 0x110, 0x114, 0x3e4,
		0x60, 0x62, 0xae, 0xf2, 0xf6 };
	u32 values[15];
	u8 data[4];
	unsigned int i;
	int ret;

	for (i = 0; i < 15; i++) {
		size_t length = i < 10 ? 4 : 2;

		ret = io->read(io->ctx, i ? EN7573_CONTROL : EN7573_MEMORY,
			       registers[i], data, length);
		if (ret)
			return ret;
		values[i] = i < 10 ? get_le32(data) : i < 12 ?
			(u32)data[0] << 8 | data[1] : (u32)data[1] << 8 | data[0];
		if (i < 10 && values[i] == ~0U)
			return -EIO;
	}
	*sample = (struct en7573_receiver) {
		.mcu_a0 = values[0], .mcu_a2 = values[1], .apd = values[2],
		.ocp = values[3], .firmware = values[4], .los_control = values[5],
		.system_status = values[6],
		.rx_output_control = values[7], .rx_output_shape = values[8],
		.ocp_status = values[9], .temperature_raw = values[10],
		.supply_raw = values[11], .apd_voltage_raw = values[12],
		.rssi_adc = values[13], .rssi_current_raw = values[14],
	};
	return 0;
}

static int write_control(struct en7573_io *io, u16 reg, u32 value)
{
	u8 data[] = { value, value >> 8, value >> 16, value >> 24 };
	return io->write(io->ctx, control_device(io, reg), reg, data, sizeof(data));
}

static int update_control(struct en7573_io *io, u16 reg, u32 mask, u32 value)
{
	u32 old;
	int ret = en7573_read_control(io, reg, &old);
	return ret ? ret : write_control(io, reg, (old & ~mask) | (value & mask));
}

static int rx_output_guard(struct en7573_io *io)
{
	struct en7573_state state;
	int ret = en7573_sample_state(io, &state);

	return ret ? ret : !state.md32_enabled || !state.tx_disabled ? -EACCES : 0;
}

static int mpd_update(struct en7573_io *io, u16 reg, u32 mask, u32 value)
{
	u32 old, actual;
	int ret = en7573_read_control(io, reg, &old);
	if (ret || old == ~0U) return ret ?: -EIO;
	value = (old & ~mask) | (value & mask);
	ret = write_control(io, reg, value);
	if (!ret) ret = en7573_read_control(io, reg, &actual);
	return ret ?: (actual != ~0U && (actual & mask) == (value & mask) ? 0 : -EIO);
}

static int mpd_observe(struct en7573_io *io, struct en7573_mpd *s, unsigned int row)
{
	static const u16 regs[] = { 0x33c, 0xf0, 0x66, 0x64, 0x6a, 0x3c4, 0x3c8, 0x3e0, 0x488 };
	unsigned int i;
	int first = 0;
	for (i = 0; i < EN7573_MPD_FIELDS; i++) {
		u8 data[4];
		unsigned int width = i >= 1 && i <= 4 ? 2 : 4;
		int ret = io->read(io->ctx, EN7573_CONTROL, regs[i], data, width);
		if (ret) { if (!first) first = ret; continue; }
		s->values[row][i] = width == 4 ? get_le32(data) : i == 1 ?
			(u32)data[0] | (u32)data[1] << 8 : (u32)data[0] << 8 | data[1];
		/* Preserve raw saturation, but do not call it a valid conversion. */
		if (s->values[row][i] != (width == 2 ? 0xffff : ~0U))
			s->valid[row] |= 1U << i;
	}
	s->sample_error[row] = first;
	return first;
}

int en7573_measure_mpd(struct en7573_io *io, bool active, struct en7573_mpd *s)
{
	/* OEM QKX001-06.00.44.00 en7572.ko mpd_current @ 0x4d10.
	 * The OEM maps 256-(33c>>7) through a table. Keep raw values here:
	 * neither that table nor this monitor selection establishes optical mW.
	 * No exposed conversion-ready flag: the 50+5 ms waits are OEM timing,
	 * not proof of a fresh conversion. Never invoke ddmi_tx: it recalibrates.
	 */
	static const u16 regs[] = { 0x130, 0x208, 0x120 };
	static const u32 masks[] = { 0x3f00, 0x70, 1U << 26 };
	unsigned int i;
	bool touched = false;
	int ret, next;
	if (!io || !io->read || !s || (active && (!io->write || !io->delay_ms)))
		return -EINVAL;
	memset(s, 0, sizeof(*s));
	s->active = active;
	s->restored = true;
	for (i = 0; i < 3; i++) {
		ret = en7573_read_control(io, regs[i], &s->saved[i]);
		if (ret || s->saved[i] == ~0U) return ret ?: -EIO;
	}
	ret = mpd_observe(io, s, 0);
	if (ret || !active) return ret;
	/* Do not take over an already selected OEM measurement. */
	if (s->saved[2] & masks[2]) return -EBUSY;
	touched = true;
	s->restored = false;
	ret = mpd_update(io, 0x208, masks[1], 4U << 4);
	if (!ret) ret = mpd_update(io, 0x130, masks[0], 0x24U << 8);
	if (!ret) {
		io->delay_ms(io->ctx, 50);
		ret = mpd_update(io, 0x120, masks[2], masks[2]);
	}
	if (!ret) {
		io->delay_ms(io->ctx, 5);
		for (i = 0; i < 3; i++) {
			ret = en7573_read_control(io, regs[i], &s->selected[i]);
			if (ret) break;
			s->selected_valid |= 1U << i;
		}
	}
	if (!ret) ret = mpd_observe(io, s, 1);
	/* OEM order: restore monitor gain, release selection, restore loop mux.
	 * Continue every restoration after errors. The owner contains any failure.
	 */
	if (touched) {
		static const unsigned int order[] = { 0, 2, 1 };
		for (i = 0; i < 3; i++) {
			unsigned int j = order[i];
			next = mpd_update(io, regs[j], masks[j], s->saved[j]);
			if (next && !s->restore_error) s->restore_error = next;
		}
		s->restored = !s->restore_error;
	}
	next = mpd_observe(io, s, 2);
	return ret ?: s->restore_error ?: next;
}

static int rx_output_update(struct en7573_io *io, u16 reg, u32 mask, u32 value)
{
	u32 old, actual;
	int ret = rx_output_guard(io);

	if (!ret)
		ret = en7573_read_control(io, reg, &old);
	if (ret || old == ~0U)
		return ret ? ret : -EIO;
	ret = write_control(io, reg, (old & ~mask) | (value & mask));
	if (!ret)
		ret = en7573_read_control(io, reg, &actual);
	if (ret || actual == ~0U || actual != ((old & ~mask) | (value & mask)))
		return ret ? ret : -EIO;
	return rx_output_guard(io);
}

int en7573_output_sample(struct en7573_io *io, struct en7573_output_sample *s)
{
	static const u16 regs[EN7573_OUTPUT_FIELDS] = {
		0x488, 0x3a4, 0xf0, 0xfe, 0x3a4, 0x488, 0x33c,
		0x66, 0x64, 0x6a, 0x3c4, 0x3c8, 0x3e0, 0x83, 0xfb,
		0x100, 0x108, 0x120, 0x124, 0x128, 0x130, 0x13c,
		0x208, 0x210, 0x214, 0x248, 0x160, 0x3e4, 0xb4, 0xb8,
	};
	unsigned int i;
	if (!io || !io->read || !s) return -EINVAL;
	memset(s, 0, sizeof(*s));
	s->board_disabled = -1;
	for (i = 0; i < EN7573_OUTPUT_FIELDS; i++) {
		u8 data[4];
		unsigned int width = i == 3 || i == 13 || i == 14 ? 1 :
			i == 2 || (i >= 7 && i <= 9) ? 2 : 4;
		int ret = io->read(io->ctx, EN7573_CONTROL, regs[i], data, width);
		/* Do not extend an enabled window with more failed bus transfers. */
		if (ret) { s->error = ret; return ret; }
		s->values[i] = width == 1 ? data[0] : width == 4 ? get_le32(data) :
			i == 2 ? (u32)data[0] | (u32)data[1] << 8 : (u32)data[0] << 8 | data[1];
		/* Calibration words and byte flags can legitimately be all ones. */
		if (i >= 28 || width == 1 || s->values[i] != (width == 2 ? 0xffff : ~0U))
			s->valid |= 1U << i;
	}
	return s->error;
}

int en7573_output_hold(struct en7573_io *io, struct en7573_output_hold *s, bool restore)
{
	static const u16 regs[] = { 0x130, 0x208, 0x120 };
	static const u32 masks[] = { 0x3f00, 0x70, 1U << 26 };
	static const unsigned int order[] = { 0, 2, 1 };
	unsigned int i;
	int ret, first = 0;
	if (!io || !s || !io->read || !io->write || !io->delay_ms) return -EINVAL;
	if (restore && !s->saved_valid) return 0;
	ret = rx_output_guard(io);
	if (ret) return ret;
	if (restore) {
		for (i = 0; i < 3; i++) {
			unsigned int j = order[i];
			ret = mpd_update(io, regs[j], masks[j], s->saved[j]);
			if (ret && !first) first = ret;
		}
		if (!first) s->saved_valid = false;
		return first;
	}
	if (s->saved_valid) return -EBUSY;
	for (i = 0; i < 3; i++) {
		ret = en7573_read_control(io, regs[i], &s->saved[i]);
		if (ret || s->saved[i] == ~0U) return ret ?: -EIO;
	}
	if (s->saved[2] & masks[2]) return -EBUSY;
	s->saved_valid = true; /* caller restores even after a partial setup */
	ret = mpd_update(io, 0x208, masks[1], 4U << 4);
	if (!ret) ret = mpd_update(io, 0x130, masks[0], 0x24U << 8);
	if (!ret) {
		io->delay_ms(io->ctx, 50);
		ret = mpd_update(io, 0x120, masks[2], masks[2]);
	}
	if (!ret) io->delay_ms(io->ctx, 5);
	return ret;
}

static const u16 tx_recipe_regs[EN7573_TX_SAVED] = {
 0x100, 0x210, 0x124, 0x130, 0x128, 0x13c, 0x208, 0xb4,
};

/* TSSI calibration is a four-byte data word, so an erased all-ones value
 * can be saved/restored. It is not a controller-health register. */
static int tx_recipe_tssi(struct en7573_io *io, u32 value)
{
 u32 actual;
 int ret = rx_output_guard(io);
 if (!ret) ret = write_control(io, 0xb4, value);
 if (!ret) ret = en7573_read_control(io, 0xb4, &actual);
 if (!ret && actual != value) ret = -EIO;
 return ret ?: rx_output_guard(io);
}

int en7573_tx_recipe(struct en7573_io *io, const u8 *cal, unsigned int recipe,
                     struct en7573_tx_recipe *saved, bool restore)
{
 const u8 *eye;
 u32 imod, iav, erc, value;
 unsigned int i, count;
 int ret, first = 0;
 if (!io || !io->read || !io->write || !saved) return -EINVAL;
 if (restore && !saved->count) return 0;
 ret = rx_output_guard(io);
 if (ret) return ret;
 if (restore) {
  /* Reverse order restores the original gate last. Continue restoring on
   * a bus failure; the controller owner removes power if anything fails. */
  for (i = saved->count; i; i--) {
   ret = i == 8 ? tx_recipe_tssi(io, saved->words[i-1]) :
    rx_output_update(io, tx_recipe_regs[i-1], ~0U, saved->words[i-1]);
   if (ret && !first) first = ret;
  }
  if (!first) saved->count = 0;
  return first;
 }
 if (recipe < 1 || recipe > 6 || !cal) return -EINVAL;
 if (saved->count) return -EBUSY;
 eye = cal + ((recipe == 2 || recipe == 4) ? 256 : 0);
 imod = (u32)eye[0x86] | (u32)eye[0x87] << 8;
 iav = (u32)eye[0x88] | (u32)eye[0x89] << 8;
 erc = (u32)eye[0x8e] | (u32)eye[0x8f] << 8;
 /* Reject absent/out-of-range calibrated eyes, never invent drive codes. */
 if (recipe >= 2 && recipe <= 5 &&
     ((!imod && !iav) || imod > 0xfff || iav > 0x1fff || erc > 0xfff ||
      eye[0x8c] > 1 || eye[0x90] > 7 || eye[0x91] > 7 ||
      eye[0x92] > 7 || eye[0x93] > 3)) return -ENODATA;
 if ((recipe == 4 || recipe == 5) && get_le32(eye + 0xb4) == ~0U) return -ENODATA;
 count = recipe == 4 || recipe == 5 ? 8 : 7;
 for (i = 0; i < count; i++) {
  ret = en7573_read_control(io, tx_recipe_regs[i], &value);
  if (ret || (i != 7 && value == ~0U)) return ret ?: -EIO;
  saved->words[i] = value;
 }
 saved->count = count;
 ret = rx_output_update(io, 0x100, 0xc, 0x8); /* BEN off */
 if (recipe == 6 || ret) return ret;
 if (recipe >= 2) {
#define TX_STEP(reg, mask, val) do { if (!ret) ret = rx_output_update(io, reg, mask, val); } while (0)
  TX_STEP(0x210, 0x0fff1fff, (imod << 16) | iav);
  TX_STEP(0x124, 0xff00, (u32)eye[0x8a] << 8);
  TX_STEP(0x130, 0x3f01, (((u32)eye[0x91] << 3 | eye[0x90]) << 8) | eye[0x8c]);
  TX_STEP(0x128, 0x0fffff00, erc << 16 | (u32)eye[0x8d] << 8);
  TX_STEP(0x13c, 0x70060, (u32)eye[0x92] << 16 | (u32)eye[0x93] << 5);
  if (!ret && recipe >= 4) ret = tx_recipe_tssi(io, get_le32(eye + 0xb4));
 }
 TX_STEP(0x208, 1, 0);
 TX_STEP(0x208, 1, 1);
 TX_STEP(0x100, 0xc, recipe == 1 ? saved->words[0] : 0);
#undef TX_STEP
 return ret;
}

int en7573_oem_post_init(struct en7573_io *io, struct en7573_oem_post *original,
			bool restore)
{
	u32 value;
	int ret;

	if (!io || !io->read || !io->write || !original)
		return -EINVAL;
	if (restore && !original->saved)
		return 0;
	if (!restore && original->saved)
		return -EBUSY;
	ret = rx_output_guard(io);
	if (!ret)
		ret = en7573_read_control(io, 0x110, &value);
	if (ret || value == ~0U)
		return ret ? ret : -EIO;
	if (!restore) {
		original->control = value;
		original->saved = true;
	}
	ret = rx_output_update(io, 0x110, 0x100,
			       restore ? original->control : 0x100);
	if (!ret && restore)
		original->saved = false;
	return ret;
}

int en7573_apply_rx_output(struct en7573_io *io, unsigned int profile,
			  struct en7573_rx_output *original)
{
	/* Pinned EN7572 SetRxPreEmphasis/RX_PE_LUT 950199a: electrical
	 * RX output to the SoC, not optical transmit drive. Source field order.
	 */
	static const u32 shapes[] = { 0, 0x14001400, 0x1e001e00, 0x36083208 };
	static const u32 masks[] = { 0x00003f00, 0x001f0000, 0x3f000000, 0x8 };
	struct en7573_rx_output saved = { 0 };
	unsigned int i;
	int ret;

	if (!io || !io->read || !io->write || !original || profile > 3)
		return -EINVAL;
	if (original->saved)
		return -EBUSY;
	if (!profile)
		return 0;
	ret = rx_output_guard(io);
	if (!ret)
		ret = en7573_read_control(io, 0x110, &saved.control);
	if (!ret)
		ret = en7573_read_control(io, 0x114, &saved.shape);
	if (ret || saved.control == ~0U || saved.shape == ~0U)
		return ret ? ret : -EIO;
	saved.saved = true;
	*original = saved; /* Save before the first possibly partial write. */
	for (i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
		ret = rx_output_update(io, 0x114, masks[i], shapes[profile]);
		if (ret)
			return ret;
	}
	return rx_output_update(io, 0x110, 0x40, profile == 3 ? 0 : 0x40);
}

int en7573_restore_rx_output(struct en7573_io *io,
			    struct en7573_rx_output *original)
{
	static const u32 masks[] = { 0x8, 0x3f000000, 0x001f0000, 0x00003f00 };
	unsigned int i;
	int ret;

	if (!io || !io->read || !io->write || !original)
		return -EINVAL;
	if (!original->saved)
		return 0;
	ret = rx_output_update(io, 0x110, 0x40, original->control);
	for (i = 0; !ret && i < sizeof(masks) / sizeof(masks[0]); i++)
		ret = rx_output_update(io, 0x114, masks[i], original->shape);
	if (!ret)
		original->saved = false;
	return ret;
}

int en7573_set_tx(struct en7573_io *io, bool enable)
{
	u32 old, actual, expected;
	int ret = en7573_read_control(io, EN7573_TX_CONTROL, &old);

	if (ret)
		return control_failure(io, "set-tx-read", EN7573_TX_CONTROL,
				       ret, 0, 0, 0);
	if (old == ~0U)
		return control_failure(io, "set-tx-old-all-ones", EN7573_TX_CONTROL,
				       -EIO, old, 0, ~0U);
	expected = enable ? old & ~EN7573_TX_DISABLE : old | EN7573_TX_DISABLE;
	ret = write_control(io, EN7573_TX_CONTROL, expected);
	if (ret)
		return control_failure(io, "set-tx-write", EN7573_TX_CONTROL,
				       ret, 0, 0, 0);
	ret = en7573_read_control(io, EN7573_TX_CONTROL, &actual);
	if (ret)
		return control_failure(io, "set-tx-readback", EN7573_TX_CONTROL,
				       ret, 0, 0, 0);
	if (actual == ~0U)
		return control_failure(io, "set-tx-readback-all-ones", EN7573_TX_CONTROL,
				       -EIO, actual, expected, ~0U);
	if (!!(actual & EN7573_TX_DISABLE) == enable)
		return control_failure(io, "set-tx-readback-mismatch", EN7573_TX_CONTROL,
				       -EIO, actual, expected, EN7573_TX_DISABLE);
	return 0;
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
