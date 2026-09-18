/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q1000K_VLAN_H
#define Q1000K_VLAN_H
#include <linux/types.h>
struct omci_service_config;
struct omci_vlan_tagging_filter;
/* Host-endian tag words, ordered outermost first. */
struct q1000k_vlan_tag { u16 tpid, tci; };
struct q1000k_vlan_frame {
	struct q1000k_vlan_tag tag[2];
	u16 ethertype;
	u8 count;
};
struct q1000k_vlan_match { u16 value[2], mask[2]; };
struct q1000k_vlan_expr { u16 value[2], copy[2][2]; };
struct q1000k_vlan_direction {
	struct q1000k_vlan_match match[2];
	struct q1000k_vlan_expr output[2];
	u8 input_count, output_count;
};
struct q1000k_vlan_program {
	struct q1000k_vlan_direction up, down;
	u8 ethertype;
	bool drop, fallback, downstream_passthrough;
};
int q1000k_vlan_compile(const struct omci_service_config *service,
			struct q1000k_vlan_program *program);
/* 0: strict copy validation; 1: absent untagged DEI is zero; 2: NAND rule
 * normalization (inner treatment uses output TPID/DEI zero, except mode 4).
 */
int q1000k_vlan_compile_policy(const struct omci_service_config *service,
			struct q1000k_vlan_program *program, unsigned int untagged_policy);
int q1000k_vlan_apply(const struct q1000k_vlan_program *program, bool upstream,
		      const struct q1000k_vlan_frame *input,
		      struct q1000k_vlan_frame *output);
int q1000k_vlan_filter_validate(const struct omci_vlan_tagging_filter *filter);
int q1000k_vlan_filter_apply(const struct omci_vlan_tagging_filter *filter,
			    bool ingress, const struct q1000k_vlan_frame *frame);
#endif
