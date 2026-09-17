/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q1000K_EN7573_H
#define Q1000K_EN7573_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#define EN7573_CONTROL 0x51
#define EN7573_MEMORY 0x50
#define EN7573_ID 0x1388
#define EN7573_PM_SIZE 16384
#define EN7573_DM_SIZE 4096
#define EN7573_CAL_SIZE 512
#define EN7573_CAL_ADDRESS 0x600
#define EN7573_MCU_ENABLE 0x3018
#define EN7573_TX_CONTROL 0x3e0
#define EN7573_TX_DISABLE (1U << 9)

/* Normally control/address registers use 0x51; the immutable OEM bench
 * profile moves only MD32 words to 0x50. Memory data ports always use 0x50.
 * Register addresses are big endian, register words and memory are little
 * endian, except the published optical-power words, which are big endian.
 * Transport callbacks return zero only for a complete transaction.
 */
struct en7573_io {
	void *ctx;
	/* Fixed at controller bind; ordinary control words always stay on A2. */
	bool md32_a0;
	int (*read)(void *ctx, u8 device, u16 reg, u8 *data, size_t len);
	int (*write)(void *ctx, u8 device, u16 reg, const u8 *data, size_t len);
	void (*delay_ms)(void *ctx, unsigned int ms);
	/* Failure-only control diagnostics. No firmware/calibration payloads.
	 * mask=0 means the transfer failed and actual/expected are unavailable.
	 */
	void (*diagnostic)(void *ctx, const char *stage, u8 device, u16 reg,
			   int error, u32 actual, u32 expected, u32 mask);
};

/* A failed sample leaves both values unknown (-1). Sampling never writes. */
struct en7573_state {
	int md32_enabled;
	int tx_disabled;
};

/* Opaque read-only startup observations, not firmware-health assertions. */
struct en7573_receiver {
	u32 mcu_a0, mcu_a2, apd, ocp, firmware, los_control, system_status;
	u32 rx_output_control, rx_output_shape, ocp_status;
	u32 temperature_raw, supply_raw, apd_voltage_raw, rssi_adc, rssi_current_raw;
};
struct en7573_rx_output {
	u32 control, shape;
	bool saved;
};
struct en7573_oem_post {
	u32 control;
	bool saved;
};
/* Q1000K xponconfig, after PHY/MAC loading: A2 0x110[8] = 1.
 * Meaning is undocumented. Independent TX_DISABLE must remain asserted.
 */
int en7573_oem_post_init(struct en7573_io *io, struct en7573_oem_post *original,
			bool restore);
/* Fixed source-table candidates, never a general register write API.
 * Caller must enforce immutable bench TX inhibition and serialize accesses.
 */
int en7573_apply_rx_output(struct en7573_io *io, unsigned int profile,
			  struct en7573_rx_output *original);
int en7573_restore_rx_output(struct en7573_io *io,
			    struct en7573_rx_output *original);
/* Read-only OEM DDMI and ordinary TX control/status allowlist. A valid zero
 * remains zero; bus failures and all-ones are per-field unavailable. The MCU
 * publishes DDMI asynchronously: a fresh bus read does not prove sensor age.
 */
#define EN7573_TX_FIELDS 22
struct en7573_tx_field { const char *name, *unit; u16 reg; u8 width; u32 scale; };
struct en7573_transmitter { u32 raw[EN7573_TX_FIELDS]; int error[EN7573_TX_FIELDS]; };
extern const struct en7573_tx_field en7573_tx_fields[EN7573_TX_FIELDS];
int en7573_sample_transmitter(struct en7573_io *io, struct en7573_transmitter *sample);
/* Fixed disconnected-bench recipes: 1 restart; 2/3 OEM eye0/1;
 * 4/5 same eyes with Sirherobrine TSSI refresh; 6 BEN forced off.
 * Save all affected words before any write; caller must restore with TX off.
 */
#define EN7573_TX_SAVED 8
struct en7573_tx_recipe { u32 words[EN7573_TX_SAVED]; unsigned int count; };
int en7573_tx_recipe(struct en7573_io *io, const u8 *cal, unsigned int recipe,
                     struct en7573_tx_recipe *saved, bool restore);
int en7573_sample_receiver(struct en7573_io *io, struct en7573_receiver *sample);
/* Published RX power: 0x51:0x0068, BE16 in 0.1 uW units. Zero/saturated
 * words are unavailable (-ENODATA); errors leave the output unchanged.
 * This reads the MCU's measurement, never its calibration command ports.
 */
int en7573_rx_power(struct en7573_io *io, u32 *nanowatts);

int en7573_identify(struct en7573_io *io, u16 *id);
int en7573_read_control(struct en7573_io *io, u16 reg, u32 *value);
int en7573_sample_state(struct en7573_io *io, struct en7573_state *state);
int en7573_load(struct en7573_io *io, const u8 *pm, size_t pm_size,
		const u8 *dm, size_t dm_size, const u8 *cal);
int en7573_start_tx_disabled(struct en7573_io *io);
/* Caller serializes controller access and verifies firmware is running. */
int en7573_set_tx(struct en7573_io *io, bool enable);

#endif
