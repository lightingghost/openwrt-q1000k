/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_OMCI_BACKEND_H_
#define _Q1000K_OMCI_BACKEND_H_
#include <linux/types.h>
struct net_device;
struct sk_buff;
struct q1000k_pon_profile;
int q1000k_omci_backend_init(struct net_device *dev);
/* Module owner only: after hooks/ready publication, before protocol start. */
int q1000k_omci_cold_start(void);
/* Deferred cold reset: only protocol owner, no core barrier in this caller. */
int q1000k_omci_reset(bool emergency, bool reset_phy);
/* Authenticate a complete downstream wire PLOAM before any handler runs.
 * Broadcast uses the default integrity key; directed registration/operational
 * traffic requires the current ONU's installed registration-derived bank.
 */
int q1000k_omci_ploam_verify(const u8 *message, size_t length);
extern u32 q1000k_ploam_rejected;
extern int q1000k_ploam_last_error;
/* Protocol stopped and RX admission closed before cleanup. */
void q1000k_omci_backend_cleanup(void);
/* These request functions require protocol ownership and copy their inputs. */
int q1000k_omci_profile(const u8 tag[8], u8 sequence, bool acknowledge);
int q1000k_omci_burst_profile(const struct q1000k_pon_profile *profile,
			    const u8 tag[8], u8 sequence, bool acknowledge);
int q1000k_omci_assign(u16 onu);
/* Verify the hardware registration-key transition before publishing it. */
int q1000k_omci_registration_keys(void);
int q1000k_omci_ranging(u32 delay, bool absolute, bool negative, u8 sequence, bool acknowledge);
int q1000k_omci_ranged(void);
int q1000k_omci_alloc_changed(void);
void q1000k_omci_state(void);
/* Ordered protocol control callback, invoked without the executor mutex. */
void q1000k_omci_control(void);
/* NAPI/RCU receive; consumes every skb, including rejected PDUs. */
void q1000k_omci_receive(struct sk_buff *skb, u16 gem, bool crc_error);
#endif
