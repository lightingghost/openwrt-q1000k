/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PON_CONTROL_H
#define _Q1000K_PON_CONTROL_H
#include <linux/types.h>

struct q1000k_pon;

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
int q1000k_pon_get_los(struct q1000k_pon *pon);
int q1000k_pon_check(struct q1000k_pon *pon);
#endif
