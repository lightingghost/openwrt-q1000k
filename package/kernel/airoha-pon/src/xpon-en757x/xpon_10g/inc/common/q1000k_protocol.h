/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _Q1000K_PROTOCOL_H_
#define _Q1000K_PROTOCOL_H_
#include <linux/interrupt.h>
#include <linux/timer.h>

struct q1000k_protocol_ops {
	void (*irq)(void);
	void (*phy)(unsigned int source, unsigned int event);
	void (*fault)(int error);
};
/* Init/start/stop are serialized by the module owner. Init leaves IRQ and
 * dispatch closed; stop must precede freeing any callback/timer/task state.
 * The owner masks the MAC source before stop. No callbacks enter under RCU.
 */
int q1000k_protocol_init(int irq, const struct q1000k_protocol_ops *ops);
int q1000k_protocol_start(void);
void q1000k_protocol_stop(void);
int q1000k_protocol_status(void);
int q1000k_protocol_phy(unsigned int source, unsigned int event);
/* Pause waits for the current handler and retains the execution mutex until
 * resume by the SAME task. Pending timer/task/IRQ/PHY events remain queued.
 * No protocol callback may call pause/stop; use the already-owned context.
 * No core OMCI barrier may be called while holding this mutex.
 */
int q1000k_protocol_pause(void);
int q1000k_protocol_resume(void);
bool q1000k_protocol_owned(void);
/* Serialize a process control caller with protocol events. The nonnegative
 * token must be returned unchanged to leave; nested owned calls are allowed.
 * A control caller must not hold RTNL, RCU or a spinlock. OMCI service
 * callbacks may hold their core session lock; reverse notifications to the
 * core must run outside this executor to preserve that lock order.
 */
int q1000k_protocol_enter(void);
void q1000k_protocol_leave(int token);
void q1000k_protocol_fail(int error);

int q1000k_protocol_timer_init(struct timer_list *timer,
			     void (*callback)(struct timer_list *), unsigned long expires);
int q1000k_protocol_timer_delete(struct timer_list *timer, bool sync, bool shutdown);
void q1000k_protocol_task_init(struct tasklet_struct *task,
			    void (*callback)(unsigned long), unsigned long arg);
void q1000k_protocol_task_schedule(struct tasklet_struct *task);
void q1000k_protocol_task_kill(struct tasklet_struct *task);
#endif
