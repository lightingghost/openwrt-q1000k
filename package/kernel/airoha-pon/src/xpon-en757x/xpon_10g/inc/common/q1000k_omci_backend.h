/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_OMCI_BACKEND_H_
#define _Q1000K_OMCI_BACKEND_H_
#include <linux/types.h>
struct net_device;
struct sk_buff;
int q1000k_omci_backend_init(struct net_device *dev);
/* Module owner only: after hooks/ready publication, before protocol start. */
int q1000k_omci_cold_start(void);
/* Deferred cold reset: only protocol owner, no core barrier in this caller. */
int q1000k_omci_reset(bool emergency, bool reset_phy);
/* Protocol stopped and RX admission closed before cleanup. */
void q1000k_omci_backend_cleanup(void);
/* These request functions require protocol ownership and copy their inputs. */
int q1000k_omci_profile(const u8 tag[8], u8 sequence, bool acknowledge);
int q1000k_omci_assign(u16 onu);
int q1000k_omci_alloc_changed(void);
void q1000k_omci_state(void);
/* Ordered protocol control callback, invoked without the executor mutex. */
void q1000k_omci_control(void);
/* NAPI/RCU receive; consumes every skb, including rejected PDUs. */
void q1000k_omci_receive(struct sk_buff *skb, u16 gem, bool crc_error);
#endif
