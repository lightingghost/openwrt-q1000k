// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/unaligned.h>
#ifndef CONFIG_UML
#error UML only; no device or optical hardware is attached.
#endif
/* API */
/* TYPES */
#define check(x) do { if (!(x)) { pr_err("Q1000K PHY assertion %s:%d: %s\n",__func__,__LINE__,#x); BUG(); } } while (0)
static struct device fake_dev;
static u32 registers[0x8000];
static DEFINE_SPINLOCK(register_lock);
static struct task_struct *fake_irq_task;
static irqreturn_t (*fake_irq_fn)(int,void *);
static bool fake_irq_owned, hold_event;
static DECLARE_COMPLETION(event_entered);
static DECLARE_COMPLETION(event_release);
static DECLARE_COMPLETION(exit_done);
static DECLARE_COMPLETION(irq_returned);
static unsigned int polls,irqs;
static int an7581_pon_phy_status(void) { return 0; }
static struct device *get_pon_phy_dev(void) { return &fake_dev; }
static int get_pon_phy_irq(void) { return 75; }
static int GET_HIR(void) { return 14; }
static u32 wan_mode=0x12;
static int GET_WAN_CONF(void) { return wan_mode; }
static int an7581_pon_wan_get(u32 *mode) { *mode=wan_mode; return 0; }
static int an7581_pon_wan_set(u32 mode) { check(mode==10); wan_mode=mode; return 0; }
static int an7581_pon_phy_read(u32 reg,u32 *value)
{
    unsigned long flags;
    spin_lock_irqsave(&register_lock,flags);
    *value=registers[(reg&0x1ffff)/4];
    spin_unlock_irqrestore(&register_lock,flags);
    return 0;
}
static int an7581_pon_phy_write(u32 reg,u32 value)
{
    unsigned long flags;
    spin_lock_irqsave(&register_lock,flags);
    if(reg==EN7581_XGPON_PHY_XG_PON_INT_STA) registers[(reg&0x1ffff)/4]&=~value;
    else registers[(reg&0x1ffff)/4]=value;
    spin_unlock_irqrestore(&register_lock,flags);
    return 0;
}
static int fake_request_threaded_irq(unsigned int irq,irq_handler_t primary,irq_handler_t thread,
                                     unsigned long flags,const char *name,void *dev)
{
    check(irq==75 && !primary && dev==&fake_dev && !fake_irq_owned);
    check(flags==(IRQF_SHARED|IRQF_ONESHOT));
    fake_irq_fn=thread; fake_irq_owned=true; return 0;
}
static void *fake_free_irq(unsigned int irq,void *dev)
{
    check(irq==75 && dev==&fake_dev && fake_irq_owned);
    if(fake_irq_task) { kthread_stop(fake_irq_task); fake_irq_task=NULL; }
    fake_irq_owned=false; fake_irq_fn=NULL; return NULL;
}
#define request_threaded_irq fake_request_threaded_irq
#define free_irq fake_free_irq
static int pon_phy_clear_int(void) { return 0; }
static int phy_mode_config(int mode,int tx)
{
    check(mode==PHY_XGSPON_CONFIG && tx==PHY_DISABLE);
    gpPhyPriv->phy_init_done=TRUE; return 0;
}

static void pon_phy_api_dispatch(struct ecnt_data *in)
{
    struct xpon_phy_api_data_s *data=(void *)in;
    data->ret=0;
}
static void phy_event_poll(struct timer_list *timer) { q1000k_phy_poll(); }

