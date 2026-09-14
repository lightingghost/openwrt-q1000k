// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#include <q1000k_phy_api.h>
#define EXPORT_SYMBOL(x)
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define DEFINE_MUTEX(x) struct mutex x
#define IS_ENABLED(x) 0
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define TRUE 1
#define FALSE 0
#define PHY_DISABLE 0
#define PHY_ENABLE 1
#define PHY_TX_DIS_ON_HW_ONLY 2
#define PHY_TX_DIS_RESTORE_BY_SW 3
#define PHY_DEFAULT 2
#define PHY_XGSPON_CONFIG 4
#define PHY_UNKNOWN_CONFIG 0
#define PHY_TRANS_NOT_FOUND_IN_IOT_LIST 255
#define PHY_LINK_STATUS_UNKNOWN 0
#define I2C_U2_CLK_DIV 255
#define PHY_MSG_ERR 1
#define SCU_WAN_CONF_REG_WAN_SEL_BITS 0xff
#define SCU_WAN_CONF_REG_WAN_SEL_XGSPON 10
#define GFP_KERNEL 0
#define XPON_PHY_API_TYPE_GET 1
#define XPON_PHY_API_TYPE_SET 2
#define PON_SET_PHY_START 1
#define PON_SET_PHY_STOP 2
#define PON_SET_PHY_MODE_CONFIG 3
#define PON_SET_PHY_TRANS_POWER_SWITCH 4
#define PON_SET_PHY_TX_POWER_CONFIG 5
#define PON_GET_PHY_INIT_STATUS 6
#define PHY_ISR_FUNC 0
#define PHY_EVENT_POLL_FUNC 1
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQF_ONESHOT 1
#define IRQF_SHARED 2
/* REGISTERS */
struct task_struct { int id; };
static struct task_struct main_task, other_task;
static struct task_struct *current = &main_task;
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static bool mutex_trylock(struct mutex *m) { if(m->held) return false; m->held=true; return true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
static int atomic_context, irq_context, preempt_rcu;
static int in_atomic(void) { return atomic_context; }
static int in_interrupt(void) { return irq_context; }
static int irqs_disabled(void) { return irq_context; }
static int rcu_preempt_depth(void) { return preempt_rcu; }
static int rcu_read_lock_held(void) { return 1; } /* Real !lockdep stub. */
struct device { int n; };
struct timer_list { bool armed, dead; };
struct work_struct { bool queued; void (*fn)(struct work_struct *); };
#define DECLARE_WORK(n,f) struct work_struct n={.fn=f}
static unsigned long jiffies=100;
static unsigned int msecs_to_jiffies(unsigned int n) { return n; }
static void timer_setup(struct timer_list *t,void (*f)(struct timer_list *),int n) { memset(t,0,sizeof(*t)); }
static void mod_timer(struct timer_list *t,unsigned long n) { assert(!t->dead); t->armed=true; }
static void timer_delete_sync(struct timer_list *t) { t->armed=false; }
static void timer_shutdown_sync(struct timer_list *t) { t->armed=false; t->dead=true; }
static void schedule_work(struct work_struct *w) { w->queued=true; }
static void cancel_work_sync(struct work_struct *w);
static void spin_lock_init(int *p) { *p=1; }
static int no_memory, allocs;
static void *kzalloc(size_t size,int flags) { if(no_memory) return NULL; allocs++; return calloc(1,size); }
static void kfree(void *p) { if(p) allocs--; free(p); }
struct phy_private {
    int scu_hir_np_sys_hw_id,wan_sel,rx_fec_setting,trans_index,i2c_u2_clk_div,i2c_addr_num;
    int phy_status,trans_tx_enable,trans_tx_status,first_plugin_flag,trans_msg_print_cnt;
    int debugLevel,pon_stop_flag,event_poll_timer_value,event_handle_lock,pma_reset_lock;
    int is_phy_start,is_irq_requested,phy_init_done;
    struct { struct { int mode,txPowerEnFlag; } flags; } phyCfg;
    struct timer_list event_poll_timer;
};
static struct phy_private *gpPhyPriv;
static int (*en7581_xgpon_func[2])(char *);
static int (**ponPhyFunc)(char *);
struct ecnt_data { int n; };
struct xpon_phy_api_data_s { int ret,api_type,cmd_id; };
static int provider=1,provider_error,hwid=14,wan=10,mode_error,api_error,poll_error,isr_error,fw_error;
static unsigned int reads,writes,fail_read,fail_write;
static u32 regs[0x8000];
static int allocated_irq,irq_error,clear_error,clears,polls,isrs,fw_calls,api_calls,mode_calls;
static int reenter, cancel_run;
static struct device device;
typedef int irqreturn_t;
static int (*irq_fn)(int,void *);
static struct device *get_pon_phy_dev(void) { return provider ? &device : NULL; }
static int get_pon_phy_irq(void) { return provider ? 75 : -ENODEV; }
static int an7581_pon_phy_status(void) { return provider ? provider_error : -ENODEV; }
static int GET_HIR(void) { return hwid; }
static int GET_WAN_CONF(void) { return wan; }
static int an7581_pon_phy_read(u32 r,u32 *v) {
    reads++; if(reads==fail_read) return -EIO;
    *v=regs[(r&0x1ffff)/4]; return 0;
}
static int an7581_pon_phy_write(u32 r,u32 v) {
    writes++; if(writes==fail_write) return -EIO;
    regs[(r&0x1ffff)/4]=v; return 0;
}
static int request_threaded_irq(int irq,void *primary,int (*thread)(int,void *),int flags,const char *name,void *dev) {
    assert(irq==75 && !primary && flags==(IRQF_SHARED|IRQF_ONESHOT) && dev==&device);
    assert(!allocated_irq && gpPhyPriv && !gpPhyPriv->is_irq_requested);
    assert(regs[(EN7581_XGPON_PHY_XG_PON_INT_EN&0x1ffff)/4]==0);
    if(irq_error) return irq_error;
    allocated_irq=1; irq_fn=thread; return 0;
}
static void free_irq(int irq,void *dev);
static int pon_phy_clear_int(void) { clears++; return clear_error; }
static int phy_mode_config(int mode,int tx) {
    assert(mode==PHY_XGSPON_CONFIG && tx==PHY_DISABLE);
    mode_calls++; gpPhyPriv->phy_init_done=!mode_error; return mode_error;
}
static int phy_fw_ready(int enable) { assert(enable==PHY_DISABLE); fw_calls++; return fw_error; }
static void pon_phy_api_dispatch(struct ecnt_data *d) { struct xpon_phy_api_data_s *data=(void *)d; api_calls++; data->ret=api_error; }
static void phy_event_poll(struct timer_list *t) { q1000k_phy_poll(); }

struct q1000k_pon { bool held, tx; };
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
static void cancel_work_sync(struct work_struct *w)
{
    assert(!qphy_callback.held);
    if(cancel_run && w->queued) { cancel_run=0; w->fn(w); }
    w->queued=false;
}
static void free_irq(int irq,void *dev)
{
    assert(allocated_irq && irq==75 && dev==&device && !qphy_callback.held);
    assert(!qphy_active);
    assert(irq_fn(irq,dev)==IRQ_NONE); /* A final racing thread must do no work. */
    allocated_irq=0; irq_fn=NULL;
}
static int event(char *p)
{
    assert(qphy_callback.held);
    assert(!q1000k_phy_callback_context());
    current=&other_task; assert(q1000k_phy_callback_context()==-EPERM); current=&main_task;
    if(reenter) {
        struct xpon_phy_api_data_s call={.api_type=XPON_PHY_API_TYPE_GET};
        assert(q1000k_phy_start()==-EBUSY);
        assert(q1000k_phy_stop()==-EBUSY);
        assert(q1000k_phy_quiesce()==-EDEADLK);
        assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EBUSY);
        assert(q1000k_phy_call(&call)==-EBUSY);
    }
    return 0;
}
static int poll_event(char *p) { polls++; event(p); return poll_error; }
static int irq_event(char *p) { isrs++; event(p); return isr_error; }
static void reset(void)
{
    if(gpPhyPriv) q1000k_phy_exit();
    assert(!allocated_irq && !allocs && !qphy_control.held && !qphy_callback.held);
    qphy_dead=false; qphy_fault=0; qphy_active=false;
    assert(!controller.held); controller_error=pins_error=pbus_error=0;
    provider=1; provider_error=no_memory=irq_error=clear_error=mode_error=0;
    atomic_context=irq_context=preempt_rcu=0; hwid=14; wan=10;
    fail_read=fail_write=reads=writes=0; clears=polls=isrs=fw_calls=api_calls=mode_calls=0;
    api_error=poll_error=isr_error=fw_error=reenter=cancel_run=0;
    memset(regs,0,sizeof(regs));
    en7581_xgpon_func[PHY_EVENT_POLL_FUNC]=poll_event;
    en7581_xgpon_func[PHY_ISR_FUNC]=irq_event;
}
static void initialized(void)
{
    reset(); assert(!q1000k_phy_init()); assert(!writes && !allocated_irq);
    assert(!q1000k_phy_configure(PHY_XGSPON_CONFIG));
    assert(gpPhyPriv->phy_init_done && !qphy_active && !allocated_irq);
    reads=writes=0;
}
int main(void)
{
    unsigned int n;
    struct xpon_phy_api_data_s data={.api_type=XPON_PHY_API_TYPE_GET,.cmd_id=99};
    reset(); provider=0; assert(q1000k_phy_init()==-ENODEV && !allocs);
    reset(); hwid=7; assert(q1000k_phy_init()==-ENODEV && !allocs);
    reset(); provider_error=-EIO; assert(q1000k_phy_init()==-EIO && !allocs);
    reset(); no_memory=1; assert(q1000k_phy_init()==-ENOMEM && !allocs);
    reset(); assert(!q1000k_phy_init() && !writes);
    assert(q1000k_phy_start()==-EAGAIN);
    assert(q1000k_phy_configure(99)==-EOPNOTSUPP);
    wan=0; assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EINVAL && !mode_calls);
    assert(q1000k_phy_call(&data)==-EAGAIN && !api_calls);
    for(n=0;n<3;n++) {
        atomic_context=n==0; irq_context=n==1; preempt_rcu=n==2;
        assert(q1000k_phy_start()==-EWOULDBLOCK);
        assert(q1000k_phy_stop()==-EWOULDBLOCK);
        assert(q1000k_phy_quiesce()==-EWOULDBLOCK);
        assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EWOULDBLOCK);
        assert(q1000k_phy_call(&data)==-EWOULDBLOCK);
    }
    atomic_context=irq_context=preempt_rcu=0;
    reset(); assert(!q1000k_phy_init()); controller_error=-ENODEV;
    assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-ENODEV && !qphy_fault && !controller.held);
    controller_error=0; assert(!q1000k_phy_configure(PHY_XGSPON_CONFIG) && controller.held);
    assert(q1000k_phy_set_tx(true)==-EAGAIN && !controller.tx);
    assert(q1000k_phy_board_profile()==-EPERM);
    qphy_callback_lock();
    assert(!q1000k_phy_board_profile() && gpPhyPriv->trans_index==82);
    assert(regs[(EN7581_XGPON_PHY_SFP_VLD_LEVEL&0x1ffff)/4]==9);
    assert(regs[(EN7581_XPON_PMA_XPON_SETTING_0&0x1ffff)/4]==0x10001);
    assert(regs[(EN7581_XPON_PMA_XPON_SETTING_1&0x1ffff)/4]==0x1010100);
    assert(q1000k_phy_trans_power(PHY_ENABLE)==-EACCES);
    qphy_callback_unlock();
    assert(!q1000k_phy_start() && !controller.tx);
    assert(!q1000k_phy_set_tx(true) && controller.tx && gpPhyPriv->trans_tx_status==PHY_ENABLE);
    qphy_callback_lock();
    assert(!q1000k_phy_trans_power(PHY_TX_DIS_ON_HW_ONLY) && !controller.tx);
    assert(gpPhyPriv->trans_tx_status==PHY_ENABLE);
    assert(!q1000k_phy_trans_power(PHY_TX_DIS_RESTORE_BY_SW) && controller.tx);
    qphy_callback_unlock();
    assert(!q1000k_phy_stop() && !controller.tx && !gpPhyPriv->phyCfg.flags.txPowerEnFlag);
    for(n=0;n<2;n++) {
        reset(); assert(!q1000k_phy_init());
        pins_error=n==0 ? -ETIMEDOUT : 0; pbus_error=n==1 ? -ETIMEDOUT : 0;
        assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-ETIMEDOUT && qphy_fault && !controller.tx);
    }
    for(n=1;n<=3;n++) {
        initialized(); writes=0; fail_write=n; qphy_callback_lock();
        assert(q1000k_phy_board_profile()==-EIO && writes==n); qphy_callback_unlock();
        initialized(); reads=0; fail_read=n; qphy_callback_lock();
        assert(q1000k_phy_board_profile()==-EIO && reads==n); qphy_callback_unlock();
    }
    initialized(); mode_error=-EIO; gpPhyPriv->phy_init_done=0;
    assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EIO && qphy_fault);
    for(n=1;n<=7;n++) {
        initialized(); fail_write=n;
        assert(q1000k_phy_start()==-EIO && !allocated_irq && !qphy_active);
        assert(!gpPhyPriv->is_irq_requested && !gpPhyPriv->is_phy_start);
        assert(qphy_fault==-EIO && q1000k_phy_start()==-EIO);
        initialized(); fail_read=n;
        assert(q1000k_phy_start()==-EIO && !allocated_irq && !qphy_active);
    }
    initialized(); clear_error=-ETIMEDOUT;
    assert(q1000k_phy_start()==-ETIMEDOUT && !allocated_irq && qphy_fault);
    initialized(); irq_error=-EBUSY;
    assert(q1000k_phy_start()==-EBUSY && !allocated_irq && !qphy_fault);
    irq_error=0; assert(!q1000k_phy_start() && allocated_irq && qphy_active);
    assert(!q1000k_phy_start());
    assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EBUSY);
    assert(!q1000k_phy_call(&data) && api_calls==1);
    api_error=-EOPNOTSUPP; assert(q1000k_phy_call(&data)==-EOPNOTSUPP);
    assert(irq_fn(75,&device)==IRQ_NONE && !isrs);
    regs[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]=EN7581_XGPON_PHY_RX_LOS_INT_EN;
    reenter=1; assert(irq_fn(75,&device)==IRQ_HANDLED && isrs==1);
    q1000k_phy_poll(); assert(qphy_poll_job.queued);
    qphy_poll_job.fn(&qphy_poll_job); assert(polls==1 && gpPhyPriv->event_poll_timer.armed);
    cancel_run=1; assert(!q1000k_phy_stop());
    assert(!qphy_active && !allocated_irq && !qphy_poll_job.queued && !gpPhyPriv->event_poll_timer.armed);
    assert(polls==1 && fw_calls==1 && !gpPhyPriv->is_irq_requested);
    assert(!q1000k_phy_stop());
    for(n=0;n<20;n++) { assert(!q1000k_phy_start()); assert(!q1000k_phy_stop()); }
    initialized(); assert(!q1000k_phy_start());
    isr_error=-EIO; regs[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]=~0U;
    assert(irq_fn(75,&device)==IRQ_NONE && qphy_fault==-EIO && !qphy_active);
    assert(q1000k_phy_stop()==-EIO && !allocated_irq);
    initialized(); assert(!q1000k_phy_start());
    poll_error=-ERANGE; q1000k_phy_poll(); qphy_poll_job.fn(&qphy_poll_job);
    assert(qphy_fault==-ERANGE && !qphy_active);
    assert(q1000k_phy_stop()==-ERANGE && !allocated_irq);
    initialized(); assert(!q1000k_phy_start()); fw_error=-EIO;
    assert(q1000k_phy_stop()==-EIO && !allocated_irq && !qphy_active);
    initialized(); assert(!q1000k_phy_start()); q1000k_phy_exit();
    assert(!gpPhyPriv && !allocated_irq && !allocs && !qphy_poll_job.queued);
    assert(q1000k_phy_start()==-ENODEV && q1000k_phy_call(&data)==-ENODEV);
    q1000k_phy_exit();
    return 0;
}
