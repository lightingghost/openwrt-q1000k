// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define pr_err_ratelimited(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
typedef uint8_t u8;
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define AIROHA_GDM2_IDX 2
#define EXPORT_SYMBOL_GPL(...)
static bool rtnl,irq;
static unsigned int writes,ticks,ignore_at,corrupt_at;
static int complete_at;
static unsigned int target;
static bool unstable,never_done,never_idle,never_empty;
struct airoha_qdma { u32 closed[8],status[8]; };
struct airoha_eth { struct airoha_qdma qdma[2]; u32 tx,rx,forward,command,busy,loop; };
struct airoha_gdm_dev { struct airoha_eth *eth; };
struct airoha_pon {
    void *netdev;
    struct airoha_gdm_dev *dma_dev;
    bool admission_lock,control_fault,paused,pause_ready;
    u32 tx_enabled,retiring,configuring,fe_retired;
    u8 closed[32];
    int pending;
};
static struct airoha_eth eth;
static struct airoha_gdm_dev dev={.eth=&eth};
static struct airoha_pon pon;
static void rtnl_lock(void) { assert(!rtnl && !irq); rtnl=true; }
static void rtnl_unlock(void) { assert(rtnl); rtnl=false; }
#define in_interrupt() irq
#define irqs_disabled() irq
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!*(l)); *(l)=true; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=false; } while(0)
#define rcu_access_pointer(p) (p)
#define atomic_read_acquire(p) (*(p))
static void check(void) { assert(rtnl && !pon.admission_lock && pon.configuring==U32_MAX); }
static u32 *fe_reg(u32 reg)
{
    switch(reg) {
    case 0x140c: return &eth.forward;
    case 0x151c: return &eth.loop;
    case 0x1520: return &eth.command;
    case 0x1524: return &eth.tx;
    case 0x1528: return &eth.rx;
    case 0x1570: return &eth.busy;
    default: assert(!"access outside GDM2/CDM2 registers"); return NULL;
    }
}
static u32 airoha_fe_rr(struct airoha_eth *e,u32 reg)
{
    check(); assert(e==&eth);
    if(corrupt_at && writes==corrupt_at) return U32_MAX;
    return *fe_reg(reg);
}
static void airoha_fe_wr(struct airoha_eth *e,u32 reg,u32 value)
{
    check(); assert(e==&eth && reg!=0x151c && reg!=0x1570); writes++;
    if(writes!=ignore_at) *fe_reg(reg)=reg==0x1528 ? value & 0xffff : value;
}
static u32 airoha_qdma_rr(struct airoha_qdma *q,u32 reg)
{
    check(); assert(q==&eth.qdma[1] && !(reg&3));
    if(corrupt_at && writes==corrupt_at) return 0;
    if(reg>=0xa0 && reg<=0xbc) return q->closed[(reg-0xa0)/4];
    assert(reg>=0x1280 && reg<=0x129c);
    return q->status[(reg-0x1280)/4];
}
static void airoha_qdma_wr(struct airoha_qdma *q,u32 reg,u32 value)
{
    check(); assert(q==&eth.qdma[1] && reg>=0xa0 && reg<=0xbc && !(reg&3));
    writes++; if(writes!=ignore_at) q->closed[(reg-0xa0)/4]=value;
}
static void usleep_range(unsigned int low,unsigned int high)
{
    check(); assert(!irq && low==1000 && high==1500);
    ticks++;
    if(complete_at>=0 && ticks>=(unsigned int)complete_at && !(unstable && ticks==2)) {
        if(!never_done && (eth.command&1)) eth.command|=2;
        if(!never_idle) eth.busy&=~BIT(target);
        if(!never_empty) eth.qdma[1].status[target/4]&=~(0xffU<<((target&3)*8));
    } else {
        eth.command&=~2U; eth.busy|=BIT(target);
        eth.qdma[1].status[target/4]|=0xffU<<((target&3)*8);
    }
}
/* PRODUCTION */
static void reset(unsigned int channel)
{
    memset(&eth,0,sizeof(eth)); memset(&pon,0,sizeof(pon));
    eth.tx=U32_MAX; eth.rx=0xefab; eth.forward=0x56789abc;
    eth.command=BIT(17)|BIT(26); eth.busy=0xf000;
    memset(&eth.qdma[0],0xa5,sizeof(eth.qdma[0]));
    memset(eth.qdma[1].closed,255,sizeof(eth.qdma[1].closed));
    for(unsigned int i=0;i<8;i++) eth.qdma[1].status[i]=0xaa5555aa;
    pon.netdev=&dev; pon.dma_dev=&dev; pon.tx_enabled=eth.tx;
    pon.paused=pon.pause_ready=true;
    memset(pon.closed,255,sizeof(pon.closed));
    rtnl=irq=false; writes=ticks=ignore_at=corrupt_at=0;
    unstable=never_done=never_idle=never_empty=false;
    target=channel; complete_at=3;
}
static void finished(void)
{
    const u8 *lan=(void *)&eth.qdma[0];
    assert(!rtnl && !pon.admission_lock && !pon.configuring);
    for(unsigned int i=0;i<sizeof(eth.qdma[0]);i++) assert(lan[i]==0xa5);
    for(unsigned int i=0;i<32;i++) assert(pon.closed[i]==255);
}
int main(void)
{
    unsigned int c,fail;
    for(c=0;c<32;c++) {
        reset(c);
        assert(!airoha_pon_retire_fe(&pon,c));
        assert(ticks==4 && writes==10 && !pon.control_fault);
        assert(pon.fe_retired==BIT(c));
        assert(eth.tx==(U32_MAX&~BIT(c)) && pon.tx_enabled==eth.tx);
        assert(eth.rx==(0xefab&~BIT(c)) && eth.forward==(0x56789abc&~BIT(c)));
        assert(eth.command==(BIT(17)|BIT(26)) && (pon.retiring&BIT(c)));
        for(unsigned int i=0;i<8;i++) assert(eth.qdma[1].closed[i]==U32_MAX);
        finished();
    }
    reset(31); complete_at=1; unstable=true;
    assert(!airoha_pon_retire_fe(&pon,31) && ticks==4); finished();
    for(fail=1;fail<=10;fail++) {
        reset(31); ignore_at=fail;
        assert(airoha_pon_retire_fe(&pon,31)<0 && pon.control_fault);
        /* Independent containment retries close the port after failed restore. */
        assert(!eth.tx && !eth.rx && !eth.forward); finished();
        reset(31); corrupt_at=fail;
        assert(airoha_pon_retire_fe(&pon,31)<0 && pon.control_fault); finished();
    }
    for(fail=0;fail<4;fail++) {
        reset(7);
        if(fail==0) complete_at=-1;
        if(fail==1) never_done=true;
        if(fail==2) never_idle=true;
        if(fail==3) never_empty=true;
        assert(airoha_pon_retire_fe(&pon,7)==-ETIMEDOUT && ticks==12);
        assert(pon.control_fault && !eth.tx && !eth.rx && !eth.forward); finished();
    }
    reset(1); pon.netdev=NULL;
    assert(airoha_pon_retire_fe(&pon,1)==-ENODEV && !writes); finished();
    reset(1); pon.pending=1;
    assert(airoha_pon_retire_fe(&pon,1)==-EAGAIN && !writes); finished();
    reset(1); pon.closed[17]=0;
    assert(airoha_pon_retire_fe(&pon,1)==-EBUSY && !writes);
    reset(1); irq=true;
    assert(airoha_pon_retire_fe(&pon,1)==-EWOULDBLOCK && !writes); irq=false; finished();
    reset(1); pon.control_fault=true;
    assert(airoha_pon_retire_fe(&pon,1)==-EIO && !writes); finished();
    reset(1); eth.command|=BIT(19);
    assert(airoha_pon_retire_fe(&pon,1)==-EBUSY && !writes && !pon.control_fault); finished();
    reset(1); eth.command=U32_MAX;
    assert(airoha_pon_retire_fe(&pon,1)==-EIO && !writes && pon.control_fault); finished();
    reset(1); eth.tx^=BIT(9);
    assert(airoha_pon_retire_fe(&pon,1)==-EIO && !writes && pon.control_fault); finished();
    reset(1); eth.qdma[1].closed[3]=0;
    assert(airoha_pon_retire_fe(&pon,1)==-EIO && !writes && pon.control_fault); finished();
    assert(airoha_pon_retire_fe(NULL,0)==-EINVAL);
    assert(airoha_pon_retire_fe(&pon,32)==-EINVAL);
    return 0;
}
