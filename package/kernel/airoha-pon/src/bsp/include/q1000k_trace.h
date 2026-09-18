/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q1000K_TRACE_H
#define Q1000K_TRACE_H
#include <linux/types.h>

/* Numeric, nonsecret evidence only. Never pass addresses, keys or identity
 * words. ABI 1: fields are documented in the release capability manifest. */
enum q1000k_event {
	QT_GENERATION = 1, QT_FAULT, QT_PHY_IRQ, QT_PHY_POLL, QT_PHY_EDGE,
	QT_TX, QT_RECOVERY, QT_MAC_IRQ, QT_MAC_ERROR, QT_PLOAM_VERIFY,
	QT_PLOAM_DISPATCH, QT_PROFILE, QT_PROFILE_QUEUE, QT_PROFILE_APPLY,
	QT_ASSIGN, QT_RESET, QT_STATE, QT_JOB_QUEUE, QT_JOB_RUN, QT_MAC_CONFIG,
	QT_MAC_PROFILE, QT_OMCI, QT_RECOVERY_PHASE, QT_PROFILE_SKIP, QT_DISCOVERY,
	QT_ACTIVATION, QT_CONTROL, QT_OMCI_GEM, QT_OMCI_OPERATION,
	QT_OMCI_NATIVE_TX, QT_GWAN_APPEND, QT_EVENT_COUNT
};
void q1000k_trace(unsigned int event, unsigned int id, int result,
		   u32 a, u32 b, u32 c, u32 d);
void q1000k_discovery_snapshot(u32 interrupts);
/* Read-only MMIO at activation boundaries; no payload, keys or FIFO data. */
void q1000k_activation_snapshot(u32 stage, u32 sequence);
void q1000k_trace_generation(unsigned int reason);
int q1000k_trace_init(void);
void q1000k_trace_exit(void);
#endif
