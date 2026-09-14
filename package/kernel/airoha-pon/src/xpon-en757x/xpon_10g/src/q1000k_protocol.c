// SPDX-License-Identifier: GPL-2.0-only
/* Resumable, bounded MAC protocol executor. No hardware is accessed here. */
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "common/q1000k_protocol.h"

#define Q1000K_PROTOCOL_TIMERS 32
#define Q1000K_PROTOCOL_TASKS 16
#define Q1000K_PROTOCOL_EVENTS 64

enum q1000k_job_type { Q1000K_JOB_TIMER, Q1000K_JOB_TASK, Q1000K_JOB_PHY,
	Q1000K_JOB_IRQ, Q1000K_JOB_CONTROL };
struct q1000k_protocol_job {
	struct list_head node;
	enum q1000k_job_type type;
	bool queued, dead;
	union {
		struct { struct timer_list *timer; void (*fn)(struct timer_list *); } timer;
		struct { struct tasklet_struct *task; void (*fn)(unsigned long); unsigned long arg; } task;
		struct { unsigned int source, event; } phy;
	};
};
static DEFINE_SPINLOCK(qprotocol_lock);
static DEFINE_MUTEX(qprotocol_execute);
static struct task_struct *qprotocol_owner;
static struct task_struct *qprotocol_pause_owner;
static struct workqueue_struct *qprotocol_wq;
static struct q1000k_protocol_ops qprotocol_ops;
static LIST_HEAD(qprotocol_pending);
static struct q1000k_protocol_job qprotocol_timers[Q1000K_PROTOCOL_TIMERS];
static struct q1000k_protocol_job qprotocol_tasks[Q1000K_PROTOCOL_TASKS];
static struct q1000k_protocol_job qprotocol_events[Q1000K_PROTOCOL_EVENTS];
static struct q1000k_protocol_job qprotocol_irq_job, qprotocol_control_job;
static int qprotocol_irq, qprotocol_error;
static bool qprotocol_live, qprotocol_started, qprotocol_paused;
static bool qprotocol_irq_masked, qprotocol_fault_pending;
static void qprotocol_work(struct work_struct *work);
static DECLARE_WORK(qprotocol_worker, qprotocol_work);

static bool qprotocol_process(void)
{
	return !in_interrupt() && !in_atomic() && !irqs_disabled() &&
		!rcu_preempt_depth() &&
		!(IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC) && rcu_read_lock_held());
}

bool q1000k_protocol_owned(void)
{
	return qprotocol_process() && READ_ONCE(qprotocol_owner) == current;
}

int q1000k_protocol_enter(void)
{
	int ret;

	if (!qprotocol_process())
		return -EWOULDBLOCK;
	if (q1000k_protocol_owned())
		return q1000k_protocol_status() ?: 1;
	mutex_lock(&qprotocol_execute);
	ret = q1000k_protocol_status();
	if (ret) {
		mutex_unlock(&qprotocol_execute);
		return ret;
	}
	WRITE_ONCE(qprotocol_owner, current);
	return 0;
}

void q1000k_protocol_leave(int token)
{
	if (token == 1)
		return;
	if (WARN_ON_ONCE(token || !q1000k_protocol_owned() ||
			qprotocol_pause_owner == current))
		return;
	WRITE_ONCE(qprotocol_owner, NULL);
	mutex_unlock(&qprotocol_execute);
}

static void qprotocol_kick(void)
{
	/* qprotocol_lock also protects queue lifetime against IRQ/RCU producers. */
	if (qprotocol_wq && qprotocol_live && qprotocol_started && !qprotocol_paused)
		queue_work(qprotocol_wq, &qprotocol_worker);
}

static void qprotocol_enqueue(struct q1000k_protocol_job *job)
{
	if (!qprotocol_live || qprotocol_error || job->dead || job->queued)
		return;
	list_add_tail(&job->node, &qprotocol_pending);
	job->queued = true;
	qprotocol_kick();
}

