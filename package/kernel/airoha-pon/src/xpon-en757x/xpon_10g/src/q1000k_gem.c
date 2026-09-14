// SPDX-License-Identifier: GPL-2.0-only
/* AN7581 GEM commands; the existing MAC provider owns the mapped registers. */
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include "common/xpon_global.h"
#include "common/q1000k_pipeline.h"
#include "common/q1000k_gem.h"

#define GEM_CMD_WRITE BIT(31)
#define GEM_CMD_VALID BIT(18)
#define GEM_CMD_UNICAST BIT(17)
#define GEM_CMD_ENCRYPT BIT(16)
#define GEM_STS_DONE BIT(31)
#define GEM_POLL_LIMIT 3000

static DEFINE_SPINLOCK(q1000k_gem_lock);
static bool q1000k_gem_fault;

bool q1000k_gem_faulted(void)
{
	return READ_ONCE(q1000k_gem_fault);
}

static int q1000k_gem_command(u32 command, u32 *status)
{
	unsigned int retry;
	u32 value;

	lockdep_assert_held(&q1000k_gem_lock);
	if (q1000k_gem_fault)
		return -EIO;
	IO_SREG(GEM_PORT_CFG, command);
	for (retry = 0; retry < GEM_POLL_LIMIT; retry++) {
		value = IO_GREG(GEM_PORT_STS);
		if (value == ~0U) {
			q1000k_gem_fault = true;
			return -EIO;
		}
		if (value & GEM_STS_DONE) {
			*status = value;
			return 0;
		}
		udelay(1);
	}
	/* Never consume a late completion as the result of a later command. */
	WRITE_ONCE(q1000k_gem_fault, true);
	return -ETIMEDOUT;
}

static int q1000k_gem_read_locked(u16 id, struct q1000k_gem_value *value)
{
	u32 status;
	int ret;

	ret = q1000k_gem_command(id, &status);
	if (ret)
		return ret;
	value->valid = !!(status & BIT(2));
	value->multicast = !(status & BIT(1));
	value->encrypted = !!(status & BIT(0));
	return 0;
}

int q1000k_gem_read(u16 id, struct q1000k_gem_value *value)
{
	unsigned long flags;
	int ret;

	if (id > Q1000K_GEM_ID_MAX || !value)
		return -EINVAL;
	spin_lock_irqsave(&q1000k_gem_lock, flags);
	ret = q1000k_gem_read_locked(id, value);
	spin_unlock_irqrestore(&q1000k_gem_lock, flags);
	return ret;
}

static bool q1000k_gem_equal(const struct q1000k_gem_value *a,
			     const struct q1000k_gem_value *b)
{
	return a->valid == b->valid &&
	       (!a->valid || (a->multicast == b->multicast &&
			     a->encrypted == b->encrypted));
}

int q1000k_gem_replace(u16 id, const struct q1000k_gem_value *expected,
			const struct q1000k_gem_value *value)
{
	struct q1000k_gem_value observed;
	unsigned long flags;
	u32 command, status;
	int ret;

	if (id > Q1000K_GEM_ID_MAX || !expected || !value ||
	    expected->valid > 1 || expected->multicast > 1 || expected->encrypted > 1 ||
	    value->valid > 1 || value->multicast > 1 || value->encrypted > 1)
		return -EINVAL;
	spin_lock_irqsave(&q1000k_gem_lock, flags);
	ret = q1000k_gem_read_locked(id, &observed);
	if (ret)
		goto out;
	if (!q1000k_gem_equal(&observed, expected)) {
		ret = -ESTALE;
		goto out;
	}
	if (q1000k_gem_equal(&observed, value))
		goto out;
	command = GEM_CMD_WRITE | id;
	if (value->valid) {
		command |= GEM_CMD_VALID;
		if (!value->multicast)
			command |= GEM_CMD_UNICAST;
		if (value->encrypted)
			command |= GEM_CMD_ENCRYPT;
	} else {
		command |= GEM_CMD_UNICAST;
	}
	ret = q1000k_gem_command(command, &status);
	if (ret)
		goto out;
	ret = q1000k_gem_read_locked(id, &observed);
	if (ret)
		goto out;
	if (!q1000k_gem_equal(&observed, value)) {
		WRITE_ONCE(q1000k_gem_fault, true);
		ret = -EIO;
	}
out:
	spin_unlock_irqrestore(&q1000k_gem_lock, flags);
	return ret;
}

/* Cold start verifies the complete address space. Runtime transactions can
 * instead clear their tracked old bindings while preserving the active OMCC.
 */
int q1000k_gem_clear_namespace(u16 preserve)
{
	const struct q1000k_gem_value empty = {};
	struct q1000k_gem_value value;
	unsigned int id;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR);

	if (ret)
		return ret;
	for (id = 0; id <= Q1000K_GEM_ID_MAX; id++) {
		if (id == preserve)
			continue;
		ret = q1000k_gem_read(id, &value);
		if (ret)
			return ret;
		if (value.valid) {
			ret = q1000k_gem_replace(id, &value, &empty);
			if (ret)
				return ret;
		}
		if (!(id & 63))
			cond_resched();
	}
	return 0;
}
