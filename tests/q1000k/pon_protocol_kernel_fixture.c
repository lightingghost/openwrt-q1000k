// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>

static int irq_depth, irq_requested, irq_fail;
static irq_handler_t irq_handler;
static void *irq_cookie;
static int fixture_request_irq(unsigned int irq, irq_handler_t handler,
	unsigned long flags, const char *name, void *cookie)
{
	if (irq_fail) return -EBUSY;
	if (irq != 42 || flags != IRQF_NO_AUTOEN || irq_requested) return -EINVAL;
	irq_requested=1; irq_depth=1; irq_handler=handler; irq_cookie=cookie;
	return 0;
}
static void fixture_enable_irq(unsigned int irq) { WARN_ON(irq != 42 || --irq_depth); }
static void fixture_disable_irq_nosync(unsigned int irq) { WARN_ON(irq != 42 || irq_depth++); }
static void fixture_free_irq(unsigned int irq, void *cookie)
{
	WARN_ON(irq != 42 || cookie != irq_cookie || irq_depth || !irq_requested);
	irq_requested=0;
}
/* PRODUCTION */

#define CHECK(x) do { if (!(x)) { pr_err("protocol check failed line %d: %s\n", __LINE__, #x); return -EINVAL; } } while (0)
static struct timer_list sample_timer, capacity_timers[33];
static struct tasklet_struct sample_task;
static int seen[128], seen_count, task_count, timer_count, fault_seen, irq_count;
static int block_kind, thread_ret;
static DECLARE_COMPLETION(entered);
static DECLARE_COMPLETION(release_callback);
static DECLARE_COMPLETION(thread_done);
static void maybe_block(int kind)
{
	WARN_ON(!q1000k_protocol_owned());
	if (READ_ONCE(block_kind) == kind) {
		complete(&entered);
		wait_for_completion(&release_callback);
	}
}
static void test_irq(void) { irq_count++; maybe_block(1); }
static void test_phy(unsigned int source, unsigned int event)
{
	WARN_ON(source != 7);
	seen[seen_count++] = event;
	maybe_block(2);
}
static void test_fault(int error) { fault_seen=error; maybe_block(4); }
static void test_task(unsigned long arg)
{
	WARN_ON(arg != 123);
	task_count++;
	maybe_block(5);
	if (task_count == 1) q1000k_protocol_task_schedule(&sample_task);
}
static void test_timer(struct timer_list *timer)
{
	timer_count++;
	maybe_block(3);
	mod_timer(timer, jiffies + HZ);
}
static int control_count;
static void test_control(void)
{
	int token;
	WARN_ON(q1000k_protocol_owned());
	control_count++;
	if (READ_ONCE(block_kind) == 6) {
		complete(&entered);
		wait_for_completion(&release_callback);
	}
	token = q1000k_protocol_enter();
	if (token >= 0) q1000k_protocol_leave(token);
}
static const struct q1000k_protocol_ops test_ops = {
	.irq=test_irq, .phy=test_phy, .fault=test_fault, .control=test_control,
};
static void finish_thread(void)
{
	complete(&thread_done);
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop()) schedule();
	}
	__set_current_state(TASK_RUNNING);
}
static int pause_thread(void *unused)
{
	thread_ret=q1000k_protocol_pause();
	if (!thread_ret) thread_ret=q1000k_protocol_resume();
	finish_thread(); return 0;
}
static int cancel_thread(void *unused)
{
	thread_ret=q1000k_protocol_timer_delete(&sample_timer, true, false);
	finish_thread(); return 0;
}
static int stop_thread(void *unused)
{
	q1000k_protocol_stop(); finish_thread(); return 0;
}
static void reset_barriers(int kind)
{
	reinit_completion(&entered); reinit_completion(&release_callback);
	reinit_completion(&thread_done); WRITE_ONCE(block_kind,kind);
}
static void inject_irq(void)
{
	unsigned long flags;
	local_irq_save(flags); irq_handler(42, irq_cookie); local_irq_restore(flags);
}
static int run_test(void)
{
	struct task_struct *thread;
	unsigned int i;
	int ret;

	CHECK(q1000k_protocol_status() == -ENODEV);
	CHECK(q1000k_protocol_init(-1,&test_ops) == -EINVAL);
	irq_fail=1; CHECK(q1000k_protocol_init(42,&test_ops) == -EBUSY);
	irq_fail=0; CHECK(!q1000k_protocol_init(42,&test_ops));
	CHECK(!q1000k_protocol_timer_init(&sample_timer,test_timer,0));
	q1000k_protocol_task_init(&sample_task,test_task,123);
	CHECK(!q1000k_protocol_status());
	ret=q1000k_protocol_enter(); CHECK(ret == 0 && q1000k_protocol_owned());
	CHECK(q1000k_protocol_enter() == 1);
	q1000k_protocol_leave(1); CHECK(q1000k_protocol_owned());
	q1000k_protocol_leave(ret); CHECK(!q1000k_protocol_owned());
	CHECK(irq_depth == 1);
	CHECK(!q1000k_protocol_start()); CHECK(irq_depth == 0);
	CHECK(q1000k_protocol_start() == -EALREADY);
	rcu_read_lock(); ret=q1000k_protocol_pause(); rcu_read_unlock();
	CHECK(ret == -EWOULDBLOCK);
	local_bh_disable(); ret=q1000k_protocol_pause(); local_bh_enable();
	CHECK(ret == -EWOULDBLOCK);
	CHECK(q1000k_protocol_resume() == -EPERM);
	CHECK(!q1000k_protocol_pause()); CHECK(q1000k_protocol_pause() == -EDEADLK);
	CHECK(!q1000k_protocol_phy(7,11)); CHECK(!q1000k_protocol_phy(7,12));
	q1000k_protocol_task_schedule(&sample_task); q1000k_protocol_task_schedule(&sample_task);
	mod_timer(&sample_timer, jiffies+1);
	msleep(30); CHECK(!seen_count && !task_count && !timer_count);
	CHECK(q1000k_protocol_timer_delete(&sample_timer,true,false) >= 0);
	CHECK(!q1000k_protocol_resume()); flush_workqueue(qprotocol_wq);
	CHECK(seen_count == 2 && seen[0] == 11 && seen[1] == 12);
	CHECK(task_count == 2 && timer_count == 0);

	/* Session work waits outside the executor. A concurrent core service
	 * can acquire/release it, and later MAC events remain ordered behind it.
	 */
	CHECK(q1000k_protocol_control() == -EPERM);
	reset_barriers(6);
	ret=q1000k_protocol_enter(); CHECK(ret == 0);
	CHECK(!q1000k_protocol_control() && !q1000k_protocol_control());
	CHECK(!q1000k_protocol_phy(7,13));
	q1000k_protocol_leave(ret);
	CHECK(wait_for_completion_timeout(&entered,HZ));
	CHECK(control_count == 1 && seen_count == 2);
	ret=q1000k_protocol_enter(); CHECK(ret == 0);
	q1000k_protocol_leave(ret);
	complete(&release_callback); flush_workqueue(qprotocol_wq);
	CHECK(seen_count == 3 && seen[2] == 13);
	WRITE_ONCE(block_kind,0);

	/* A service pause waits for a running IRQ; IRQ stays disabled until ack. */
	reset_barriers(1); inject_irq(); CHECK(wait_for_completion_timeout(&entered,HZ));
	CHECK(irq_depth == 1);
	thread=kthread_run(pause_thread,NULL,"protocol_pause_test"); CHECK(!IS_ERR(thread));
	msleep(20); CHECK(!completion_done(&thread_done));
	complete(&release_callback); CHECK(wait_for_completion_timeout(&thread_done,HZ));
	CHECK(!thread_ret); flush_workqueue(qprotocol_wq); CHECK(irq_depth == 0 && irq_count == 1);
	kthread_stop(thread); WRITE_ONCE(block_kind,0);

	/* Cancellation waits for an executing timer and removes its self-rearm. */
	reset_barriers(3); mod_timer(&sample_timer,jiffies+1);
	CHECK(wait_for_completion_timeout(&entered,HZ));
	thread=kthread_run(cancel_thread,NULL,"protocol_cancel_test"); CHECK(!IS_ERR(thread));
	msleep(20); CHECK(!completion_done(&thread_done));
	complete(&release_callback); CHECK(wait_for_completion_timeout(&thread_done,HZ));
	CHECK(thread_ret >= 0 && !timer_pending(&sample_timer)); kthread_stop(thread);
	WRITE_ONCE(block_kind,0);

	/* Stop must not return while a callback still owns module state. */
	reset_barriers(2); CHECK(!q1000k_protocol_phy(7,13));
	CHECK(wait_for_completion_timeout(&entered,HZ));
	thread=kthread_run(stop_thread,NULL,"protocol_stop_test"); CHECK(!IS_ERR(thread));
	msleep(20); CHECK(!completion_done(&thread_done));
	complete(&release_callback); CHECK(wait_for_completion_timeout(&thread_done,HZ));
	kthread_stop(thread); CHECK(!irq_requested && !qprotocol_wq);
	CHECK(q1000k_protocol_phy(7,14) == -ENODEV);
	CHECK(!timer_pending(&sample_timer)); WRITE_ONCE(block_kind,0);

	/* Overflow is sticky, runs containment once, and never dispatches a
	 * partially retained history as if no event had been lost. */
	CHECK(!q1000k_protocol_init(42,&test_ops)); CHECK(!q1000k_protocol_start());
	CHECK(!q1000k_protocol_pause());
	for (i=0;i<Q1000K_PROTOCOL_EVENTS;i++) CHECK(!q1000k_protocol_phy(7,i));
	CHECK(q1000k_protocol_phy(7,999) == -ENOSPC);
	CHECK(q1000k_protocol_resume() == -ENOSPC); flush_workqueue(qprotocol_wq);
	CHECK(fault_seen == -ENOSPC && seen_count == 4);
	CHECK(q1000k_protocol_pause() == -ENOSPC);
	CHECK(q1000k_protocol_phy(7,999) == -ENOSPC);
	q1000k_protocol_stop(); CHECK(!irq_requested);
	/* Unstarted teardown balances the initial disabled IRQ. Capacity
	 * failure initializes the rejected timer for safe caller cleanup.
	 */
	CHECK(!q1000k_protocol_init(42,&test_ops));
	for (i=0;i<Q1000K_PROTOCOL_TIMERS;i++)
		CHECK(!q1000k_protocol_timer_init(&capacity_timers[i],test_timer,0));
	CHECK(q1000k_protocol_timer_init(&capacity_timers[i],test_timer,0) == -ENOSPC);
	CHECK(q1000k_protocol_start() == -ENOSPC);
	q1000k_protocol_stop(); CHECK(!irq_requested && !irq_depth);
	CHECK(q1000k_protocol_timer_delete(&capacity_timers[i],true,true) >= 0);
	return 0;
}
static int __init protocol_test_init(void)
{
	int ret=run_test();
	if (ret) return ret;
	pr_info("Q1000K_PON_PROTOCOL_KERNEL_PASS\n"); return 0;
}
static void __exit protocol_test_exit(void) { }
module_init(protocol_test_init);
module_exit(protocol_test_exit);
MODULE_LICENSE("GPL");
