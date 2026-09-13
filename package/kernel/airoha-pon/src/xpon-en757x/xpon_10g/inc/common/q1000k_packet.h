/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PON_PACKET_H_
#define _Q1000K_PON_PACKET_H_

#include <linux/netdevice.h>
#include <linux/skbuff.h>

/* Native RX already owns a populated skb. Errors leave ownership with caller. */
int q1000k_pwan_rx_prepare(struct sk_buff *skb, const void *msg,
			   unsigned int msg_len, unsigned int packet_len);
void q1000k_pwan_rx_protocol(struct sk_buff *skb, struct net_device *dev,
			    bool omci);

#endif
