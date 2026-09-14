// SPDX-License-Identifier: GPL-2.0-only
/* AN7581 indirect T-CONT table: the MAC owner supplies its mapped registers. */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include "common/xpon_global.h"
#include "common/q1000k_pipeline.h"
#include "common/q1000k_tcont.h"

#define TCONT_CMD_WRITE BIT(31)
#define TCONT_CMD_CHANNEL GENMASK(24, 20)
#define TCONT_CMD_VALID BIT(16)
#define TCONT_CMD_ID GENMASK(13, 0)
#define TCONT_STS_DONE BIT(31)
#define TCONT_POLL_LIMIT 3000

static DEFINE_SPINLOCK(q1000k_tcont_lock);
static bool q1000k_tcont_fault;
static u32 q1000k_tcont_quarantined;

static int q1000k_tcont_command(u32 command, u32 *status)
{
	unsigned int retry;
	u32 value;

	lockdep_assert_held(&q1000k_tcont_lock);
	if (q1000k_tcont_fault)
		return -EIO;
	/* Construct every field explicitly; reserved bits are always zero. */
	IO_SREG(TCONT_ID_CFG, command);
	for (retry = 0; retry < TCONT_POLL_LIMIT; retry++) {
		value = IO_GREG(TCONT_ID_STS);
		if (value == ~0U) {
			q1000k_tcont_fault = true;
			return -EIO;
		}
		if (value & TCONT_STS_DONE) {
			*status = value;
			return 0;
		}
		udelay(1);
	}
	/* A late completion must not be mistaken for the next command. */
	q1000k_tcont_fault = true;
	return -ETIMEDOUT;
}

static int q1000k_tcont_read_locked(unsigned int channel, bool *valid,
				     u16 *alloc_id)
{
	u32 status;
	int ret;

	ret = q1000k_tcont_command(FIELD_PREP(TCONT_CMD_CHANNEL, channel), &status);
	if (ret)
		return ret;
	*valid = !!(status & TCONT_CMD_VALID);
	*alloc_id = FIELD_GET(TCONT_CMD_ID, status);
	return 0;
}

static int q1000k_tcont_write_locked(unsigned int channel, bool valid,
				      u16 alloc_id)
{
	u32 command, status;
	u16 read_id;
	bool read_valid;
	int ret;

	if (valid && (q1000k_tcont_quarantined & BIT(channel)))
		return -EBUSY;
	command = TCONT_CMD_WRITE | FIELD_PREP(TCONT_CMD_CHANNEL, channel);
	if (valid)
		command |= TCONT_CMD_VALID | FIELD_PREP(TCONT_CMD_ID, alloc_id);
	ret = q1000k_tcont_command(command, &status);
	if (ret)
		return ret;
	ret = q1000k_tcont_read_locked(channel, &read_valid, &read_id);
	if (ret)
		return ret;
	if (read_valid != valid || (valid && read_id != alloc_id)) {
		q1000k_tcont_fault = true;
		return -EIO;
	}
	return 0;
}

int q1000k_tcont_read(unsigned int channel, bool *valid, u16 *alloc_id)
{
	unsigned long flags;
	int ret;

	if (channel >= Q1000K_TCONT_COUNT || !valid || !alloc_id)
		return -EINVAL;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	ret = q1000k_tcont_read_locked(channel, valid, alloc_id);
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
	return ret;
}

int q1000k_tcont_find(u16 alloc_id, u16 onu_id, u8 *channel)
{
	unsigned long flags;
	unsigned int i;
	u16 id;
	bool valid;
	int ret = -ENOENT;

	if (alloc_id > Q1000K_ALLOC_ID_MAX || !channel)
		return -EINVAL;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	if (q1000k_tcont_fault) {
		ret = -EIO;
		goto out;
	}
	if (onu_id < GPON_UNASSIGN_ONU_ID && alloc_id == onu_id) {
		*channel = 0;
		ret = 0;
		goto out;
	}
	for (i = 1; i < Q1000K_TCONT_COUNT; i++) {
		ret = q1000k_tcont_read_locked(i, &valid, &id);
		if (ret)
			goto out;
		if (valid && id == alloc_id) {
			*channel = i;
			goto out;
		}
	}
	ret = -ENOENT;
out:
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
	return ret;
}

