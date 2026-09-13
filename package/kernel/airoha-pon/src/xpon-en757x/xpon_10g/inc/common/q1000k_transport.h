/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PON_TRANSPORT_H_
#define _Q1000K_PON_TRANSPORT_H_

#include <linux/netdevice.h>
#include <linux/soc/airoha/airoha_pon.h>

typedef int (*q1000k_pon_receive_t)(void *msg, unsigned int msg_len,
				    struct sk_buff *skb, unsigned int len);

/* Process context; do not hold RTNL. The RX function consumes every skb and
 * remains valid until stop returns. No callback survives stop, even when its
 * return value reports a native TX reclamation timeout. Neither function
 * starts/stops shared DMA or operates the MAC/PHY, clocks or optical pins.
 */
int q1000k_transport_start(const char *lower, q1000k_pon_receive_t receive);
int q1000k_transport_stop(void);
bool q1000k_transport_running(void);

/* IRQ-safe; no consumer callback lock may be held. One bit per queue, with
 * all queues closed on attachment. Opening requires the MAC's provisioning
 * sequence; closing does not retire previously submitted hardware traffic.
 */
int q1000k_transport_set_queue_close(u8 channel, u8 closed);
int q1000k_transport_get_queue_close(u8 channel, u8 *closed);

/* Verified native GDM2 TX channel control. Enable requires closed queues
 * and reclaimed native mappings. Disable permanently closes admission first.
 * This does not implement RX control or FE/optical FIFO retirement.
 */
int q1000k_transport_set_tx_channel(u8 channel, bool enabled);

/* Permanently close this attachment's channel and poll native TX mappings.
 * IRQ-safe; -EAGAIN means mappings remain. Zero does not prove FE/optical
 * FIFO retirement. No queue reopening is available after this operation.
 */
int q1000k_transport_quiesce_channel(u8 channel);

/* Zero transfers ownership to the bounded retry queue. Negative errno leaves
 * skb unchanged and owned by the caller. Never returns NETDEV_TX_BUSY: the
 * vendor has already modified the skb by this point. Accepted packets may be
 * dropped on expiry, channel closure, lower detach or shutdown. Metadata and
 * the native admission epoch are captured once, outside cb, before enqueue.
 */
int q1000k_transport_xmit(struct sk_buff *skb, u32 word0, u32 word1);

#endif
