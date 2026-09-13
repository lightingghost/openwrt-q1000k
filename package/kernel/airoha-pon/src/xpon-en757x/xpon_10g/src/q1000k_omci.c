// SPDX-License-Identifier: GPL-2.0-only
/* Q1000K raw OMCI TX framing, before the imported MAC/MIC implementation. */
#include <linux/errno.h>
#include <linux/if_vlan.h>
#include "common/q1000k_omci.h"

int q1000k_omci_tx_prepare(struct sk_buff *skb)
{
	unsigned int len;
	int extra;

	if (!skb || skb_shared(skb) || skb->len < 10 || skb->len > 16128)
		return -EINVAL;
	if (skb_is_gso(skb) || skb->ip_summed == CHECKSUM_PARTIAL ||
	    skb_vlan_tag_present(skb))
		return -EOPNOTSUPP;
	if (skb_linearize(skb))
		return -ENOMEM;

	switch (skb->data[3]) {
	case 0x0a:
		len = 44;
		break;
	case 0x0b:
		len = 10 + ((unsigned int)skb->data[8] << 8) + skb->data[9];
		break;
	default:
		return -EPROTONOSUPPORT;
	}
	/* Only a complete payload, optionally followed by one MIC, is accepted.
	 * Do not truncate unknown extra bytes or allow 16-bit length wraparound.
	 */
	if (len > 16128 - 4 || (skb->len != len && skb->len != len + 4))
		return -EMSGSIZE;

	/* Space released by trimming the old MIC can be reused. Unshare cloned
	 * data even when it already has sufficient space for the replacement.
	 */
	extra = max_t(int, 4 - skb_tailroom(skb) - (int)(skb->len - len), 0);
	if ((extra || skb_cloned(skb)) &&
	    pskb_expand_head(skb, 0, extra, GFP_ATOMIC))
		return -ENOMEM;
	skb_trim(skb, len);
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}
