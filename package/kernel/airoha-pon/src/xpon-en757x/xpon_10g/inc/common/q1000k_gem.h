/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_GEM_H_
#define _Q1000K_GEM_H_

#include <linux/types.h>

#define Q1000K_GEM_ID_MAX 65534

struct q1000k_gem_value {
	u8 valid;
	u8 multicast;
	u8 encrypted;
};

/* IRQ-safe, serialized indirect MAC commands. Outputs are unchanged on
 * error. Reserved command bits are zero; every update is read back.
 * A timeout/mismatch latches a fault for this module instance. Recovery
 * requires verified hardware reset before another instance, not just reload.
 */
int q1000k_gem_read(u16 id, struct q1000k_gem_value *value);
bool q1000k_gem_faulted(void);

/* Compare and update under one command lock. -ESTALE preserves a different
 * binding; type/encryption are ignored when comparing invalid entries.
 * Caller must first coordinate packet admission and physical retirement.
 * This operation itself neither drains traffic nor makes an ID reusable.
 */
int q1000k_gem_replace(u16 id, const struct q1000k_gem_value *expected,
			const struct q1000k_gem_value *value);

/* Internal namespace-clear callback only. Verify/clear all usable GEM IDs,
 * except an unchanged OMCC to preserve. 0xffff means preserve no entry.
 */
int q1000k_gem_clear_namespace(u16 preserve);

#endif
