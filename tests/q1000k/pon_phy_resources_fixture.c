// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
typedef uint32_t u32;
#include <an7581_pon_phy.h>
#define __iomem
#define EXPORT_SYMBOL(x)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define DEFINE_RWLOCK(x) pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER
#define DEFINE_MUTEX(x) pthread_mutex_t x = PTHREAD_MUTEX_INITIALIZER
#define IS_ENABLED(x) 0
#define pr_err_ratelimited(...) ((void)0)
static _Thread_local int held;
static unsigned int reads, writes;
static bool lifecycle_locked;
static int atomic_context, reset_step, reset_fail, wrong_status;
static void mutex_lock(pthread_mutex_t *m) { assert(!held); assert(!pthread_mutex_lock(m)); lifecycle_locked=true; }
static void mutex_unlock(pthread_mutex_t *m) { assert(!held && lifecycle_locked); lifecycle_locked=false; assert(!pthread_mutex_unlock(m)); }
static int in_atomic(void) { return atomic_context; }
static int in_interrupt(void) { return 0; }
static int irqs_disabled(void) { return 0; }
static int rcu_preempt_depth(void) { return 0; }
static int rcu_read_lock_held(void) { return 1; }
struct reset_control { int asserted; };
static int reset_control_assert(struct reset_control *r);
static int reset_control_deassert(struct reset_control *r);
static int reset_control_status(struct reset_control *r);
static void udelay(unsigned int n) { assert(n==1 && !held && lifecycle_locked); }
#define read_lock_irqsave(l,f) do { assert(!held); assert(!pthread_mutex_lock(l)); held=1; (f)=0; } while (0)
#define read_unlock_irqrestore(l,f) do { (void)(f); assert(held); held=0; assert(!pthread_mutex_unlock(l)); } while (0)
#define write_lock_irqsave read_lock_irqsave
#define write_unlock_irqrestore read_unlock_irqrestore
struct device { int id; };
static u32 memory[3][0x800];
static void address(void *p)
{
    unsigned int b;
    assert(held);
    for (b=0;b<3;b++) {
        uintptr_t off=(uintptr_t)p-(uintptr_t)memory[b];
        if (!(off&3) && off<=(b ? 0x1000 : 0x1fff)-4) return;
    }
    assert(!"unowned MMIO");
}
static u32 readl(void *p) { address(p); reads++; return *(u32 *)p; }
static void writel(u32 v, void *p) { address(p); writes++; *(u32 *)p=v; }
/* PRODUCTION */
static void resetting(void)
{
    u32 value=123;
    assert(!held && lifecycle_locked && pon_phy->resetting);
    assert(an7581_pon_phy_read(0x1faf0000,&value)==-EBUSY && value==123);
    assert(an7581_pon_phy_write(0x1faf0000,0)==-EBUSY);
    assert(an7581_pon_phy_status()==-EBUSY);
}
static int reset_control_assert(struct reset_control *r)
{
    resetting(); reset_step++;
    if(reset_step==reset_fail) return -ETIMEDOUT;
    r->asserted=1; return 0;
}
static int reset_control_deassert(struct reset_control *r)
{
    resetting(); reset_step++;
    if(reset_step==reset_fail) return -ETIMEDOUT;
    r->asserted=0; return 0;
}
static int reset_control_status(struct reset_control *r)
{
    resetting(); reset_step++;
    if(reset_step==reset_fail) return -ETIMEDOUT;
    return reset_step==wrong_status ? !r->asserted : r->asserted;
}
static void *worker(void *arg)
{
    u32 b=(uintptr_t)arg;
    unsigned int n;
    for (n=0;n<20000;n++) {
        assert(!an7581_pon_phy_update(0x1fa8b004,b,b,0));
        assert(!an7581_pon_phy_update(0x1fa8b004,b,b,1));
    }
    return NULL;
}
int main(void)
{
    struct device dev={1};
    struct reset_control reset={0};
    struct an7581_pon_phy provider={.dev=&dev,.irq=75,.reset=&reset};
    u32 b, off, v=123, before, start, end, mask, reg;
    pthread_t threads[4];
    assert(!get_pon_phy_dev() && get_pon_phy_irq()==-ENODEV);
    assert(an7581_pon_phy_status()==-ENODEV);
    assert(an7581_pon_phy_reset()==-ENODEV && !reset_step);
    assert(an7581_pon_phy_read(0x1faf0000,&v)==-ENODEV && v==123);
    assert(an7581_pon_phy_write(0x1faf0000,0)==-ENODEV);
    assert(!reads && !writes);
    for(b=0;b<3;b++) provider.base[b]=memory[b];
    pon_phy=&provider;
    assert(get_pon_phy_dev()==&dev && get_pon_phy_irq()==75);
    for(b=0;b<3;b++) for(off=0;off<0x2100;off++) {
        bool valid=false;
        unsigned int bank;
        before=writes; reg=phy_address[b]+off;
        for(bank=0;bank<3;bank++)
            if (!(reg&3) && reg>=phy_address[bank] &&
                reg-phy_address[bank]<=phy_size[bank]-4) valid=true;
        assert(an7581_pon_phy_write(reg,reg)==(valid ? 0 : -EINVAL));
        assert(writes-before==valid);
        assert(an7581_pon_phy_read(reg,&v)==(valid ? 0 : -EINVAL));
        if(valid) assert(v==reg);
        assert(an7581_pon_phy_read(reg|0xa0000000,&v)==(valid ? 0 : -EINVAL));
    }
    before=writes;
    assert(an7581_pon_phy_write(0x1fa7a000,0)==-EINVAL);
    assert(an7581_pon_phy_write(0x1fa7b000,0)==-EINVAL);
    assert(an7581_pon_phy_write(0x1fb00830,0)==-EINVAL);
    assert(an7581_pon_phy_write(0x1fa2ff24,0)==-EINVAL);
    assert(an7581_pon_phy_write(0x1faf3000,0)==-EINVAL);
    assert(an7581_pon_phy_write(0xdfaf0000,0)==-EINVAL);
    assert(an7581_pon_phy_write(0xffaf0000,0)==-EINVAL);
    assert(an7581_pon_phy_read(0x1faf0000,NULL)==-EINVAL);
    assert(writes==before);
    for(start=0;start<32;start++) for(end=start;end<32;end++) {
        mask=(~0U>>(31-end)) & (~0U<<start);
        memory[0][0]=0xa5a5a5a5;
        assert(!an7581_pon_phy_update(0x1faf0000,end,start,mask>>start));
        assert(memory[0][0]==(0xa5a5a5a5|mask));
        assert(!an7581_pon_phy_update(0x1faf0000,end,start,0));
        assert(memory[0][0]==(0xa5a5a5a5&~mask));
    }
    before=reads;
    assert(!an7581_pon_phy_update(0x1faf0000,31,0,~0U));
    assert(reads==before); /* Full-word writes never consume a status read. */
    before=writes;
    assert(an7581_pon_phy_update(0x1faf0000,32,0,0)==-EINVAL);
    assert(an7581_pon_phy_update(0x1faf0000,0,1,0)==-EINVAL);
    assert(an7581_pon_phy_update(0x1faf0000,30,0,1U<<31)==-ERANGE);
    assert(writes==before && an7581_pon_phy_status()==-EINVAL);
    provider.fault=0;
    assert(get_pon_phy_data(0x1fa7a000)==~0U);
    assert(an7581_pon_phy_status()==-EINVAL);
    provider.fault=0;
    set_pon_phy_data(0x1fa7b000,0);
    assert(an7581_pon_phy_status()==-EINVAL);
    provider.fault=0;
    memory[2][1]=0x40000000;
    for(b=0;b<4;b++) assert(!pthread_create(&threads[b],NULL,worker,(void *)(uintptr_t)b));
    for(b=0;b<4;b++) assert(!pthread_join(threads[b],NULL));
    assert(memory[2][1]==0x4000000f && !an7581_pon_phy_status());
    atomic_context=1;
    assert(an7581_pon_phy_reset()==-EWOULDBLOCK && !reset_step);
    atomic_context=0;
    assert(!an7581_pon_phy_reset() && reset_step==4 && !reset.asserted);
    for(b=1;b<=4;b++) {
        reset_step=0; reset_fail=b; provider.fault=0;
        assert(an7581_pon_phy_reset()==-ETIMEDOUT && !provider.resetting);
        assert(provider.fault==-ETIMEDOUT && reset.asserted);
        before=reset_step;
        assert(an7581_pon_phy_reset()==-ETIMEDOUT && reset_step==(int)before);
    }
    reset_fail=0;
    for(b=2;b<=4;b+=2) {
        provider.fault=0; reset_step=0; wrong_status=b;
        assert(an7581_pon_phy_reset()==-EIO && provider.fault==-EIO && reset.asserted);
    }
    pon_phy=NULL; before=writes;
    set_pon_phy_data(0x1faf0000,0);
    assert(writes==before && get_pon_phy_irq()==-ENODEV);
    return 0;
}
