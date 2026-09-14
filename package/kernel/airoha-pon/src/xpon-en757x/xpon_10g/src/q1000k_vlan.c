// SPDX-License-Identifier: GPL-2.0-only
/* G.988 class 171: compile tag operations before publishing a service table. */
#include <linux/errno.h>
#include <linux/string.h>
#include <net/xpon/omci.h>
#include "common/q1000k_vlan.h"

static bool qv_tpid(u16 tpid)
{
	return tpid == 0x8100 || tpid == 0x88a8 || tpid == 0x9100;
}

static int qv_filter(struct q1000k_vlan_match *m, u8 pbit, u16 vid,
		     u8 mode, u16 tpid)
{
	if (pbit <= 7) { m->value[1] = pbit << 13; m->mask[1] = 0xe000; }
	else if (pbit != 8 && pbit != 14) return -EINVAL;
	/* A default rule ignores its tag filter fields. */
	if (pbit == 14)
		return 0;
	if (vid <= 4094) { m->value[1] |= vid; m->mask[1] |= 0x0fff; }
	else if (vid != 4096) return -EINVAL;
	if (!mode) return 0;
	if (mode < 4 || mode > 7) return -EINVAL;
	if (mode != 4 && !qv_tpid(tpid)) return -EOPNOTSUPP;
	m->mask[0] = 0xffff;
	m->value[0] = mode == 4 ? 0x8100 : tpid;
	if (mode >= 6) { m->mask[1] |= 0x1000; m->value[1] |= (mode & 1) << 12; }
	return 0;
}

static int qv_copy(struct q1000k_vlan_expr *e, unsigned int count,
		   bool outer, unsigned int word, u16 mask)
{
	if (!count || (outer && count < 2))
		return -EINVAL;
	e->copy[outer ? 0 : count - 1][word] |= mask;
	return 0;
}

static int qv_output(struct q1000k_vlan_expr *e, unsigned int count,
		     u8 pbit, u16 vid, u8 mode, u16 tpid)
{
	int ret;

	if (pbit <= 7) e->value[1] = pbit << 13;
	else if (pbit == 8 || pbit == 9) {
		ret = qv_copy(e, count, pbit == 9, 1, 0xe000);
		if (ret) return ret;
	} else return -EOPNOTSUPP; /* DSCP mapping requires its own classifier. */
	if (vid <= 4094) e->value[1] |= vid;
	else if (vid == 4096 || vid == 4097) {
		ret = qv_copy(e, count, vid == 4097, 1, 0x0fff);
		if (ret) return ret;
	} else return -EINVAL;
	if (mode == 5 || mode > 7) return -EINVAL;
	if (mode != 0 && mode != 1 && mode != 4 && !qv_tpid(tpid))
		return -EOPNOTSUPP;
	if (mode < 2) return qv_copy(e, count, mode == 1, 0, 0xffff) ?:
		qv_copy(e, count, mode == 1, 1, 0x1000);
	e->value[0] = mode == 4 ? 0x8100 : tpid;
	if (mode == 2 || mode == 3) return qv_copy(e, count, mode == 3, 1, 0x1000);
	if (mode == 7) e->value[1] |= 0x1000;
	return 0;
}

static void qv_inverse(struct q1000k_vlan_program *p)
{
	struct q1000k_vlan_direction *up = &p->up, *down = &p->down;
	unsigned int i, j, w;

	down->input_count = up->output_count;
	down->output_count = up->input_count;
	for (i = 0; i < up->output_count; i++)
		for (w = 0; w < 2; w++) {
			const struct q1000k_vlan_expr *e = &up->output[i];
			u16 copied = e->copy[0][w] | e->copy[1][w];

			down->match[i].mask[w] = ~copied;
			down->match[i].value[w] = e->value[w];
			for (j = 0; j < up->input_count; j++) {
				u16 constrained = e->copy[j][w] & up->match[j].mask[w];

				down->match[i].mask[w] |= constrained;
				down->match[i].value[w] |= up->match[j].value[w] & constrained;
			}
		}
	for (i = 0; i < up->input_count; i++)
		for (w = 0; w < 2; w++) {
			struct q1000k_vlan_expr *e = &down->output[i];
			u16 remaining = ~up->match[i].mask[w];

			e->value[w] = up->match[i].value[w];
			for (j = 0; j < up->output_count; j++) {
				u16 copied = up->output[j].copy[i][w] & remaining;

				e->copy[j][w] |= copied;
				remaining &= ~copied;
			}
			/* A removed wildcard field uses the lowest legal value. */
			if (!w) e->value[w] |= 0x8100 & remaining;
		}
}

int q1000k_vlan_compile(const struct omci_service_config *s,
			struct q1000k_vlan_program *program)
{
	struct q1000k_vlan_program p = {};
	const struct omci_extended_vlan_rule *r;
	struct q1000k_vlan_direction *up = &p.up;
	unsigned int added, n, i;
	int ret;