struct q1000k_pon { bool held, tx; };
static bool controller_inhibit, controller_los=true;
static struct q1000k_pon controller;
static int controller_error, pins_error, pbus_error;
static struct q1000k_pon *q1000k_pon_get(void)
{
    if (controller_error) return ERR_PTR(controller_error);
    controller.held=true; return &controller;
}
static int q1000k_pon_check(struct q1000k_pon *p)
{
    return p==&controller && p->held ? controller_error : -ENODEV;
}
static int q1000k_pon_receiver_startup(struct q1000k_pon *p)
{ return q1000k_pon_check(p); }
static int q1000k_pon_oem_post_init(struct q1000k_pon *p,bool restore)
{ (void)restore; return q1000k_pon_check(p); }
static int q1000k_pon_get_tx(struct q1000k_pon *p,bool *enabled)
{
    int ret=q1000k_pon_check(p); if (!ret) *enabled=p->tx; return ret;
}
static int power_error;
static int q1000k_pon_get_rx_power(struct q1000k_pon *p,u32 *value)
{
    int ret=q1000k_pon_check(p); if (!ret) ret=power_error;
    if (!ret) *value=19900;
    return ret;
}
static int q1000k_pon_get_tx_inhibit(struct q1000k_pon *p,bool *inhibited)
{
    int ret=q1000k_pon_check(p); if(!ret) *inhibited=controller_inhibit; return ret;
}
static int q1000k_pon_get_los(struct q1000k_pon *p)
{
    int ret=q1000k_pon_check(p); return ret ? ret : controller_los;
}
static int q1000k_pon_set_tx(struct q1000k_pon *p,bool enable)
{
    int ret=q1000k_pon_check(p); if (!ret) p->tx=enable; return ret;
}
static int q1000k_pon_put(struct q1000k_pon *p)
{
    int ret=q1000k_pon_set_tx(p,false); p->held=false; return ret;
}
static int an7581_pon_phy_prepare_pins(void) { return pins_error; }
static int an7581_pon_pbus_enable(void) { return pbus_error; }
/* PRODUCTION */
int q1000k_phy_rx_cleanup(void) {
    check(!q1000k_phy_callback_context() && !qphy_active && !controller.tx);
    return 0;
}

