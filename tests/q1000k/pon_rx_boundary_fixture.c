// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define AIROHA_GDM2_IDX 2
#define REG_GDM_TXCHN_EN(n) 0
#define REG_GDM_RXCHN_EN(n) 1
#define REG_CDM_HWFWD_CHN(n) 2
#define REG_GDM_RETIRE_STS(n) 3
#define REG_GDM_CHN_BUSY(n) 4
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define rcu_access_pointer(p) (p)
#define atomic_read_acquire(p) (*(p))
static int irq, rtnl, graces, ticks, drain_calls, drain_error, shared, unstable, fail_after;
#define in_interrupt() irq
#define irqs_disabled() irq
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(*(l)); *(l)=0; (void)(f); } while(0)
struct airoha_qdma { int n; };
struct airoha_eth { struct airoha_qdma qdma[2]; u32 regs[5]; };
struct airoha_gdm_dev { bool pon_flow_fault; struct airoha_eth *eth; };
struct airoha_pon {
    void *netdev;
    struct airoha_gdm_dev *dma_dev;
    int admission_lock,pending;
    bool control_fault,paused,pause_ready,rx_closed,rx_drained;
    u32 configuring,fe_retired;
};
static struct airoha_eth eth;
static struct airoha_gdm_dev dev={.eth=&eth};
static struct airoha_pon pon;
static void rtnl_lock(void) { assert(!rtnl && !irq); rtnl=1; }
static void rtnl_unlock(void) { assert(rtnl && !pon.admission_lock); rtnl=0; }
static void synchronize_net(void) { assert(rtnl && !pon.admission_lock && pon.rx_closed); graces++; }
static int airoha_pon_qdma_shared(struct airoha_gdm_dev *d) { assert(d==&dev); return shared; }
static u32 airoha_fe_rr(struct airoha_eth *e,int reg)
{
    assert(e==&eth && rtnl && !pon.admission_lock && reg>=0 && reg<5);
    if(reg==3 && unstable && ticks==1) return 1;
    if(reg==3 && fail_after && ticks>=fail_after) return ~0U;
    return e->regs[reg];
}
static void usleep_range(int lo,int hi) { assert(lo==1000 && hi==1500 && !pon.admission_lock); ticks++; }
static int airoha_qdma_pon_drain_rx(struct airoha_qdma *q)
{
    assert(q==&eth.qdma[1] && rtnl && !pon.admission_lock && pon.rx_closed && pon.configuring==U32_MAX);
    drain_calls++; return drain_error;
}
/* Full PPE retirement/fault containment is exercised by test_pon_transport. */
static void airoha_pon_fault_locked(struct airoha_pon *p) { p->control_fault=true; }
/* PRODUCTION */
static void reset(void)
{
    memset(&pon,0,sizeof(pon)); memset(&eth,0,sizeof(eth));
    pon.netdev=(void *)1; pon.dma_dev=&dev; pon.fe_retired=~0U; pon.paused=pon.pause_ready=true;
    irq=rtnl=graces=ticks=drain_calls=drain_error=shared=unstable=fail_after=0;
}
int main(void)
{
    reset(); assert(!airoha_pon_drain_rx(&pon));
    assert(pon.rx_closed && pon.rx_drained && drain_calls==1 && ticks==1 && graces==1 && !pon.configuring);
    assert(!airoha_pon_drain_rx(&pon) && drain_calls==1);
    reset(); unstable=1;
    assert(!airoha_pon_drain_rx(&pon) && ticks==3);
    for(int i=0;i<32;i++) {
        reset(); pon.fe_retired&=~(1U<<i);
        assert(airoha_pon_drain_rx(&pon)==-EBUSY && !pon.rx_closed && !drain_calls && !graces);
    }
    for(int i=0;i<5;i++) {
        reset(); eth.regs[i]=1;
        assert(airoha_pon_drain_rx(&pon)==(i<3 ? -EIO : -ETIMEDOUT));
        assert(!drain_calls && pon.rx_closed && !pon.rx_drained && pon.control_fault && !pon.configuring);
    }
    reset(); shared=1;
    assert(airoha_pon_drain_rx(&pon)==-EIO && !drain_calls);
    reset(); drain_error=-ETIMEDOUT;
    assert(airoha_pon_drain_rx(&pon)==-ETIMEDOUT && pon.control_fault && !pon.rx_drained);
    reset(); fail_after=1;
    assert(airoha_pon_drain_rx(&pon)==-EIO && !drain_calls);
    reset(); pon.netdev=NULL;
    assert(airoha_pon_drain_rx(&pon)==-ENODEV && !graces);
    reset(); irq=1;
    assert(airoha_pon_drain_rx(&pon)==-EWOULDBLOCK && !graces); irq=0;
    reset(); pon.pending=1;
    assert(airoha_pon_drain_rx(&pon)==-EBUSY && !graces);
    reset(); pon.paused=false;
    assert(airoha_pon_drain_rx(&pon)==-EBUSY && !graces);
    assert(airoha_pon_drain_rx(NULL)==-EINVAL);
    return 0;
}
