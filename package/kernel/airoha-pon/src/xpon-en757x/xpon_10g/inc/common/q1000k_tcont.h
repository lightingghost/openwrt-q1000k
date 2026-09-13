/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_TCONT_H_
#define _Q1000K_TCONT_H_

#include <linux/types.h>

#define Q1000K_TCONT_COUNT 32
#define Q1000K_ALLOC_ID_MAX 0x3fff

/* These command operations do not establish queue/FE/optical retirement.
 * Channel zero is the ONU-ID shadow and cannot be written through this API.
 * IRQ-safe; a single lock covers each complete scan/command/readback sequence.
 * Read/query outputs are unchanged on failure. A command timeout or mismatched
 * write verification latches a fault for this MAC module's lifetime: recovery
 * requires verified hardware reset before a subsequent module instance.
 */
int q1000k_tcont_read(unsigned int channel, bool *valid, u16 *alloc_id);
int q1000k_tcont_find(u16 alloc_id, u16 onu_id, u8 *channel);
int q1000k_tcont_enable(u16 alloc_id, u16 onu_id);
int q1000k_tcont_disable(u16 alloc_id, u16 onu_id);

/* Prevent slot reuse after uncertain setup/removal. Quarantine is permanent
 * for this module instance; neither a successful read nor an invalid MAC entry
 * proves that previously submitted traffic has left the native/FE pipeline.
 * Reads and invalidation remain available unless a command fault is latched.
 */
void q1000k_tcont_quarantine(unsigned int channel);

#endif
