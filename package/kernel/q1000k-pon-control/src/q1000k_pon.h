/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PON_CONTROL_H
#define _Q1000K_PON_CONTROL_H
#include <linux/types.h>

struct q1000k_pon;
#ifndef Q1000K_EN7573_H
#define EN7573_TX_SAVED 8
struct en7573_tx_recipe { u32 words[EN7573_TX_SAVED]; unsigned int count; };
#endif
int q1000k_pon_tx_recipe(struct q1000k_pon *pon, unsigned int recipe,
                        struct en7573_tx_recipe *saved, bool restore);

/* Process context only. Acquisition requires verified firmware/calibration
 * already loaded and TX disabled. Exactly one consumer owns the controller.
 * An acquired reference survives device removal; calls then return -ENODEV.
 */
struct q1000k_pon *q1000k_pon_get(void);
int q1000k_pon_put(struct q1000k_pon *pon);
int q1000k_pon_set_tx(struct q1000k_pon *pon, bool enable);
/* Verify controller state before returning its current optical TX state. */
int q1000k_pon_get_tx(struct q1000k_pon *pon, bool *enabled);
/* Read the immutable probe-time TX inhibit after verifying the lease/health. */
int q1000k_pon_get_tx_inhibit(struct q1000k_pon *pon, bool *inhibited);
/* Controller-reported RX power; no registration required. ENODATA means the
 * MCU has not published a usable reading. Other errors invalidate the sample.
 */
int q1000k_pon_get_rx_power(struct q1000k_pon *pon, u32 *nanowatts);
int q1000k_pon_get_los(struct q1000k_pon *pon);
int q1000k_pon_check(struct q1000k_pon *pon);
/* Immutable TX-inhibited bench only; exact OEM post-module bit, one saved
 * field. Caller holds the controller lease throughout probe and restoration.
 */
int q1000k_pon_oem_post_init(struct q1000k_pon *pon, bool restore);
/* Lifecycle owner after PHY/MAC initialization, before TX/protocol activation.
 * Idempotent within one controller lifetime; disabled on the old RX bench DT.
 */
int q1000k_pon_receiver_startup(struct q1000k_pon *pon);
/* Exclusive PHY lease owner, after callback/IRQ/DMA drain, RX-inhibited
 * activation bench only. Reverify the original MCU pair and retained DSD. */
int q1000k_pon_bench_reinitialize(struct q1000k_pon *pon);
#endif
