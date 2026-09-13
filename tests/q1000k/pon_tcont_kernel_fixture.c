// SPDX-License-Identifier: GPL-2.0-only
/* Real Linux spinlocks/IRQs/kthreads; only the two MAC registers are fixtures. */
#include <linux/module.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/utsname.h>
#ifndef CONFIG_UML
#error This test runs only in a disposable UML guest.
#endif
#define GPON_UNASSIGN_ONU_ID 0x3ff
#define TCONT_ID_CFG 0x5250
#define TCONT_ID_STS 0x5254
static void fixture_write(unsigned int reg,u32 value);
static u32 fixture_read(unsigned int reg);
#define IO_SREG(reg,value) fixture_write(reg,value)
#define IO_GREG(reg) fixture_read(reg)

/* PRODUCTION */

static struct { bool valid; u16 id; } table[32];
static u32 command;
static unsigned int commands, writes, fail_command;
static bool ignore_write;
static void fixture_write(unsigned int reg,u32 value)
{
    unsigned int channel=(value>>20)&31;

    lockdep_assert_held(&q1000k_tcont_lock);
    WARN_ON(!irqs_disabled() || reg!=0x5250 || (value & ~0x81f13fffU));
    command=value; commands++;
    if(value & 0x80000000U) {
        WARN_ON(!channel);
        writes++;
        if(!ignore_write) {
            table[channel].valid=!!(value&0x10000);
            table[channel].id=value&0x3fff;
        }
    }
}
static u32 fixture_read(unsigned int reg)
{
    unsigned int channel=(command>>20)&31;

    lockdep_assert_held(&q1000k_tcont_lock);
    WARN_ON(!irqs_disabled() || reg!=0x5254);
    if(commands==fail_command) return 0;
    return 0x80000000U | (table[channel].valid ? 0x10000U : 0) | table[channel].id;
}
/* Independent test cases model a hardware reset plus a fresh module instance.
 * This is deliberately not a production software recovery interface.
 */
static void reset_model(void)
{
    unsigned long flags;
    spin_lock_irqsave(&q1000k_tcont_lock,flags);
    memset(table,0,sizeof(table));
    q1000k_tcont_fault=false; q1000k_tcont_quarantined=0;
    commands=writes=fail_command=0; ignore_write=false;
    spin_unlock_irqrestore(&q1000k_tcont_lock,flags);
}
#define CHECK(condition) do { if(!(condition)) { \
    pr_err("Q1000K_PON_TCONT_KERNEL_FAIL line=%d: %s\n",__LINE__,#condition); \
    return -EINVAL; \
} } while(0)
struct request {
    u16 id;
    int result;
    struct completion *start;
    struct completion done;
};
static int allocate_thread(void *arg)
{
    struct request *request=arg;
    wait_for_completion(request->start);
    request->result=q1000k_tcont_enable(request->id,10);
    complete(&request->done);
    /* Keep the task alive until its owner calls kthread_stop(). */
    while(!kthread_should_stop())
        msleep_interruptible(1);
    return 0;
}
static int allocation_race(bool different)
{
    struct task_struct *tasks[32]={0};
    struct request requests[32];
    DECLARE_COMPLETION_ONSTACK(start);
    unsigned int i,good=0,bad=0;
    int ret=0;

    reset_model();
    for(i=0;i<32;i++) {
        requests[i]=(struct request){.id=1000+(different ? i : 0),.start=&start};
        init_completion(&requests[i].done);
        tasks[i]=kthread_run(allocate_thread,&requests[i],"pon-tcont-%u",i);
        if(IS_ERR(tasks[i])) {
            ret=PTR_ERR(tasks[i]); tasks[i]=NULL; break;
        }
    }
    complete_all(&start);
    for(i=0;i<32;i++) if(tasks[i]) {
        if(!wait_for_completion_timeout(&requests[i].done,msecs_to_jiffies(2000)) && !ret)
            ret=-ETIMEDOUT;
        kthread_stop(tasks[i]);
    }
    if(ret) return ret;
    for(i=0;i<32;i++) {
        int result=requests[i].result;
        if(result>0) {
            good++;
            CHECK(result<32 && table[result].valid && table[result].id==requests[i].id);
        } else {
            bad++;
            CHECK(result==(different ? -ENOSPC : -EEXIST));
        }
    }
    CHECK(good==(different ? 31 : 1) && bad==(different ? 1 : 31) && writes==good);
    for(i=1;i<32;i++) if(table[i].valid) {
        CHECK(q1000k_tcont_disable(table[i].id,10)==i);
        CHECK(!table[i].valid && (q1000k_tcont_quarantined & BIT(i)));
    }
    if(different) CHECK(q1000k_tcont_enable(3000,10)==-ENOSPC);
    return 0;
}
static int __init pon_tcont_test_init(void)
{
    unsigned int i,before;
    u16 id;
    u8 channel;
    bool valid;

    if(!strstr(init_utsname()->release,"-q1000k-pon-tcont-test")) return -EPERM;
    for(i=1;i<=33;i++) {
        reset_model(); fail_command=i;
        CHECK(q1000k_tcont_enable(100,10)==-ETIMEDOUT && commands==i);
        before=commands; valid=true; id=0xaaaa; channel=0xa5;
        CHECK(q1000k_tcont_read(1,&valid,&id)==-EIO && valid && id==0xaaaa);
        CHECK(q1000k_tcont_find(100,10,&channel)==-EIO && channel==0xa5);
        CHECK(q1000k_tcont_enable(101,10)==-EIO && commands==before);
        cond_resched();
    }
    reset_model(); ignore_write=true;
    CHECK(q1000k_tcont_enable(100,10)==-EIO && q1000k_tcont_fault);
    reset_model();
    table[1].id=100; table[31].id=100; table[31].valid=true;
    CHECK(!q1000k_tcont_find(100,10,&channel) && channel==31);
    CHECK(q1000k_tcont_enable(100,10)==-EEXIST && !writes);
    CHECK(q1000k_tcont_disable(100,10)==31 && !table[31].valid && !table[1].valid);
    CHECK(q1000k_tcont_disable(10,10)==-EOPNOTSUPP);
    CHECK(!allocation_race(false));
    CHECK(!allocation_race(true));
    pr_info("Q1000K_PON_TCONT_KERNEL_PASS timeout_points=33 racing_callers=64 verification=pass quarantine=pass\n");
    return 0;
}
static void __exit pon_tcont_test_exit(void) {}
module_init(pon_tcont_test_init);
module_exit(pon_tcont_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K T-CONT commands and concurrency test");
