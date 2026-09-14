/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_MAC_COLD_H_
#define _Q1000K_MAC_COLD_H_
#include <linux/types.h>
/* Only the owning CLEAR callback, after exclusive reset and physical drain. */
int q1000k_mac_cold_release(void);
/* Fixed AN7581 XGS-PON discovery defaults, full identity and invalid data keys.
 * INSTALL only. Integrity banks must be installed before selecting bank one.
 * This does not enable controller TX, packet queues or protocol interrupts.
 */
int q1000k_mac_cold_install(const u8 serial[8], const u8 registration[36], bool emergency);
int q1000k_mac_cold_select_keys(void);
int q1000k_mac_cold_interrupts(u32 enables);
int q1000k_mac_profiles_invalidate(void);
int q1000k_mac_profile_install(u8 index, u8 version, u16 length);
/* Protocol-owner state publication. Unsupported NG-PON2/fast-resume states
 * are rejected. The caller publishes software state only after success.
 */
int q1000k_mac_activation_set(u8 state);
#endif
