// SPDX-License-Identifier: GPL-2.0-only
/* No physical device: model MAC registers and native drain, retain Linux locks. */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/irq_work.h>
#include <linux/utsname.h>
#ifndef CONFIG_UML
#error This test runs only in a disposable UML guest.
#endif
typedef u16 ushort;
typedef u8 unchar;
/* MODEL */
#define GEM_PORT_CFG 0x5274
#define GEM_PORT_STS 0x5278
static void fixture_write(unsigned int reg,u32 value);
static u32 fixture_read(unsigned int reg);
#define IO_SREG(reg,value) fixture_write(reg,value)
#define IO_GREG(reg) fixture_read(reg)
static void q1000k_tcont_quarantine(unsigned int channel);
static int q1000k_transport_quiesce_channel(u8 channel);
/* PRODUCTION */
static u8 table[65536];
static u32 command,quarantined,closed;
static unsigned int commands,writes,fail_command;
static bool ignore_write;
static void fixture_write(unsigned int reg,u32 value)
{
    lockdep_assert_held(&q1000k_gem_lock);
    lockdep_assert_not_held(&q1000k_gwan_lock);
    WARN_ON(!irqs_disabled() || reg!=0x5274 || (value & ~0x8007ffffU) || (value&0xffff)==0xffff);
    command=value; commands++;
    if(value & BIT(31)) {
        writes++;
        if(!ignore_write) table[value&0xffff]=(value>>16)&7;
    }
}
static u32 fixture_read(unsigned int reg)
{
    lockdep_assert_held(&q1000k_gem_lock);
    WARN_ON(!irqs_disabled() || reg!=0x5278);
    if(commands==fail_command) return 0;
    return BIT(31) | table[command&0xffff];
}
static void q1000k_tcont_quarantine(unsigned int channel)
{
    lockdep_assert_not_held(&q1000k_gwan_lock);
    WARN_ON(!channel || channel>=32); quarantined |= BIT(channel);
}
static int q1000k_transport_quiesce_channel(u8 channel)
{
    struct q1000k_gwan_binding b;
    lockdep_assert_not_held(&q1000k_gwan_lock);
    WARN_ON(atomic_read(&q1000k_tcont_config_busy)!=1 || !(quarantined&BIT(channel)));
    WARN_ON(gwan_create_new_gemport(4000,33,0,200)!=-EBUSY);
    WARN_ON(q1000k_gwan_binding(1000,true,&b)==0);
    closed |= BIT(channel); return 0;
}
#define CHECK(condition) do { if(!(condition)) { \
    pr_err("Q1000K_PON_GEM_KERNEL_FAIL line=%d: %s\n",__LINE__,#condition); \
    return -EINVAL; \
} } while(0)
/* Reset modeled hardware and globals only between independent test cases. */
static void reset_model(void)
{
    memset(&wan,0,sizeof(wan)); memset(table,0,sizeof(table));
    memset(wan.gpon.allocId,0xff,sizeof(wan.gpon.allocId));
    for(unsigned int i=0;i<65536;i++) wan.gpon.gemIdToIndex[i]=0x7fff;
    memset(q1000k_gwan_retiring_gems,0,sizeof(q1000k_gwan_retiring_gems));
    q1000k_gem_fault=false; q1000k_gwan_retiring_channels=quarantined=closed=0;
    atomic_set(&q1000k_tcont_config_busy,0);
    commands=writes=fail_command=0; ignore_write=false;
}
struct request { unsigned int lane; int result; bool reader; struct completion *start; struct completion done; };
static atomic_t readers_stop, snapshots;
static int run_thread(void *arg)
{
    struct request *r=arg;
    wait_for_completion(r->start);
    if(r->reader) {
        do {
            for(unsigned int gem=1000;gem<1256;gem++) {
                struct q1000k_gwan_binding b;
                int ret=q1000k_gwan_binding(gem,true,&b);
                if(ret && ret!=-ENOENT && ret!=-ENODATA && ret!=-ESHUTDOWN) r->result=-EINVAL;
                if(!ret) {
                    if(b.gem!=gem || b.alloc_id!=200 || b.channel!=4 || b.ani!=gem-1000) r->result=-EINVAL;
                    q1000k_gwan_account(gem,true,60); atomic_inc(&snapshots);
                }
            }
            cond_resched();
        } while(!atomic_read(&readers_stop) && !kthread_should_stop());
    } else {
        for(unsigned int gem=1000+r->lane;gem<1256;gem+=4) {
            int ret;
            do { ret=gwan_create_new_gemport(gem,33,0,200); cond_resched(); } while(ret==-EBUSY && !kthread_should_stop());
            if(ret) { r->result=ret; break; }
            do { ret=gwan_config_gemport(gem,ENUM_CFG_NETIDX,gem-1000); cond_resched(); } while(ret==-EBUSY && !kthread_should_stop());
            if(ret) { r->result=ret; break; }
        }
    }
    complete(&r->done);
    while(!kthread_should_stop()) msleep_interruptible(1);
    return 0;
}
static int registry_race(void)
{
    struct task_struct *tasks[8]={0};
    struct request requests[8];
    DECLARE_COMPLETION_ONSTACK(start);
    unsigned int i;
    int ret=0;
    reset_model(); q1000k_gwan_publish_tcont(4,200);
    atomic_set(&readers_stop,0); atomic_set(&snapshots,0);
    for(i=0;i<8;i++) {
        requests[i]=(struct request){.lane=i%4,.reader=i<4,.start=&start};
        init_completion(&requests[i].done);
        tasks[i]=kthread_run(run_thread,&requests[i],"pon-gem-%u",i);
        if(IS_ERR(tasks[i])) { ret=PTR_ERR(tasks[i]); tasks[i]=NULL; break; }
    }
    complete_all(&start);
    for(i=4;i<8;i++) if(tasks[i]) {
        if(!wait_for_completion_timeout(&requests[i].done,msecs_to_jiffies(5000))) ret=-ETIMEDOUT;
        kthread_stop(tasks[i]);
        if(!ret) ret=requests[i].result;
    }
    if(!ret && gwan_remove_all_gemport()!=-EOPNOTSUPP) ret=-EINVAL;
    atomic_set(&readers_stop,1);
    for(i=0;i<4;i++) if(tasks[i]) {
        if(!wait_for_completion_timeout(&requests[i].done,msecs_to_jiffies(2000))) ret=-ETIMEDOUT;
        kthread_stop(tasks[i]);
        if(!ret) ret=requests[i].result;
    }
    if(ret) return ret;
    CHECK(wan.gpon.gemNumbers==256 && writes==256 && closed==BIT(4) && atomic_read(&snapshots));
    CHECK(gwan_create_new_gemport(3000,33,0,201)==-ENOSPC);
    return 0;
}
static int irq_result;
static void irq_control(struct irq_work *work)
{
    struct q1000k_gwan_binding b;
    WARN_ON(!in_hardirq());
    irq_result=gwan_create_new_gemport(65534,4,0,200);
    if(!irq_result) irq_result=gwan_config_gemport(65534,ENUM_CFG_NETIDX,255);
    if(!irq_result) irq_result=q1000k_gwan_binding(65534,true,&b);
    if(!irq_result && (b.gem!=65534 || b.channel!=4 || b.ani!=255)) irq_result=-EINVAL;
    if(!irq_result && gwan_remove_gemport(65534)!=-EOPNOTSUPP) irq_result=-EINVAL;
}
static int __init pon_gem_test_init(void)
{
    const struct q1000k_gem_value empty={0},value={.valid=1};
    struct q1000k_gem_value output;
    DEFINE_IRQ_WORK(work,irq_control);
    unsigned int i,before;
    if(!strstr(init_utsname()->release,"-q1000k-pon-gem-test")) return -EPERM;
    for(i=1;i<=3;i++) {
        reset_model(); fail_command=i;
        CHECK(q1000k_gem_replace(65534,&empty,&value)==-ETIMEDOUT);
        before=commands; output=(struct q1000k_gem_value){9,8,7};
        CHECK(q1000k_gem_read(65534,&output)==-EIO && output.valid==9 && commands==before);
        cond_resched();
    }
    reset_model(); ignore_write=true;
    CHECK(q1000k_gem_replace(65534,&empty,&value)==-EIO && q1000k_gem_faulted());
    reset_model(); q1000k_gwan_publish_tcont(4,200);
    irq_result=-EINPROGRESS; irq_work_queue(&work); irq_work_sync(&work);
    CHECK(!irq_result && closed==BIT(4) && wan.gpon.gemNumbers==1);
    CHECK(!registry_race());
    pr_info("Q1000K_PON_GEM_KERNEL_PASS timeout_points=3 slots=256 hardirq=pass snapshots=%d\n",atomic_read(&snapshots));
    return 0;
}
static void __exit pon_gem_test_exit(void) {}
module_init(pon_gem_test_init);
module_exit(pon_gem_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K GEM commands and binding transactions");
