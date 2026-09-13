/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_GWAN_H_
#define _Q1000K_GWAN_H_

#include <linux/types.h>

struct q1000k_gwan_binding {
	u16 gem;
	u16 alloc_id;
	u16 ani;
	u16 index;
	u8 channel;
	bool multicast;
};

/* One coherent data GEM/T-CONT snapshot; outputs are unchanged on failure.
 * Active bindings cannot be reassigned or reused before physical retirement.
 * Removal closes native admission permanently for the associated channel.
 * No callbacks, MMIO or packet submission occur while the state lock is held.
 */
int q1000k_gwan_binding(u16 gem, bool tx, struct q1000k_gwan_binding *binding);
void q1000k_gwan_account(u16 gem, bool tx, unsigned int bytes);

#endif
