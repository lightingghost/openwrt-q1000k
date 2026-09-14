/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PHY_API_H
#define _Q1000K_PHY_API_H

#include <linux/types.h>

struct xpon_phy_api_data_s;

/* Process context only. Symbols pin phy_10g for the MAC's lifetime. */
int q1000k_phy_configure(u32 mode);
int q1000k_phy_start(void);
int q1000k_phy_stop(void);
/* Final coordinated optical TX enable/disable, after successful PHY start. */
int q1000k_phy_set_tx(bool enable);
int q1000k_phy_call(struct xpon_phy_api_data_s *data);
/* Timer callback only queues work; PHY polling runs in process context. */
void q1000k_phy_poll(void);
int q1000k_phy_init(void);
void q1000k_phy_exit(void);

/* Private to phy_10g: verify process context and callback mutex ownership. */
int q1000k_phy_callback_context(void);
int q1000k_phy_top_reset(void);
int q1000k_phy_pma_reset(void);
int q1000k_phy_controller_check(void);
int q1000k_phy_board_profile(void);
int q1000k_phy_trans_power(u32 operation);

#endif
