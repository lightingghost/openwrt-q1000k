// SPDX-License-Identifier: GPL-2.0-only
/* Bounds and skb contract for the Q1000K native packet adapter. */
#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "common/q1000k_packet.h"

int q1000k_pwan_rx_prepare(struct sk_buff *skb, const void *msg,
			   unsigned int msg_len, unsigned int packet_len)
{
	u32 words[4];
	unsigned int needed;

	if (!skb || !msg || msg_len < sizeof(words) || !packet_len ||
	    packet_len > 16128 || skb->len != packet_len)
		return -EINVAL;
	memcpy(words, msg, sizeof(words));
	if ((words[0] & GENMASK(13, 11)) || (words[2] >> 24) > 1)
		return -EBADMSG;
	/* Legacy parsers dereference skb->data directly, including CMAC input. */
	if (skb_linearize(skb))
		return -ENOMEM;
	if (words[0] & BIT(8)) {
		/* Bounds for the imported parser, not an authentication verdict.
		 * Baseline CMAC covers 44 bytes. Extended length is at bytes 8/9,
		 * followed by the content after the 10-byte header. The no-MIC
		 * descriptor flag selects its software verifier, which reads four
		 * additional bytes. Never let its u16 length calculation wrap.
		 */
		if (packet_len < 10)
			return -EMSGSIZE;
		switch (skb->data[3]) {
		case 0x0a:
			needed = 44;
			break;
		case 0x0b:
			needed = 10 + ((unsigned int)skb->data[8] << 8) + skb->data[9];
			break;
		default:
			return -EPROTONOSUPPORT;
		}
		if (words[0] & BIT(30))
			needed += 4;
		if (needed > packet_len)
			return -EMSGSIZE;
	} else if (packet_len < ETH_HLEN) {
		return -EMSGSIZE;
	}
	/* The explicit descriptor metadata does not live in the skb control area. */
	memset(skb->cb, 0, sizeof(skb->cb));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

void q1000k_pwan_rx_protocol(struct sk_buff *skb, struct net_device *dev,
			    bool omci)
{
	skb->dev = dev;
	skb->ip_summed = CHECKSUM_NONE;
	if (omci) {
		skb_reset_mac_header(skb);
		skb_reset_network_header(skb);
		skb->pkt_type = PACKET_HOST;
		skb->protocol = 0;
	} else {
		skb->protocol = eth_type_trans(skb, dev);
	}
}
