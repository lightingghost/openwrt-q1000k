/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_OMCI_H_
#define _Q1000K_OMCI_H_

#include <linux/skbuff.h>

/* Validate a complete outgoing raw OMCI frame, remove its optional four-byte
 * trailer and reserve writable tailroom for a replacement MIC. This does not
 * authenticate the supplied trailer or generate a MIC. The caller retains
 * ownership on all returns; a failed call may have linearized the buffer.
 */
int q1000k_omci_tx_prepare(struct sk_buff *skb);

#endif
