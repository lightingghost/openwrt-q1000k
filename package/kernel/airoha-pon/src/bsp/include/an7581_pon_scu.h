/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_PON_SCU_H
#define _AN7581_PON_SCU_H
#include <linux/types.h>

/* Only the PON lifecycle may change the shared WAN selector. */
int an7581_pon_wan_get(u32 *mode);
int an7581_pon_wan_set(u32 mode);
int an7581_pon_pbus_enable(void);
#endif
