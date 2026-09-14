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

/* Process context only, outside RTNL and RCU. Native scheduler commands may
 * sleep; the adapter mutex pins the attachment across the entire operation.
 * Both operations require closed queues and reclaimed native mappings.
 */
int q1000k_transport_get_port_config(struct airoha_pon_port_config *config);
int q1000k_transport_configure_port(const struct airoha_pon_port_config *expected,
		const struct airoha_pon_port_config *config);
int q1000k_transport_set_qos(u8 channel, const struct airoha_pon_qos *qos);
int q1000k_transport_get_qos(u8 channel, struct airoha_pon_qos *qos);

/* Process-context lifecycle stages, outside RTNL/RCU. The caller serializes
 * the complete operation with MAC provisioning. Pause drains CPU mappings
 * without detaching RX. retire_fe requires a completed pause and only retires
 * FE/QDMA; MAC/optical and RX draining remain mandatory before ID reuse.
 * Resume restores eligible saved queues only after the caller's MAC work.
 */
int q1000k_transport_pause(unsigned int timeout_ms);
int q1000k_transport_retire_fe(u8 channel);
/* After all FE channels retire and MAC ingress stops; leaves RX DMA closed. */
int q1000k_transport_drain_rx(void);
/* Call only while MAC ingress/egress is held stopped, after old tables are
 * removed. Activate only after replacement tables and channels are verified. */
int q1000k_transport_reset_epoch(void);
int q1000k_transport_activate_rx(u32 channels);
int q1000k_transport_resume(void);

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

/* Process context. Zero closes OMCI admission and waits for any current native
 * submission before purging old software retries. A nonzero epoch must be
 * strictly newer than any previously published epoch on this attachment.
 * Does not drain descriptors already accepted by native DMA or optical FIFOs.
 * Call outside backend locks acquired by RX/TX callbacks.
 */
int q1000k_transport_set_auth_epoch(u64 auth_epoch);
/* The skb must already carry its MIC for this exact epoch. Ownership and
 * retry expiry follow xmit; no automatic key-generation refresh is allowed.
 */
int q1000k_transport_xmit_omci(struct sk_buff *skb, u16 gem, u8 mic_index,
			     u64 auth_epoch);

#endif
