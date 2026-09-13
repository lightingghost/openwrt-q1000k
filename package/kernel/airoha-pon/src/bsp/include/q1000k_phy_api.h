/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PHY_API_H
#define _Q1000K_PHY_API_H

#include <linux/types.h>

struct xpon_phy_api_data_s;

/* Process context only. Symbols pin phy_10g for the MAC's lifetime. */
int q1000k_phy_configure(u32 mode);
int q1000k_phy_start(void);
int q1000k_phy_stop(void);
int q1000k_phy_call(struct xpon_phy_api_data_s *data);
/* Timer callback only queues work; PHY polling runs in process context. */
void q1000k_phy_poll(void);
int q1000k_phy_init(void);
void q1000k_phy_exit(void);

#endif
