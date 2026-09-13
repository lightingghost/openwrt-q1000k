/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_XPON_H
#define _AN7581_XPON_H

#include <linux/types.h>

struct device;

/* Borrowed device: consumers must hold this provider's module dependency. */
struct device *get_xpon_dev(void);
int get_xpon_irq(int index);
u32 get_xpon_data(u32 reg);
void set_xpon_data(u32 reg, u32 value);

#endif