int q1000k_tcont_enable(u16 alloc_id, u16 onu_id)
{
	unsigned long flags;
	unsigned int i, free_channel = 0;
	u16 id;
	bool valid;
	int ret;

	if (alloc_id > Q1000K_ALLOC_ID_MAX)
		return -EINVAL;
	/* ONU assignment needs its own protocol/OMCC transaction. */
	if (onu_id < GPON_UNASSIGN_ONU_ID && alloc_id == onu_id)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	for (i = 1; i < Q1000K_TCONT_COUNT; i++) {
		ret = q1000k_tcont_read_locked(i, &valid, &id);
		if (ret)
			goto out;
		if (valid && id == alloc_id) {
			ret = -EEXIST;
			goto out;
		}
		if (!valid && !free_channel &&
		    !(q1000k_tcont_quarantined & BIT(i)))
			free_channel = i;
	}
	if (!free_channel) {
		ret = -ENOSPC;
		goto out;
	}
	ret = q1000k_tcont_write_locked(free_channel, true, alloc_id);
	if (!ret)
		ret = free_channel;
out:
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
	return ret;
}

int q1000k_tcont_disable(u16 alloc_id, u16 onu_id)
{
	unsigned long flags;
	unsigned int i;
	u16 id;
	bool valid;
	int ret;

	if (alloc_id > Q1000K_ALLOC_ID_MAX)
		return -EINVAL;
	if (onu_id < GPON_UNASSIGN_ONU_ID && alloc_id == onu_id)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	for (i = 1; i < Q1000K_TCONT_COUNT; i++) {
		ret = q1000k_tcont_read_locked(i, &valid, &id);
		if (ret)
			goto out;
		if (valid && id == alloc_id) {
			/* MAC invalidation alone never makes a channel reusable. */
			q1000k_tcont_quarantined |= BIT(i);
			ret = q1000k_tcont_write_locked(i, false, 0);
			if (!ret)
				ret = i;
			goto out;
		}
	}
	ret = -ENOENT;
out:
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
	return ret;
}

void q1000k_tcont_quarantine(unsigned int channel)
{
	unsigned long flags;

	if (!channel || channel >= Q1000K_TCONT_COUNT)
		return;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	q1000k_tcont_quarantined |= BIT(channel);
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
}

/* Only the serialized physical namespace transaction may release quarantine. */
int q1000k_tcont_clear_namespace(void)
{
	unsigned long flags;
	unsigned int channel;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR);

	if (ret)
		return ret;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	for (channel = 1; channel < Q1000K_TCONT_COUNT; channel++) {
		ret = q1000k_tcont_write_locked(channel, false, 0);
		if (ret)
			goto out;
	}
	/* Channel zero remains the separate ONU-ID/OMCC shadow. */
	q1000k_tcont_quarantined = 0;
out:
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
	return ret;
}

int q1000k_tcont_install(unsigned int channel, u16 alloc_id, u16 onu_id)
{
	unsigned long flags;
	unsigned int i;
	bool valid, occupied = false;
	u16 id;
	int ret = q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL);

	if (ret)
		return ret;
	if (!channel || channel >= Q1000K_TCONT_COUNT ||
	    alloc_id > Q1000K_ALLOC_ID_MAX ||
	    (onu_id < GPON_UNASSIGN_ONU_ID && alloc_id == onu_id))
		return -EINVAL;
	spin_lock_irqsave(&q1000k_tcont_lock, flags);
	if (q1000k_tcont_quarantined & BIT(channel)) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 1; i < Q1000K_TCONT_COUNT; i++) {
		ret = q1000k_tcont_read_locked(i, &valid, &id);
		if (ret)
			goto out;
		if (valid && i != channel && id == alloc_id) {
			ret = -EEXIST;
			goto out;
		}
		if (valid && i == channel) {
			if (id != alloc_id) {
				ret = -EBUSY;
				goto out;
			}
			occupied = true;
		}
	}
	if (!occupied)
		ret = q1000k_tcont_write_locked(channel, true, alloc_id);
out:
	spin_unlock_irqrestore(&q1000k_tcont_lock, flags);
	return ret;
}
