/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_PON_PHY_H
#define _AN7581_PON_PHY_H

#include <linux/types.h>

/* Physical addresses or the exact legacy KSEG1 aliases are accepted. */
int an7581_pon_phy_read(u32 reg, u32 *value);
int an7581_pon_phy_write(u32 reg, u32 value);
int an7581_pon_phy_update(u32 reg, u32 end, u32 start, u32 value);
/* A failed legacy access is sticky; initialization must check this status. */
int an7581_pon_phy_status(void);

#endif
