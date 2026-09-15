/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PHY_API_H
#define _Q1000K_PHY_API_H

#include <linux/types.h>

struct xpon_phy_api_data_s;

/* One XGS burst profile shared by the MAC and optical PHY install owners. */
struct q1000k_pon_profile {
	u8 preamble[8], delimiter[8];
	u8 index, version, repeat, preamble_len, delimiter_len, fec;
};

static inline bool q1000k_pon_profile_valid(const struct q1000k_pon_profile *p)
{
	return p && p->index < 4 && p->version < 16 && p->fec < 2 &&
		p->repeat && p->preamble_len && p->preamble_len <= 8 &&
		p->delimiter_len && p->delimiter_len <= 8;
}

/* Process context only. Wait for active PHY callbacks; recursion from this
 * PHY's callback returns EDEADLK before taking any control lock. Symbols pin
 * phy_10g for the MAC's lifetime. PHY callbacks must only enqueue MAC events.
 */
int q1000k_phy_configure(u32 mode);
/* Lifecycle owner only, after native attachment and CPU/DMA pause. Select
 * XGS-PON from the known USXGMII boot handoff with verified controller TX off.
 * An already selected XGS-PON path is unchanged; unknown/active modes fail.
 */
int q1000k_phy_prepare_wan(void);
int q1000k_phy_start(void);
int q1000k_phy_stop(void);
/* Lifecycle owner: wait for callbacks; rejects invocation by a callback. */
int q1000k_phy_quiesce(void);
/* Final coordinated optical TX enable/disable, after successful PHY start. */
int q1000k_phy_set_tx(bool enable);
/* Wait for callbacks and verify the controller before snapshotting TX. */
int q1000k_phy_get_tx(bool *enabled);
/* The physical pipeline must have quiesced PHY callbacks and TX first. */
int q1000k_phy_profile_set(const struct q1000k_pon_profile *profile);
int q1000k_phy_call(struct xpon_phy_api_data_s *data);
/* Timer callback only queues work; PHY polling runs in process context. */
void q1000k_phy_poll(void);
int q1000k_phy_init(void);
void q1000k_phy_exit(void);

/* Private to phy_10g: verify process context and callback mutex ownership. */
int q1000k_phy_callback_context(void);
int q1000k_phy_top_reset(void);
int q1000k_phy_pma_reset(void);
int q1000k_phy_pma_init(void);
int q1000k_phy_controller_check(void);
int q1000k_phy_board_profile(void);
int q1000k_phy_trans_power(u32 operation);

#endif
