// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
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
#define PON_SET_PHY_FW_READY 6
#define PON_SET_PHY_RX_FEC_SETTING 20
#define PON_SET_PHY_XGPON_RX_ENABLE 30
#define PON_SET_PHY_XGPON_RX_DISABLE 31
#define DS_FEC_SETTING_FORCE_ON 1
#define DS_FEC_SETTING_FORCE_OFF 0
#define DS_FEC_SETTING_FORCE_OC 3
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
#define ktime_to_ms(t) (t)
static u64 ktime_get_boottime(void) { return jiffies; }
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
typedef struct { u32 correct_bytes,correct_codewords,uncorrect_codewords,total_rx_codewords,fec_seconds; } PHY_FecCount_T;
typedef struct { u32 frame_count_low,frame_count_high,lof_counter; } PHY_FrameCount_T;
struct xpon_phy_api_data_s { int ret,api_type,cmd_id; union { int *data; PHY_FecCount_T *rx_fec_cnt; PHY_FrameCount_T *rx_frame_cnt; }; };
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
    if(r==EN7581_XGPON_PHY_XG_PON_INT_STA) regs[(r&0x1ffff)/4]&=~v;
    else regs[(r&0x1ffff)/4]=v;
    return 0;
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
static int q1000k_pon_get_tx(struct q1000k_pon *p,bool *enabled)
{
    int ret=q1000k_pon_check(p); if (!ret) *enabled=p->tx; return ret;
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
static int wan_error, wan_set_error, wan_writes;
static int an7581_pon_wan_get(u32 *mode) { *mode=wan; return wan_error; }
static int an7581_pon_wan_set(u32 mode)
{
    assert(controller.held && !controller.tx && mode==10);
    wan_writes++; if(wan_set_error) return wan_set_error; wan=mode; return 0;
}
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
        bool tx;
        assert(q1000k_phy_get_tx(&tx)==-EDEADLK);
        struct xpon_phy_api_data_s call={.api_type=XPON_PHY_API_TYPE_GET};
        assert(q1000k_phy_start()==-EDEADLK);
        assert(q1000k_phy_stop()==-EDEADLK);
        assert(q1000k_phy_quiesce()==-EDEADLK);
        assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EDEADLK);
        assert(q1000k_phy_prepare_wan()==-EDEADLK);
        assert(q1000k_phy_needs_configure()==-EDEADLK);
        assert(q1000k_phy_call(&call)==-EDEADLK);
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
    wan_error=wan_set_error=wan_writes=0;
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
    assert(!q1000k_phy_needs_configure());
    assert(gpPhyPriv->phy_init_done && !qphy_active && !allocated_irq);
    reads=writes=0;
}
static void checked_queries(void)
{
    PHY_FecCount_T fec,saved;
    PHY_FrameCount_T frames,previous;
    struct xpon_phy_api_data_s q={.api_type=XPON_PHY_API_TYPE_GET};
    initialized();
    regs[(EN7581_XGPON_PHY_DBG_RX_SYNC_ST&0x1ffff)/4]=EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC;
    q.cmd_id=PON_GET_PHY_READY_STATUS; assert(q1000k_phy_call(&q)==1 && q.ret==1);
    q.cmd_id=PON_GET_PHY_IS_SYNC; assert(q1000k_phy_call(&q)==1);
    regs[(EN7581_XGPON_PHY_SFP_STA&0x1ffff)/4]=EN7581_XGPON_PHY_SFP_RX_LOS_ST;
    assert(!q1000k_phy_call(&q));
    q.cmd_id=PON_GET_PHY_LOS_STATUS; assert(q1000k_phy_call(&q)==1);
    controller.tx=true; gpPhyPriv->phyCfg.flags.txPowerEnFlag=false;
    q.cmd_id=PON_GET_PHY_GET_TX_POWER_EN_FLAG; assert(q1000k_phy_call(&q)==1);
    gpPhyPriv->phyCfg.flags.mode=PHY_XGSPON_CONFIG;
    q.cmd_id=PON_GET_PHY_MODE; assert(q1000k_phy_call(&q)==PHY_XGSPON_CONFIG);
    regs[(EN7581_XGPON_PHY_DBG_CTRL&0x1ffff)/4]=EN7581_XGPON_PHY_DBG_RX_FEC_FORCE_OFF;
    q.cmd_id=PON_GET_PHY_RX_FEC_GETTING; assert(q1000k_phy_call(&q)==1);
    regs[(EN7581_XGPON_PHY_DBG_TX_FEC_STA&0x1ffff)/4]=EN7581_XGPON_PHY_TX_FEC;
    q.cmd_id=PON_GET_PHY_TX_FEC_STATUS; assert(q1000k_phy_call(&q)==1);
    q.cmd_id=PON_GET_PHY_RX_FEC_COUNTER; q.rx_fec_cnt=NULL; assert(q1000k_phy_call(&q)==-EINVAL);
    q.cmd_id=PON_GET_PHY_RX_FRAME_COUNTER; assert(q1000k_phy_call(&q)==-EINVAL);
    assert(!qphy_fault && !writes && !api_calls);
    for(unsigned int cmd=0x8000;cmd<0x8080;cmd++) {
        switch(cmd) {
        case PON_GET_PHY_INIT_STATUS: case PON_GET_PHY_MODE: case PON_GET_PHY_GET_TX_POWER_EN_FLAG:
        case PON_GET_PHY_LOS_STATUS: case PON_GET_PHY_READY_STATUS: case PON_GET_PHY_IS_SYNC:
        case PON_GET_PHY_RX_FEC_GETTING: case PON_GET_PHY_TX_FEC_STATUS:
        case PON_GET_PHY_RX_FEC_COUNTER: case PON_GET_PHY_RX_FRAME_COUNTER: continue;
        }
        unsigned int before=reads; q.cmd_id=cmd;
        assert(q1000k_phy_call(&q)==-EOPNOTSUPP && reads==before && !writes && !qphy_fault);
    }
    for(unsigned int failure=0;failure<=5;failure++) {
        initialized(); memset(&fec,0xa5,sizeof(fec)); saved=fec;
        regs[(EN7581_XGPON_PHY_FEC_CORRECTED_BYTE_CNT&0x1ffff)/4]=~0U;
        regs[(EN7581_XGPON_PHY_FEC_CORRECTED_CW_CNT&0x1ffff)/4]=31;
        regs[(EN7581_XGPON_PHY_FEC_UNCORRECTED_CW_CNT&0x1ffff)/4]=32;
        regs[(EN7581_XGPON_PHY_FEC_TOTAL_CW_CNT&0x1ffff)/4]=33;
        regs[(EN7581_XGPON_PHY_FEC_ERR_SECONDS&0x1ffff)/4]=34;
        q.cmd_id=PON_GET_PHY_RX_FEC_COUNTER; q.rx_fec_cnt=&fec; fail_read=failure;
        if(failure) assert(q1000k_phy_call(&q)==-EIO && !memcmp(&fec,&saved,sizeof(fec)) && qphy_fault);
        else {
            assert(!q1000k_phy_call(&q) && !writes && !qphy_fault);
            assert(fec.correct_bytes==~0U && fec.correct_codewords==31 && fec.uncorrect_codewords==32 && fec.total_rx_codewords==33 && fec.fec_seconds==34);
        }
    }
    for(unsigned int failure=0;failure<=2;failure++) {
        initialized(); memset(&frames,0x55,sizeof(frames)); previous=frames;
        regs[(EN7581_XGPON_PHY_DBG_RX_FRAME2PHYD_CNT&0x1ffff)/4]=~0U;
        regs[(EN7581_XGPON_PHY_DBG_LOF_CNT&0x1ffff)/4]=123;
        q.cmd_id=PON_GET_PHY_RX_FRAME_COUNTER; q.rx_frame_cnt=&frames; fail_read=failure;
        if(failure) assert(q1000k_phy_call(&q)==-EIO && !memcmp(&frames,&previous,sizeof(frames)));
        else assert(!q1000k_phy_call(&q) && frames.frame_count_low==~0U && !frames.frame_count_high && frames.lof_counter==123 && !writes);
    }
    initialized(); q.cmd_id=PON_GET_PHY_READY_STATUS;
    regs[(EN7581_XGPON_PHY_DBG_RX_SYNC_ST&0x1ffff)/4]=~0U;
    assert(q1000k_phy_call(&q)==-EIO && qphy_fault && !controller.tx);
    initialized(); controller_error=-ETIMEDOUT;
    assert(q1000k_phy_call(&q)==-ETIMEDOUT && qphy_fault==-ETIMEDOUT);
}
static void prepare_wan(void)
{
    reset(); assert(!q1000k_phy_init()); wan=0x12;
    controller_error=-ENODEV;
    assert(q1000k_phy_needs_configure()==1);
    assert(q1000k_phy_prepare_wan()==-ENODEV && !wan_writes && wan==0x12);
    controller_error=0; controller.tx=true;
    assert(q1000k_phy_prepare_wan()==-EBUSY && !wan_writes);
    controller.tx=false;
    assert(!q1000k_phy_prepare_wan() && wan==10 && wan_writes==1 && controller.held);
    assert(!writes && !mode_calls && !allocated_irq && !controller.tx);
    assert(!q1000k_phy_prepare_wan() && wan_writes==1);
    for(int unknown=0;unknown<256;unknown++) {
        if(unknown==10 || unknown==0x12) continue;
        reset(); assert(!q1000k_phy_init()); wan=unknown;
        assert(q1000k_phy_prepare_wan()==-EOPNOTSUPP && !wan_writes && !controller.held);
    }
    reset(); assert(!q1000k_phy_init()); wan=0x12; wan_error=-EIO;
    assert(q1000k_phy_prepare_wan()==-EIO && !wan_writes);
    reset(); assert(!q1000k_phy_init()); wan=0x12; wan_set_error=-EIO;
    assert(q1000k_phy_prepare_wan()==-EIO && wan_writes==1 && wan==0x12);
    assert(qphy_fault==-EIO && !controller.tx);
    initialized(); wan=0x12;
    assert(q1000k_phy_prepare_wan()==-EBUSY && !wan_writes);
    reset();
}
static void rx_bench_tests(void)
{
    struct q1000k_rx_sample sample, saved;
    reset(); assert(!q1000k_phy_init()); controller_inhibit=false;
    assert(!q1000k_phy_set_rx_bench(false));
    assert(q1000k_phy_set_rx_bench(true)==-EACCES && !qphy_rx_bench);
    controller_inhibit=true; controller.tx=true;
    assert(q1000k_phy_set_rx_bench(true)==-EACCES && !qphy_rx_bench);
    controller.tx=false; assert(!q1000k_phy_set_rx_bench(true));
    assert(q1000k_phy_rx_sample(&sample)==-EAGAIN);
    assert(!q1000k_phy_configure(PHY_XGSPON_CONFIG));
    assert(q1000k_phy_set_rx_bench(false)==-EBUSY);
    assert(!q1000k_phy_set_rx_bench(true));
    assert(!q1000k_phy_start());
    regs[(EN7581_XGPON_PHY_SFP_STA&0x1ffff)/4]=EN7581_XGPON_PHY_SFP_RX_LOS_ST;
    controller_los=true;
    assert(!q1000k_phy_rx_sample(&sample) && sample.controller_los && sample.phy_los && !sample.synced);
    unsigned int old_writes=writes;
    controller_los=false; regs[(EN7581_XGPON_PHY_SFP_STA&0x1ffff)/4]=0;
    regs[(EN7581_XGPON_PHY_DBG_RX_SYNC_ST&0x1ffff)/4]=EN7581_XGPON_PHY_DBG_RX_SYNC_ST_SYNC;
    regs[(EN7581_XGPON_PHY_DBG_RX_FRAME2PHYD_CNT&0x1ffff)/4]=~0U;
    assert(!q1000k_phy_rx_sample(&sample) && sample.synced && sample.frames==~0U && old_writes==writes);
    unsigned int old_polls=polls, old_isrs=isrs;
    qphy_poll_work(&qphy_poll_job); assert(polls==old_polls && qphy_rx_polls==1);
    regs[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]=QPHY_RX_BENCH_IRQS|EN7581_XGPON_PHY_TX_FAULT_INT_EN;
    assert(irq_fn(75,&device)==IRQ_HANDLED && isrs==old_isrs && qphy_rx_irqs==1);
    assert(regs[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]==EN7581_XGPON_PHY_TX_FAULT_INT_EN);
    assert(irq_fn(75,&device)==IRQ_NONE);
    saved=sample; fail_read=reads+4;
    assert(q1000k_phy_rx_sample(&sample)==-EIO && !memcmp(&sample,&saved,sizeof(sample)));
    assert(qphy_fault==-EIO && !qphy_active && !controller.tx);
    reset(); assert(!q1000k_phy_init()); assert(!q1000k_phy_set_rx_bench(true));
    assert(!q1000k_phy_configure(PHY_XGSPON_CONFIG)); assert(!q1000k_phy_start());
    assert(q1000k_phy_set_tx(true)==-EACCES && !controller.tx && !qphy_active);
    reset(); controller_inhibit=false; controller_los=true;
}
int main(void)
{
    rx_bench_tests();
    prepare_wan();
    unsigned int n;
    struct xpon_phy_api_data_s data={.api_type=XPON_PHY_API_TYPE_GET,.cmd_id=99};
    struct q1000k_pon_profile profile={.repeat=255,.preamble_len=8,.delimiter_len=4,.fec=1,.version=15};
    checked_queries();
    memset(profile.preamble,0xff,8); memcpy(profile.delimiter,"abcdefgh",8);
    for(int index=0;index<4;index++) {
        initialized(); profile.index=index;
        regs[(EN7581_XGPON_PHY_XG_TX_FEC_EN_CTRL&0x1ffff)/4]=0x80808080;
        assert(!q1000k_phy_profile_set(&profile));
        assert(regs[((EN7581_XGPON_PHY_PREAMBLE1_UPPER+8*index)&0x1ffff)/4]==0xffffffff);
        assert(regs[((EN7581_XGPON_PHY_DELIMITER1_UPPER+8*index)&0x1ffff)/4]==0x61626364);
        assert(regs[((EN7581_XGPON_PHY_PSBU_INFO1+4*index)&0x1ffff)/4]==0xff0804);
        assert(regs[(EN7581_XGPON_PHY_XG_TX_FEC_EN_CTRL&0x1ffff)/4]==(0x80808080|(1U<<(8*index))));
    }
    unsigned int profile_writes=writes, profile_reads=reads;
    for(unsigned int n=1;n<=profile_writes;n++) {
        initialized(); fail_write=n;
        assert(q1000k_phy_profile_set(&profile)==-EIO && qphy_fault && !controller.tx);
    }
    for(unsigned int n=1;n<=profile_reads;n++) {
        initialized(); fail_read=n;
        assert(q1000k_phy_profile_set(&profile)==-EIO && qphy_fault && !controller.tx);
    }
    initialized(); profile.index=4;
    assert(q1000k_phy_profile_set(&profile)==-EINVAL && !writes);
    profile.index=0; profile.preamble_len=9;
    assert(q1000k_phy_profile_set(&profile)==-EINVAL && !writes); profile.preamble_len=8;
    assert(!q1000k_phy_start());
    assert(q1000k_phy_profile_set(&profile)==-EBUSY);
    assert(!q1000k_phy_stop());
    int fec=1;
    struct xpon_phy_api_data_s receive={.api_type=XPON_PHY_API_TYPE_SET,.cmd_id=PON_SET_PHY_RX_FEC_SETTING,.data=&fec};
    assert(!q1000k_phy_call(&receive) && gpPhyPriv->rx_fec_setting==1);
    fec=4; assert(q1000k_phy_call(&receive)==-EINVAL && gpPhyPriv->rx_fec_setting==1);
    fec=-1; assert(q1000k_phy_call(&receive)==-EINVAL);
    receive.data=NULL; assert(q1000k_phy_call(&receive)==-EINVAL);
    receive.cmd_id=PON_SET_PHY_XGPON_RX_ENABLE;
    assert(!q1000k_phy_call(&receive));
    assert(regs[(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL&0x1ffff)/4] & (1U<<16));
    assert(!q1000k_phy_start());
    receive.cmd_id=PON_SET_PHY_XGPON_RX_DISABLE;
    assert(q1000k_phy_call(&receive)==-EBUSY);
    for(unsigned int cmd=0;cmd<128;cmd++) {
        if(cmd==PON_SET_PHY_RX_FEC_SETTING || cmd==PON_SET_PHY_XGPON_RX_ENABLE || cmd==PON_SET_PHY_XGPON_RX_DISABLE) continue;
        receive.cmd_id=cmd; assert(q1000k_phy_call(&receive)==-EOPNOTSUPP);
    }
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
        assert(q1000k_phy_prepare_wan()==-EWOULDBLOCK);
        assert(q1000k_phy_needs_configure()==-EWOULDBLOCK);
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
    bool tx=true;
    assert(q1000k_phy_get_tx(NULL)==-EINVAL);
    assert(!q1000k_phy_get_tx(&tx) && !tx);
    assert(!q1000k_phy_start() && !controller.tx);
    assert(!q1000k_phy_set_tx(true) && controller.tx && gpPhyPriv->trans_tx_status==PHY_ENABLE);
    assert(!q1000k_phy_get_tx(&tx) && tx);
    qphy_callback_lock();
    assert(!q1000k_phy_trans_power(PHY_TX_DIS_ON_HW_ONLY) && !controller.tx);
    assert(gpPhyPriv->trans_tx_status==PHY_ENABLE);
    assert(!q1000k_phy_trans_power(PHY_TX_DIS_RESTORE_BY_SW) && controller.tx);
    qphy_callback_unlock();
    assert(!q1000k_phy_stop() && !controller.tx && !gpPhyPriv->phyCfg.flags.txPowerEnFlag);
    assert(!q1000k_phy_get_tx(&tx) && !tx);
    controller_error=-EIO; tx=true;
    assert(q1000k_phy_get_tx(&tx)==-EIO && tx && qphy_fault && !qphy_active);
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
    initialized(); controller.tx=true; mode_calls=0;
    assert(q1000k_phy_configure(PHY_XGSPON_CONFIG)==-EBUSY && !mode_calls && !controller.tx);
    assert(q1000k_phy_needs_configure()==-EBUSY);
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
    data.cmd_id=PON_GET_PHY_READY_STATUS;
    assert(!q1000k_phy_call(&data) && !api_calls);
    struct xpon_phy_api_data_s fw_request={.api_type=XPON_PHY_API_TYPE_SET,.cmd_id=PON_SET_PHY_FW_READY};
    assert(q1000k_phy_call(&fw_request)==-EOPNOTSUPP && !api_calls);

    data.cmd_id=PON_GET_PHY_BIP_COUNTER; assert(q1000k_phy_call(&data)==-EOPNOTSUPP);
    assert(irq_fn(75,&device)==IRQ_NONE && !isrs);
    regs[(EN7581_XGPON_PHY_XG_PON_INT_STA&0x1ffff)/4]=EN7581_XGPON_PHY_RX_LOS_INT_EN;
    reenter=1; assert(irq_fn(75,&device)==IRQ_HANDLED && isrs==1);
    q1000k_phy_poll(); assert(qphy_poll_job.queued);
    qphy_poll_job.fn(&qphy_poll_job); assert(polls==1 && gpPhyPriv->event_poll_timer.armed);
    cancel_run=1; assert(!q1000k_phy_stop());
    assert(!qphy_active && !allocated_irq && !qphy_poll_job.queued && !gpPhyPriv->event_poll_timer.armed);
    assert(polls==1 && !fw_calls && !gpPhyPriv->is_irq_requested);
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
    initialized(); assert(!q1000k_phy_start()); controller_error=-EIO;
    assert(q1000k_phy_stop()==-EIO && !allocated_irq && !qphy_active);
    initialized(); assert(!q1000k_phy_start()); q1000k_phy_exit();
    assert(!gpPhyPriv && !allocated_irq && !allocs && !qphy_poll_job.queued);
    assert(q1000k_phy_start()==-ENODEV && q1000k_phy_call(&data)==-ENODEV);
    q1000k_phy_exit();
    return 0;
}
