// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define AIROHA_GDM2_IDX 2
#define GLOBAL_CFG_RX_DMA_EN_MASK 4
#define GLOBAL_CFG_RX_DMA_BUSY_MASK 8
#define GLOBAL_CFG_TX_DMA_BUSY_MASK 2
#define REG_QDMA_GLOBAL_CFG 4
#define REG_QUEUE_CLOSE_CFG(n) (0xa0+((n)/4)*4)
#define REG_GDM_TXCHN_EN(n) 0
#define REG_GDM_RXCHN_EN(n) 1
#define GDM_RXCHN_EN_MASK 0xffff
#define REG_CDM_HWFWD_CHN(n) 2
#define rcu_access_pointer(p) (p)
#define atomic_read_acquire(p) (*(p))
#define WRITE_ONCE(x,v) ((x)=(v))
#define smp_store_release(p,v) (*(p)=(v))
#define EXPORT_SYMBOL_GPL(n)
static int irq,rtnl, writes, fail_write, changed_other;
#define in_interrupt() irq
#define irqs_disabled() irq
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(*(l)); *(l)=0; (void)(f); } while(0)
struct airoha_qdma { u32 global,closed[8]; };
struct airoha_eth { struct airoha_qdma qdma[2]; u32 fe[3]; };
struct airoha_gdm_dev { struct airoha_eth *eth; u64 pon_generation; };
struct airoha_pon {
    void *netdev;
    struct airoha_gdm_dev *dma_dev;
    int admission_lock,pending;
    bool control_fault,paused,pause_ready,rx_closed,rx_drained,epoch_ready;
    u32 configuring,fe_retired,retiring,tx_enabled;
    u64 epoch[32],generation;
    unsigned char closed[32],paused_closed[32];
};
static struct airoha_eth eth;
static struct airoha_gdm_dev dev={.eth=&eth};
static struct airoha_pon pon;
static void rtnl_lock(void) { assert(!rtnl && !irq); rtnl=1; }
static void rtnl_unlock(void) { assert(rtnl && !pon.admission_lock); rtnl=0; }
static void check(void) { assert(rtnl && pon.admission_lock && pon.rx_closed && pon.paused); }
static u32 airoha_qdma_rr(struct airoha_qdma *q,u32 reg)
{
    check(); assert(q==&eth.qdma[1]);
    if(reg==4) return q->global;
    assert(reg>=0xa0 && reg<=0xbc && !(reg&3)); return q->closed[(reg-0xa0)/4];
}
static void airoha_qdma_set(struct airoha_qdma *q,u32 reg,u32 mask)
{
    check(); assert(q==&eth.qdma[1] && reg==4 && mask==4);
    if(++writes!=fail_write) q->global|=mask;
    if(changed_other) q->global^=0x1000;
}
static void airoha_qdma_clear(struct airoha_qdma *q,u32 reg,u32 mask)
{
    check(); assert(q==&eth.qdma[1] && reg==4 && mask==4);
    if(++writes!=fail_write) q->global&=~mask;
}
static u32 airoha_fe_rr(struct airoha_eth *e,unsigned int reg)
{
    check(); assert(e==&eth && reg<3); return e->fe[reg];
}
static int airoha_pon_fe_write_checked(struct airoha_eth *e,unsigned int reg,u32 value)
{
    check(); assert(e==&eth && reg==1);
    if(++writes==fail_write) return -EIO;
    e->fe[reg]=value & 0xffff; return e->fe[reg]==value ? 0 : -EIO;
}
/* PRODUCTION */
static void reset(void)
{
    memset(&pon,0,sizeof(pon)); memset(&eth,0,sizeof(eth));
    memset(&eth.qdma[0],0xa5,sizeof(eth.qdma[0]));
    memset(eth.qdma[1].closed,0xff,sizeof(eth.qdma[1].closed));
    eth.qdma[1].global=0xa5a50061;
    pon.netdev=(void *)1; pon.dma_dev=&dev;
    pon.fe_retired=pon.retiring=~0U;
    pon.paused=pon.pause_ready=pon.rx_closed=pon.rx_drained=true;
    memset(pon.closed,0xff,sizeof(pon.closed)); memset(pon.paused_closed,0xff,sizeof(pon.paused_closed));
    for(int i=0;i<32;i++) pon.epoch[i]=100+i;
    pon.generation=dev.pon_generation=50;
    irq=rtnl=writes=fail_write=changed_other=0;
}
static void finish(void)
{
    assert(!rtnl && !pon.admission_lock);
    for(unsigned int i=0;i<sizeof(eth.qdma[0]);i++) assert(((unsigned char *)&eth.qdma[0])[i]==0xa5);
}
int main(void)
{
    reset(); assert(!airoha_pon_reset_epoch(&pon));
    assert(pon.generation==51 && dev.pon_generation==51 && !pon.retiring && !pon.fe_retired && !pon.tx_enabled);
    assert(pon.epoch_ready && pon.rx_closed && pon.rx_drained && pon.paused && !writes);
    for(int i=0;i<32;i++) assert(pon.epoch[i]==(u64)(101+i) && pon.closed[i]==255 && pon.paused_closed[i]==255);
    assert(airoha_pon_reset_epoch(&pon)==-EBUSY);
    assert(airoha_pon_activate_rx(&pon,1)==-EINVAL && !writes);
    pon.tx_enabled=0x81;
    assert(!airoha_pon_activate_rx(&pon,0x81));
    assert(!pon.rx_closed && !pon.rx_drained && !pon.epoch_ready && pon.paused);
    assert(eth.fe[1]==0xffff && eth.qdma[1].global==0xa5a50065 && writes==2); finish();
    reset(); assert(!airoha_pon_reset_epoch(&pon)); pon.tx_enabled=1U<<31;
    assert(!airoha_pon_activate_rx(&pon,1U<<31));
    assert(eth.fe[1]==0xffff && pon.tx_enabled==(1U<<31)); finish();
    for(int i=0;i<32;i++) {
        reset(); pon.fe_retired&=~(1U<<i);
        assert(airoha_pon_reset_epoch(&pon)==-EBUSY && pon.generation==50 && !writes); finish();
        reset(); pon.epoch[i]=~0ULL;
        assert(airoha_pon_reset_epoch(&pon)==-EOVERFLOW && pon.control_fault && pon.generation==50); finish();
    }
    reset(); dev.pon_generation=~0ULL;
    assert(airoha_pon_reset_epoch(&pon)==-EOVERFLOW && pon.control_fault);
    for(int i=0;i<8;i++) {
        reset(); eth.qdma[1].closed[i]=0;
        assert(airoha_pon_reset_epoch(&pon)==-EIO && pon.control_fault && pon.generation==50); finish();
    }
    for(int i=0;i<3;i++) {
        reset(); eth.fe[i]=1;
        assert(airoha_pon_reset_epoch(&pon)==-EIO && pon.control_fault && pon.generation==50); finish();
    }
    reset(); eth.qdma[1].global=~0U;
    assert(airoha_pon_reset_epoch(&pon)==-EIO && !writes);
    reset(); eth.qdma[1].global|=8;
    assert(airoha_pon_reset_epoch(&pon)==-EIO && !writes);
    for(int i=1;i<=2;i++) {
        reset(); assert(!airoha_pon_reset_epoch(&pon)); pon.tx_enabled=1; fail_write=i;
        assert(airoha_pon_activate_rx(&pon,1)==-EIO && pon.control_fault && pon.rx_closed);
        assert(!eth.fe[1] && !(eth.qdma[1].global&4)); finish();
    }
    reset(); assert(!airoha_pon_reset_epoch(&pon)); pon.tx_enabled=1; changed_other=1;
    assert(airoha_pon_activate_rx(&pon,1)==-EIO && pon.rx_closed && pon.control_fault); finish();
    reset(); pon.netdev=NULL;
    assert(airoha_pon_reset_epoch(&pon)==-ENODEV);
    assert(airoha_pon_activate_rx(&pon,1)==-ENODEV);
    reset(); irq=1;
    assert(airoha_pon_reset_epoch(&pon)==-EWOULDBLOCK);
    assert(airoha_pon_activate_rx(&pon,1)==-EWOULDBLOCK);
    assert(airoha_pon_reset_epoch(NULL)==-EINVAL);
    assert(airoha_pon_activate_rx(NULL,1)==-EINVAL);
    return 0;
}
