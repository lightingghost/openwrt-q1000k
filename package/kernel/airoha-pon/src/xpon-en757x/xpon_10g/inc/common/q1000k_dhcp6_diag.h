/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q1000K_DHCP6_DIAG_H
#define Q1000K_DHCP6_DIAG_H

/* Header-only, bounded recognizer shared with the diagnostic fixture. No
 * address, client identifier, transaction ID or option content is retained.
 */
#define Q6D_HEADER_BYTES 128
struct q6d_sample {
	u16 gem, vlan[2], length;
	u8 tags, type, tx;
};

static inline unsigned int q6d_be16(const u8 *p)
{
	return (unsigned int)p[0] << 8 | p[1];
}

struct q6d_l2 {
	u16 outer, proto, vlan[2];
	u8 tags, length;
};

static inline bool q6d_parse_l2(const u8 *p, unsigned int captured,
		struct q6d_l2 *header)
{
	struct q6d_l2 h = { .length = 14 };

	if (captured < h.length)
		return false;
	h.outer = h.proto = q6d_be16(p + 12);
	while (h.proto == 0x8100 || h.proto == 0x88a8) {
		if (h.tags == 2 || captured < h.length + 4U)
			return false;
		h.vlan[h.tags++] = q6d_be16(p + h.length);
		h.proto = q6d_be16(p + h.length + 2);
		h.length += 4;
	}
	*header = h;
	return true;
}

static inline bool q6d_parse(const u8 *p, unsigned int captured,
		unsigned int length, struct q6d_sample *sample)
{
	struct q6d_sample s = { .gem = 0xffff };
	struct q6d_l2 l2;
	unsigned int off, payload, end, next, count = 0, size;

	if (length < captured || !q6d_parse_l2(p, captured, &l2))
		return false;
	off = l2.length;
	s.tags = l2.tags;
	s.vlan[0] = l2.vlan[0];
	s.vlan[1] = l2.vlan[1];
	if (l2.proto != 0x86dd || captured < off + 40 || p[off] >> 4 != 6)
		return false;
	payload = q6d_be16(p + off + 4);
	end = off + 40 + payload;
	if (!payload || end > length)
		return false;
	next = p[off + 6];
	off += 40;
	while (next == 0 || next == 43 || next == 60) {
		if (++count > 4 || captured < off + 2 || end < off + 2)
			return false;
		size = ((unsigned int)p[off + 1] + 1) * 8;
		next = p[off];
		if (size > end - off || size > captured - off)
			return false;
		off += size;
	}
	/* Fragmented and encrypted headers are deliberately outside this probe. */
	if (next != 17 || captured < off + 9 || end < off + 9)
		return false;
	size = q6d_be16(p + off + 4);
	if (size < 9 || size > end - off)
		return false;
	if (q6d_be16(p + off) == 546 && q6d_be16(p + off + 2) == 547)
		s.tx = 1;
	else if (q6d_be16(p + off) != 547 || q6d_be16(p + off + 2) != 546)
		return false;
	s.type = p[off + 8];
	if (!s.type)
		return false;
	s.length = length > 0xffff ? 0xffff : length;
	*sample = s;
	return true;
}

enum q6d_stage {
	Q6D_TX_SELECT, Q6D_TX_SERVICE, Q6D_TX_NATIVE,
	Q6D_RX_PRE, Q6D_RX_SELECT, Q6D_RX_CONSUMER, Q6D_STAGES
};

#ifdef __KERNEL__
struct sk_buff;
bool q1000k_dhcp6_sample(const struct sk_buff *skb, struct q6d_sample *sample);
void q1000k_dhcp6_record(enum q6d_stage stage,
		const struct q6d_sample *sample, int result);
#endif
#endif