	if (!s || !program || !s->vlan_treatment_valid)
		return -EINVAL;
	r = &s->vlan_rule;
	if (r->delete || r->filter_ethertype > 5) return -EINVAL;
	if (s->vlan_downstream_mode > 1) return -EOPNOTSUPP;
	p.downstream_passthrough = s->vlan_downstream_mode == 1;
	p.ethertype = r->filter_ethertype;
	n = r->filter_inner_pbit == 15 ? 0 : r->filter_outer_pbit == 15 ? 1 : 2;
	if (!n && r->filter_outer_pbit != 15) return -EINVAL;
	up->input_count = n;
	p.fallback = (n == 1 && r->filter_inner_pbit == 14) ||
		(n == 2 && r->filter_outer_pbit == 14);
	if (n == 2) {
		ret = qv_filter(&up->match[0], r->filter_outer_pbit, r->filter_outer_vid,
				r->filter_outer_tpid_dei, s->vlan_input_tpid);
		if (ret) return ret;
	}
	if (n) {
		ret = qv_filter(&up->match[n - 1], r->filter_inner_pbit, r->filter_inner_vid,
				r->filter_inner_tpid_dei, s->vlan_input_tpid);
		if (ret) return ret;
	}
	p.drop = r->tags_to_remove == 3;
	if (p.drop) { *program = p; return 0; }
	if (r->tags_to_remove > n) return -EINVAL;
	added = r->treat_inner_pbit == 15 ? 0 : r->treat_outer_pbit == 15 ? 1 : 2;
	if (!added && r->treat_outer_pbit != 15) return -EINVAL;
	if (n - r->tags_to_remove + added > 2) return -EOPNOTSUPP;
	up->output_count = n - r->tags_to_remove + added;
	if (added == 2) {
		ret = qv_output(&up->output[0], n, r->treat_outer_pbit, r->treat_outer_vid,
				r->treat_outer_tpid_dei, s->vlan_output_tpid);
		if (ret) return ret;
	}
	if (added) {
		ret = qv_output(&up->output[added - 1], n, r->treat_inner_pbit, r->treat_inner_vid,
				r->treat_inner_tpid_dei, s->vlan_output_tpid);
		if (ret) return ret;
	}
	for (i = r->tags_to_remove; i < n; i++) {
		struct q1000k_vlan_expr *e = &up->output[added + i - r->tags_to_remove];

		e->copy[i][0] = e->copy[i][1] = 0xffff;
	}
	qv_inverse(&p);
	*program = p;
	return 0;
}

static bool qv_ethertype(u8 filter, u16 type)
{
	switch (filter) {
	case 0: return true;
	case 1: return type == 0x0800;
	case 2: return type == 0x8863 || type == 0x8864;
	case 3: return type == 0x0806;
	case 4: return type == 0x86dd;
	case 5: return type == 0x888e;
	default: return false;
	}
}

int q1000k_vlan_apply(const struct q1000k_vlan_program *p, bool upstream,
		      const struct q1000k_vlan_frame *input,
		      struct q1000k_vlan_frame *output)
{
	const struct q1000k_vlan_direction *d;
	struct q1000k_vlan_frame result = {};
	unsigned int i, j, w;
	u16 in[2][2] = {}, out[2][2] = {};

	if (!p || !input || !output || input->count > 2) return -EINVAL;
	if (!upstream && p->downstream_passthrough) { *output = *input; return 0; }
	if (!upstream && (p->fallback || p->drop)) return -ENOENT;
	d = upstream ? &p->up : &p->down;
	if (input->count != d->input_count || !qv_ethertype(p->ethertype, input->ethertype))
		return -ENOENT;
	for (i = 0; i < input->count; i++) {
		in[i][0] = input->tag[i].tpid; in[i][1] = input->tag[i].tci;
		for (w = 0; w < 2; w++)
			if ((in[i][w] & d->match[i].mask[w]) != d->match[i].value[w])
				return -ENOENT;
	}
	if (p->drop) return -EPERM;
	for (i = 0; i < d->output_count; i++) {
		for (w = 0; w < 2; w++) {
			out[i][w] = d->output[i].value[w];
			for (j = 0; j < input->count; j++)
				out[i][w] |= in[j][w] & d->output[i].copy[j][w];
		}
		result.tag[i].tpid = out[i][0]; result.tag[i].tci = out[i][1];
	}
	result.count = d->output_count; result.ethertype = input->ethertype;
	if (!upstream) {
		struct q1000k_vlan_frame roundtrip;

		/* Copied fields may occur in more than one output tag. Both
		 * occurrences must agree before accepting an inverse mapping.
		 */
		if (q1000k_vlan_apply(p, true, &result, &roundtrip) ||
		    roundtrip.count != input->count)
			return -ENOENT;
		for (i = 0; i < input->count; i++)
			if (roundtrip.tag[i].tpid != input->tag[i].tpid ||
			    roundtrip.tag[i].tci != input->tag[i].tci)
				return -ENOENT;
	}
	*output = result;
	return 0;
}