static int handle_event(char *p)
{
    struct xpon_phy_api_data_s query={.api_type=XPON_PHY_API_TYPE_GET};
    check(lockdep_is_held(&qphy_callback));
    check(!in_atomic() && !irqs_disabled() && !rcu_read_lock_held());
    check(q1000k_phy_start()==-EDEADLK);
    check(q1000k_phy_stop()==-EDEADLK);
    check(q1000k_phy_quiesce()==-EDEADLK);
    check(q1000k_phy_call(&query)==-EDEADLK);
    u32 power; check(q1000k_phy_get_rx_power(&power)==-EDEADLK);
    complete(&event_entered);
    if(READ_ONCE(hold_event)) wait_for_completion(&event_release);
    return 0;
}
static int handle_poll(char *p) { polls++; return handle_event(p); }
static int handle_irq(char *p) { irqs++; return handle_event(p); }
static unsigned int reacquisitions;
int q1000k_phy_rx_probe(u32 mode) { return q1000k_phy_rx_reacquire(false, false); }
int q1000k_phy_rx_probe_cleanup(void) { return 0; }
u32 q1000k_phy_rx_probe_writes(void) { return 0; }
int q1000k_phy_rx_reacquire(bool restore_pll, bool restore_gain)
{
    check(restore_pll == qphy_rx_restore_pll);
    check(restore_gain == qphy_rx_restore_gain);
    check(controller_inhibit && !controller.tx && qphy_rx_attempts==1);
    reacquisitions++;
    return handle_event(NULL);
}
static int irq_task(void *unused)
{
    check(fake_irq_fn(75,&fake_dev)==IRQ_HANDLED);
    complete(&irq_returned);
    while(!kthread_should_stop()) { set_current_state(TASK_INTERRUPTIBLE); schedule(); }
    __set_current_state(TASK_RUNNING);
    return 0;
}
static int exit_task(void *quiesce)
{
    if (quiesce) check(!q1000k_phy_quiesce());
    else q1000k_phy_exit();
    complete(&exit_done);
    while(!kthread_should_stop()) { set_current_state(TASK_INTERRUPTIBLE); schedule(); }
    __set_current_state(TASK_RUNNING);
    return 0;
}
static void start_session(void)
{
    qphy_dead=false; qphy_fault=0;
    check(!q1000k_phy_init());
    check(!q1000k_phy_prepare_wan() && wan_mode==10 && !controller.tx);
    check(!q1000k_phy_configure(PHY_XGSPON_CONFIG));
    check(!q1000k_phy_start());
}
static void stop_during_callback(bool irq, bool quiesce)
{
    struct task_struct *task;
    unsigned int n;
    start_session();
    reinit_completion(&event_entered); reinit_completion(&event_release); reinit_completion(&exit_done);
    WRITE_ONCE(hold_event,true);
    if(irq) {
        registers[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]=EN7581_XGPON_PHY_RX_LOS_INT_EN;
        fake_irq_task=kthread_run(irq_task,NULL,"qphy-irq-test");
        check(!IS_ERR(fake_irq_task));
    } else {
        q1000k_phy_poll();
    }
    check(wait_for_completion_timeout(&event_entered,5*HZ));
    task=kthread_run(exit_task,(void *)(unsigned long)quiesce,"qphy-exit-test"); check(!IS_ERR(task));
    for(n=0;n<500 && READ_ONCE(qphy_active);n++) msleep(1);
    check(!READ_ONCE(qphy_active));
    check(READ_ONCE(qphy_dead)==!quiesce);
    /* The callback is still using this state: exit must remain blocked. */
    check(gpPhyPriv && !completion_done(&exit_done));
    WRITE_ONCE(hold_event,false); complete(&event_release);
    check(wait_for_completion_timeout(&exit_done,5*HZ));
    kthread_stop(task);
    if (quiesce) {
        check(gpPhyPriv && !controller.tx && !qphy_active);
        q1000k_phy_exit();
    }
    check(!gpPhyPriv && !fake_irq_owned && !fake_irq_task && !work_busy(&qphy_poll_job));
}
static int tx_after_event(void *unused)
{
    check(!q1000k_phy_set_tx(true));
    complete(&exit_done);
    while (!kthread_should_stop()) { set_current_state(TASK_INTERRUPTIBLE); schedule(); }
    __set_current_state(TASK_RUNNING); return 0;
}
static void control_during_callback(void)
{
    struct task_struct *task;
    start_session();
    reinit_completion(&event_entered); reinit_completion(&event_release); reinit_completion(&exit_done);
    WRITE_ONCE(hold_event,true); q1000k_phy_poll();
    check(wait_for_completion_timeout(&event_entered,5*HZ));
    task=kthread_run(tx_after_event,NULL,"qphy-tx-test"); check(!IS_ERR(task));
    msleep(20); check(!completion_done(&exit_done) && !controller.tx);
    WRITE_ONCE(hold_event,false); complete(&event_release);
    check(wait_for_completion_timeout(&exit_done,5*HZ) && controller.tx);
    kthread_stop(task); q1000k_phy_exit();
}
static int power_after_event(void *unused)
{
    u32 value=0;
    check(!q1000k_phy_get_rx_power(&value) && value==19900);
    complete(&exit_done);
    while (!kthread_should_stop()) { set_current_state(TASK_INTERRUPTIBLE); schedule(); }
    __set_current_state(TASK_RUNNING); return 0;
}
static void power_during_callback(void)
{
    struct task_struct *task;
    u32 value=123;
    start_session();
    reinit_completion(&event_entered); reinit_completion(&event_release); reinit_completion(&exit_done);
    WRITE_ONCE(hold_event,true); q1000k_phy_poll();
    check(wait_for_completion_timeout(&event_entered,5*HZ));
    task=kthread_run(power_after_event,NULL,"qphy-power-test"); check(!IS_ERR(task));
    msleep(20); check(!completion_done(&exit_done));
    WRITE_ONCE(hold_event,false); complete(&event_release);
    check(wait_for_completion_timeout(&exit_done,5*HZ));
    kthread_stop(task);
    power_error=-ENODATA;
    check(q1000k_phy_get_rx_power(&value)==-ENODATA && value==123 && !qphy_fault);
    power_error=0;
    check(!q1000k_phy_stop());
    check(q1000k_phy_get_rx_power(&value)==-EAGAIN && value==123);
    q1000k_phy_exit();
}
static void stop_during_reacquire(bool quiesce, bool restore_pll, bool restore_gain, u32 probe)
{
    struct task_struct *task;
    unsigned int n, previous=reacquisitions;
    qphy_dead=false; qphy_fault=0; controller_los=false;
    check(!q1000k_phy_init() && !q1000k_phy_set_rx_bench(true, true, restore_pll, restore_gain));
    check(!q1000k_phy_set_rx_probe(probe));
    check(!q1000k_phy_configure(PHY_XGSPON_CONFIG) && !q1000k_phy_start());
    registers[(EN7581_XGPON_PHY_SFP_STA&0x1ffff)/4]=0;
    if(probe==Q1000K_RX_PROBE_CHECKER_DARK) {
        q1000k_phy_poll(); flush_work(&qphy_poll_job);
        controller_los=true;
        registers[(EN7581_XGPON_PHY_SFP_STA&0x1ffff)/4]=EN7581_XGPON_PHY_SFP_RX_LOS_ST;
    }
    for(n=0;n<9;n++) { q1000k_phy_poll(); flush_work(&qphy_poll_job); }
    check(reacquisitions==previous);
    reinit_completion(&event_entered); reinit_completion(&event_release); reinit_completion(&exit_done);
    WRITE_ONCE(hold_event,true); q1000k_phy_poll();
    check(wait_for_completion_timeout(&event_entered,5*HZ));
    task=kthread_run(exit_task,(void *)(unsigned long)quiesce,"qphy-rx-exit"); check(!IS_ERR(task));
    for(n=0;n<500 && READ_ONCE(qphy_active);n++) msleep(1);
    check(!qphy_active && gpPhyPriv && !completion_done(&exit_done));
    WRITE_ONCE(hold_event,false); complete(&event_release);
    check(wait_for_completion_timeout(&exit_done,5*HZ)); kthread_stop(task);
    check(reacquisitions==previous+1 && !controller.tx && !qphy_fault);
    if(quiesce) q1000k_phy_exit();
    check(!gpPhyPriv && !fake_irq_owned && !work_busy(&qphy_poll_job));
    controller_los=true;
}
static int __init phy_test_init(void)
{
    struct xpon_phy_api_data_s data={.api_type=XPON_PHY_API_TYPE_GET};
    unsigned long flags;
    unsigned int n;
    if(!strstr(init_uts_ns.name.release,"q1000k-pon-phy-test")) return -EPERM;
    en7581_xgpon_func[PHY_ISR_FUNC]=handle_irq;
    en7581_xgpon_func[PHY_EVENT_POLL_FUNC]=handle_poll;
    check(!q1000k_phy_init());
    check(!q1000k_phy_prepare_wan() && wan_mode==10 && !controller.tx);
    check(!q1000k_phy_configure(PHY_XGSPON_CONFIG));
    rcu_read_lock();
    check(q1000k_phy_start()==-EWOULDBLOCK && q1000k_phy_call(&data)==-EWOULDBLOCK);
    rcu_read_unlock();
    local_irq_save(flags); check(q1000k_phy_start()==-EWOULDBLOCK); local_irq_restore(flags);
    for(n=0;n<50;n++) {
        check(!q1000k_phy_start());
        reinit_completion(&event_entered); q1000k_phy_poll();
        check(wait_for_completion_timeout(&event_entered,5*HZ));
        flush_work(&qphy_poll_job);
        check(!q1000k_phy_stop());
        check(!qphy_active && !fake_irq_owned && !timer_pending(&gpPhyPriv->event_poll_timer));
    }
    q1000k_phy_exit();
    stop_during_callback(false,false);
    stop_during_callback(true,false);
    stop_during_callback(false,true);
    stop_during_callback(true,true);
    control_during_callback();
    check(polls==53 && irqs==2);
    power_during_callback();
    qphy_dead=false; qphy_fault=0; controller_inhibit=true;
    check(!q1000k_phy_init()); check(!q1000k_phy_set_rx_bench(true, false, false, false));
    check(!q1000k_phy_configure(PHY_XGSPON_CONFIG));
    for(n=0;n<50;n++) {
        struct q1000k_rx_sample sample;
        check(!q1000k_phy_start());
        registers[(EN7581_XPON_PMA_RO_RX_FREQDET&0x1ffff)/4]=n;
        registers[(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL&0x1ffff)/4]=0x12340000+n;
        registers[(EN7581_XPON_PMA_RX_FORCE_MODE_9&0x1ffff)/4]=0x100+n;
        registers[(EN7581_XPON_PMA_SS_LCPLL_TDC_PCW_2&0x1ffff)/4]=0x200+n;
        q1000k_phy_poll(); flush_work(&qphy_poll_job);
        registers[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]=QPHY_RX_BENCH_IRQS;
        reinit_completion(&irq_returned);
        fake_irq_task=kthread_run(irq_task,NULL,"q1000k-rx-irq"); check(!IS_ERR(fake_irq_task));
        check(wait_for_completion_timeout(&irq_returned,5*HZ));
        registers[(EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN&0x1ffff)/4]=0x300+n;
        registers[(EN7581_XGPON_PHY_DBG_PSYNC_MISMATCH_CNT&0x1ffff)/4]=n+10;
        registers[(EN7581_XGPON_PHY_DBG_RX_CW_START_CNT&0x1ffff)/4]=~0U;
        check(!q1000k_phy_rx_sample(&sample) && sample.controller_los && !sample.synced);
        check(sample.pcs_counters.psync_mismatch==n+10 && sample.pcs_counters.cw_start==~0U);
        struct q1000k_rx_diagnostics diagnostic;
        registers[(EN7581_XPON_PMA_SS_LCPLL_TDC_RO_4&0x1ffff)/4]=0x400+n;
        registers[(EN7581_XPON_PMA_FIFO_CK_STATUS&0x1ffff)/4]=0x500+n;
        check(!q1000k_phy_snapshot(&sample,&diagnostic) && diagnostic.probe==0);
        check(diagnostic.sampled_ms>=sample.sampled_ms && !sample.tx_enabled && sample.tx_inhibited);
        check(diagnostic.rx_meter_result==n);
        check(diagnostic.tdc_ncpo==0x400+n && diagnostic.fifo_clock_status==0x500+n);
        check(sample.rx_power_valid && sample.rx_power_nw==19900);
        check(sample.receiver.pll_outputs==0x300+n && !sample.pll_restore_enabled);
        check(sample.receiver.rx_frequency==n && sample.receiver.rx_control==0x12340000+n);
        check(sample.receiver.rx_lock_force==0x100+n && sample.receiver.pll_pcw2==0x200+n);
        check(!controller.tx && q1000k_phy_set_rx_bench(false, false, false, false)==-EBUSY);
        check(!q1000k_phy_stop() && !fake_irq_owned && !timer_pending(&gpPhyPriv->event_poll_timer));
    }
    check(polls==54 && irqs==2 && qphy_rx_polls==50 && qphy_rx_irqs==50);
    q1000k_phy_exit();
    check(!controller.held && !gpPhyPriv);
    stop_during_reacquire(false, false, false, 0);
    stop_during_reacquire(false, true, false, 0);
    stop_during_reacquire(true, false, false, 0);
    stop_during_reacquire(true, true, false, 0);
    stop_during_reacquire(false, false, true, 0);
    stop_during_reacquire(true, true, true, 0);
    for (u32 mode=1;mode<Q1000K_RX_PROBE_COUNT;mode++) {
        stop_during_reacquire(false,false,false,mode);
        stop_during_reacquire(true,false,false,mode);
    }
    check(reacquisitions==6+2*(Q1000K_RX_PROBE_COUNT-1) && !controller.held);
    pr_info("Q1000K_PON_PHY_KERNEL_PASS: 50 normal + 50 RX-only cycles, RCU guards, concurrent poll and IRQ teardown\n");
    return 0;
}
static void __exit phy_test_exit(void) {}
module_init(phy_test_init);
module_exit(phy_test_exit);
MODULE_LICENSE("GPL");
