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

/* Control/address registers use 0x51, memory data ports use 0x50.
 * Register addresses are big endian, register words and memory are little
 * endian. Transport callbacks return zero only for a complete transaction.
 */
struct en7573_io {
	void *ctx;
	int (*read)(void *ctx, u8 device, u16 reg, u8 *data, size_t len);
	int (*write)(void *ctx, u8 device, u16 reg, const u8 *data, size_t len);
	void (*delay_ms)(void *ctx, unsigned int ms);
};

int en7573_identify(struct en7573_io *io, u16 *id);
int en7573_read_control(struct en7573_io *io, u16 reg, u32 *value);
int en7573_load(struct en7573_io *io, const u8 *pm, size_t pm_size,
		const u8 *dm, size_t dm_size, const u8 *cal);
int en7573_start_tx_disabled(struct en7573_io *io);

#endif