int q1000k_protocol_control(void)
{
	unsigned long flags;
	int ret;

	if (!q1000k_protocol_owned())
		return -EPERM;
	spin_lock_irqsave(&qprotocol_lock, flags);
	ret = qprotocol_error ?: (!qprotocol_live ? -ENODEV :
		!qprotocol_ops.control ? -EOPNOTSUPP : 0);
	if (!ret && !qprotocol_control_job.queued) {
		list_add(&qprotocol_control_job.node, &qprotocol_pending);
		qprotocol_control_job.queued = true;
		qprotocol_kick();
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	return ret;
}

static void qprotocol_remove(struct q1000k_protocol_job *job)
{
	if (job->queued) {
		list_del_init(&job->node);
		job->queued = false;
	}
}

void q1000k_protocol_fail(int error)
{
	unsigned long flags;

	if (error >= 0)
		return;
	spin_lock_irqsave(&qprotocol_lock, flags);
	if (qprotocol_live && !qprotocol_error) {
		qprotocol_error = error;
		qprotocol_fault_pending = true;
		qprotocol_kick();
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
}

int q1000k_protocol_status(void)
{
	return READ_ONCE(qprotocol_error) ?: (READ_ONCE(qprotocol_live) ? 0 : -ENODEV);
}

static void qprotocol_work(struct work_struct *work)
{
	struct q1000k_protocol_job *job;
	unsigned int source = 0, event = 0;
	unsigned long flags;
	int fault;
	enum q1000k_job_type type = Q1000K_JOB_IRQ;
	bool unmask;

	for (;;) {
		mutex_lock(&qprotocol_execute);
		WRITE_ONCE(qprotocol_owner, current);
		spin_lock_irqsave(&qprotocol_lock, flags);
		fault = 0;
		job = NULL;
		if (qprotocol_live && qprotocol_started && !qprotocol_paused) {
			if (qprotocol_fault_pending) {
				fault = qprotocol_error;
				qprotocol_fault_pending = false;
			} else if (!qprotocol_error && !list_empty(&qprotocol_pending)) {
				job = list_first_entry(&qprotocol_pending, struct q1000k_protocol_job, node);
				/* Copy transient PHY storage before making it reusable. */
				type = job->type;
				if (type == Q1000K_JOB_PHY) {
					source = job->phy.source;
					event = job->phy.event;
				}
				qprotocol_remove(job);
			}
		}
		spin_unlock_irqrestore(&qprotocol_lock, flags);
		if (job && type == Q1000K_JOB_CONTROL) {
			WRITE_ONCE(qprotocol_owner, NULL);
			mutex_unlock(&qprotocol_execute);
			qprotocol_ops.control();
			cond_resched();
			continue;
		}
		if (fault)
			qprotocol_ops.fault(fault);
		else if (job) {
			switch (type) {
			case Q1000K_JOB_TIMER: job->timer.fn(job->timer.timer); break;
			case Q1000K_JOB_TASK: job->task.fn(job->task.arg); break;
			case Q1000K_JOB_PHY: qprotocol_ops.phy(source, event); break;
			case Q1000K_JOB_IRQ: qprotocol_ops.irq(); break;
			case Q1000K_JOB_CONTROL: break; /* dispatched without mutex above */
			}
		}
		/* A level IRQ stays masked until its deferred handler has read and
		 * acknowledged the MAC. A sticky fault leaves it masked for stop.
		 */
		spin_lock_irqsave(&qprotocol_lock, flags);
		unmask = job == &qprotocol_irq_job && qprotocol_live &&
			!qprotocol_error && qprotocol_irq_masked;
		if (unmask)
			qprotocol_irq_masked = false;
		spin_unlock_irqrestore(&qprotocol_lock, flags);
		if (unmask)
			enable_irq(qprotocol_irq);
		WRITE_ONCE(qprotocol_owner, NULL);
		mutex_unlock(&qprotocol_execute);
		if (!job && !fault)
			return;
		cond_resched();
	}
}

static irqreturn_t qprotocol_interrupt(int irq, void *data)
{
	unsigned long flags;

	spin_lock_irqsave(&qprotocol_lock, flags);
	if (qprotocol_live && !qprotocol_irq_masked) {
		disable_irq_nosync(irq);
		qprotocol_irq_masked = true;
		qprotocol_enqueue(&qprotocol_irq_job);
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	return IRQ_HANDLED;
}

int q1000k_protocol_phy(unsigned int source, unsigned int event)
{
	unsigned long flags;
	unsigned int i;
	int ret;

	spin_lock_irqsave(&qprotocol_lock, flags);
	ret = qprotocol_error ?: (qprotocol_live ? 0 : -ENODEV);
	if (ret)
		goto out;
	for (i = 0; i < Q1000K_PROTOCOL_EVENTS; i++) {
		if (qprotocol_events[i].queued)
			continue;
		qprotocol_events[i].type = Q1000K_JOB_PHY;
		qprotocol_events[i].phy.source = source;
		qprotocol_events[i].phy.event = event;
		qprotocol_enqueue(&qprotocol_events[i]);
		goto out;
	}
	ret = -ENOSPC;
	qprotocol_error = ret;
	qprotocol_fault_pending = true;
	qprotocol_kick();
out:
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	return ret;
}

int q1000k_protocol_pause(void)
{
	unsigned long flags;
	int ret;

	if (!qprotocol_process())
		return -EWOULDBLOCK;
	if (q1000k_protocol_owned())
		return -EDEADLK;
	mutex_lock(&qprotocol_execute);
	spin_lock_irqsave(&qprotocol_lock, flags);
	ret = qprotocol_error ?: (qprotocol_live && qprotocol_started ? 0 : -ENODEV);
	if (!ret) {
		qprotocol_paused = true;
		qprotocol_pause_owner = current;
		WRITE_ONCE(qprotocol_owner, current);
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (ret)
		mutex_unlock(&qprotocol_execute);
	return ret;
}

int q1000k_protocol_resume(void)
{
	unsigned long flags;
	int ret;

	if (!q1000k_protocol_owned() || qprotocol_pause_owner != current)
		return -EPERM;
	spin_lock_irqsave(&qprotocol_lock, flags);
	qprotocol_pause_owner = NULL;
	qprotocol_paused = false;
	ret = qprotocol_error;
	qprotocol_kick();
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	WRITE_ONCE(qprotocol_owner, NULL);
	mutex_unlock(&qprotocol_execute);
	return ret;
}

static struct q1000k_protocol_job *qprotocol_find_timer(struct timer_list *timer)
{
	unsigned int i;

	for (i = 0; i < Q1000K_PROTOCOL_TIMERS; i++)
		if (qprotocol_timers[i].timer.timer == timer)
			return &qprotocol_timers[i];
	return NULL;
}

static void qprotocol_timer(struct timer_list *timer)
{
	struct q1000k_protocol_job *job;
	unsigned long flags;

	spin_lock_irqsave(&qprotocol_lock, flags);
	job = qprotocol_find_timer(timer);
	if (job)
		qprotocol_enqueue(job);
	spin_unlock_irqrestore(&qprotocol_lock, flags);
}

int q1000k_protocol_timer_init(struct timer_list *timer,
			     void (*callback)(struct timer_list *), unsigned long expires)
{
	struct q1000k_protocol_job *job = NULL;
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	if (!timer || !callback || !qprotocol_process())
		return -EINVAL;
	/* Registration belongs to module initialization before start. */
	spin_lock_irqsave(&qprotocol_lock, flags);
	if (!qprotocol_live || qprotocol_started || qprotocol_find_timer(timer)) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < Q1000K_PROTOCOL_TIMERS; i++)
		if (!qprotocol_timers[i].timer.timer) {
			job = &qprotocol_timers[i];
			break;
		}
	timer_setup(timer, qprotocol_timer, 0);
	timer->expires = expires;
	if (!job) {
		ret = -ENOSPC;
		goto out;
	}
	job->type = Q1000K_JOB_TIMER;
	job->timer.timer = timer;
	job->timer.fn = callback;
out:
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (ret)
		q1000k_protocol_fail(ret);
	return ret;
}

int q1000k_protocol_timer_delete(struct timer_list *timer, bool sync, bool shutdown)
{
	struct q1000k_protocol_job *job;
	unsigned long flags;
	bool lock = sync && !q1000k_protocol_owned();
	int ret;

	if (lock && !qprotocol_process()) {
		q1000k_protocol_fail(-EWOULDBLOCK);
		return -EWOULDBLOCK;
	}
	if (lock)
		mutex_lock(&qprotocol_execute);
	spin_lock_irqsave(&qprotocol_lock, flags);
	job = qprotocol_find_timer(timer);
	if (job && shutdown)
		job->dead = true;
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	ret = shutdown ? timer_shutdown_sync(timer) :
		(sync ? timer_delete_sync(timer) : timer_delete(timer));
	spin_lock_irqsave(&qprotocol_lock, flags);
	if (job) {
		ret |= job->queued;
		qprotocol_remove(job);
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (lock)
		mutex_unlock(&qprotocol_execute);
	return ret;
}

int q1000k_protocol_reset_jobs(void)
{
	unsigned long flags;
	unsigned int i;
	int ret;

	if (!q1000k_protocol_owned())
		return -EPERM;
	ret = q1000k_protocol_status();
	if (ret)
		return ret;
	/* Timer callbacks only enqueue under qprotocol_lock; none can wait
	 * for the executor held here. Synchronous cancellation closes that
	 * enqueue race before the old job list is removed.
	 */
	for (i = 0; i < Q1000K_PROTOCOL_TIMERS; i++)
		if (qprotocol_timers[i].timer.timer)
			timer_delete_sync(qprotocol_timers[i].timer.timer);
	spin_lock_irqsave(&qprotocol_lock, flags);
	for (i = 0; i < Q1000K_PROTOCOL_TIMERS; i++)
		qprotocol_remove(&qprotocol_timers[i]);
	for (i = 0; i < Q1000K_PROTOCOL_TASKS; i++)
		qprotocol_remove(&qprotocol_tasks[i]);
	for (i = 0; i < Q1000K_PROTOCOL_EVENTS; i++)
		qprotocol_remove(&qprotocol_events[i]);
	ret = qprotocol_error;
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	return ret;
}

static struct q1000k_protocol_job *qprotocol_find_task(struct tasklet_struct *task)
{
	unsigned int i;

	for (i = 0; i < Q1000K_PROTOCOL_TASKS; i++)
		if (qprotocol_tasks[i].task.task == task)
			return &qprotocol_tasks[i];
	return NULL;
}

void q1000k_protocol_task_init(struct tasklet_struct *task,
			    void (*callback)(unsigned long), unsigned long arg)
{
	struct q1000k_protocol_job *job = NULL;
	unsigned long flags;
	unsigned int i;
	int ret = 0;

	spin_lock_irqsave(&qprotocol_lock, flags);
	if (!task || !callback || !qprotocol_live || qprotocol_started ||
	    qprotocol_find_task(task)) {
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < Q1000K_PROTOCOL_TASKS; i++)
		if (!qprotocol_tasks[i].task.task) {
			job = &qprotocol_tasks[i];
			break;
		}
	if (!job) {
		ret = -ENOSPC;
		goto out;
	}
	job->type = Q1000K_JOB_TASK;
	job->task.task = task;
	job->task.fn = callback;
	job->task.arg = arg;
out:
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (ret)
		q1000k_protocol_fail(ret);
}

void q1000k_protocol_task_schedule(struct tasklet_struct *task)
{
	struct q1000k_protocol_job *job;
	unsigned long flags;

	spin_lock_irqsave(&qprotocol_lock, flags);
	job = qprotocol_find_task(task);
	if (job)
		qprotocol_enqueue(job);
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (!job)
		q1000k_protocol_fail(-ENOENT);
}

void q1000k_protocol_task_kill(struct tasklet_struct *task)
{
	struct q1000k_protocol_job *job;
	unsigned long flags;
	bool lock = !q1000k_protocol_owned();

	if (lock && !qprotocol_process()) {
		q1000k_protocol_fail(-EWOULDBLOCK);
		return;
	}
	if (lock)
		mutex_lock(&qprotocol_execute);
	spin_lock_irqsave(&qprotocol_lock, flags);
	job = qprotocol_find_task(task);
	if (job) {
		job->dead = true;
		qprotocol_remove(job);
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (lock)
		mutex_unlock(&qprotocol_execute);
}

int q1000k_protocol_init(int irq, const struct q1000k_protocol_ops *ops)
{
	int ret;

	if (irq < 0 || !ops || !ops->irq || !ops->phy || !ops->fault)
		return -EINVAL;
	if (qprotocol_wq)
		return -EBUSY;
	qprotocol_wq = alloc_ordered_workqueue("q1000k_mac", WQ_MEM_RECLAIM);
	if (!qprotocol_wq)
		return -ENOMEM;
	INIT_LIST_HEAD(&qprotocol_pending);
	memset(qprotocol_timers, 0, sizeof(qprotocol_timers));
	memset(qprotocol_tasks, 0, sizeof(qprotocol_tasks));
	memset(qprotocol_events, 0, sizeof(qprotocol_events));
	memset(&qprotocol_irq_job, 0, sizeof(qprotocol_irq_job));
	qprotocol_irq_job.type = Q1000K_JOB_IRQ;
	memset(&qprotocol_control_job, 0, sizeof(qprotocol_control_job));
	qprotocol_control_job.type = Q1000K_JOB_CONTROL;
	qprotocol_error = 0;
	qprotocol_fault_pending = false;
	qprotocol_started = false;
	qprotocol_paused = false;
	qprotocol_irq_masked = true;
	qprotocol_ops = *ops;
	qprotocol_irq = irq;
	ret = request_irq(irq, qprotocol_interrupt, IRQF_NO_AUTOEN, "q1000k-mac", &qprotocol_irq_job);
	if (ret) {
		destroy_workqueue(qprotocol_wq);
		qprotocol_wq = NULL;
		return ret;
	}
	WRITE_ONCE(qprotocol_live, true);
	return 0;
}

int q1000k_protocol_start(void)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&qprotocol_lock, flags);
	ret = qprotocol_error ?: (qprotocol_live ? 0 : -ENODEV);
	if (!ret && qprotocol_started)
		ret = -EALREADY;
	if (!ret) {
		qprotocol_started = true;
		qprotocol_irq_masked = false;
		qprotocol_kick();
	}
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (!ret)
		enable_irq(qprotocol_irq);
	return ret;
}

void q1000k_protocol_stop(void)
{
	struct workqueue_struct *wq;
	unsigned long flags;
	unsigned int i;
	bool unmask;

	if (WARN_ON_ONCE(!qprotocol_process() || q1000k_protocol_owned() ||
			 current_work() == &qprotocol_worker))
		return;
	spin_lock_irqsave(&qprotocol_lock, flags);
	wq = qprotocol_wq;
	qprotocol_live = false;
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	if (!wq)
		return;
	/* Wait for admitted process controls as well as queued event callbacks.
	 * Never retain execution while cancel_work_sync waits on the worker.
	 */
	mutex_lock(&qprotocol_execute);
	mutex_unlock(&qprotocol_execute);
	cancel_work_sync(&qprotocol_worker);
	for (i = 0; i < Q1000K_PROTOCOL_TIMERS; i++)
		if (qprotocol_timers[i].timer.timer)
			timer_shutdown_sync(qprotocol_timers[i].timer.timer);
	spin_lock_irqsave(&qprotocol_lock, flags);
	unmask = qprotocol_irq_masked;
	qprotocol_irq_masked = false;
	qprotocol_wq = NULL;
	spin_unlock_irqrestore(&qprotocol_lock, flags);
	/* Balance NO_AUTOEN/deferred disable before releasing the action. The
	 * MAC source is already masked by the owner; late handlers see !live.
	 */
	if (unmask)
		enable_irq(qprotocol_irq);
	free_irq(qprotocol_irq, &qprotocol_irq_job);
	destroy_workqueue(wq);
}
