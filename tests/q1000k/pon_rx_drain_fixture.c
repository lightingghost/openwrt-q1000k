// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define le32_to_cpu(x) (x)
#define GLOBAL_CFG_RX_DMA_EN_MASK 4
#define GLOBAL_CFG_RX_DMA_BUSY_MASK 8
#define REG_QDMA_GLOBAL_CFG 0
#define QDMA_INT_REG_IDX1 1
#define QDMA_INT_REG_IDX2 2
#define RX_IRQ_BANK_PIN_MASK(n) (0x102U<<(n))
#define INT_RX1_MASK(n) ((n)&0xff)
#define INT_RX2_MASK(n) (((n)>>8)&0xff)
#define REG_INT_ENABLE(bank,index) (4+(bank)*2+(index)-1)
#define QDMA_DESC_DONE_MASK 0x80000000
static int rtnl, spin, off_napi, discarded, syncs, clears, polls, dma_timeout, ignore_stop, allones, irq_writes, fail_irq, dirty_tail;
#define ASSERT_RTNL() assert(rtnl)
#define spin_lock_irqsave(l,f) do { assert(!spin && !*(l)); *(l)=1; spin=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(spin && *(l)); *(l)=0; spin=0; (void)(f); } while(0)
struct airoha_qdma;
struct airoha_queue { int ndesc,napi,tail; void *skb; bool pon_drain,rx_discard,pon_frame; uint64_t pon_generation; struct { u32 ctrl; } desc[2]; };
struct airoha_irq_bank { struct airoha_qdma *qdma; int irq_lock,irq; u32 irqmask[3]; };
struct airoha_eth;
struct airoha_qdma { struct airoha_eth *eth; int users; u32 regs[8]; struct airoha_irq_bank irq_banks[2]; struct airoha_queue q_rx[3]; };
struct airoha_eth { struct airoha_qdma qdma[2]; };
static struct airoha_eth eth;
static struct airoha_qdma *qdma=&eth.qdma[1];
static u32 airoha_qdma_rr(struct airoha_qdma *q,unsigned int reg)
{
    assert(q==qdma && rtnl);
    if(!reg && allones) return ~0U;
    return q->regs[reg];
}
static void airoha_qdma_wr(struct airoha_qdma *q,unsigned int reg,u32 value)
{
    assert(q==qdma && rtnl && spin && reg>=4 && reg<8);
    if(++irq_writes!=fail_irq) q->regs[reg]=value;
}
static void airoha_qdma_clear(struct airoha_qdma *q,unsigned int reg,u32 mask)
{
    assert(q==qdma && !reg && mask==4 && !spin && rtnl); clears++;
    if(!ignore_stop) q->regs[reg]&=~mask;
}
#define read_poll_timeout(fn,val,cond,sleep,timeout,before,args...) ({ \
    int ret=-ETIMEDOUT; assert(!spin && sleep==1000 && timeout==50000 && before); \
    for(polls=0;polls<50;polls++) { \
        if(!dma_timeout && polls==2) qdma->regs[0]&=~8U; \
        val=fn(args); if(cond) { ret=0; break; } \
    } ret; })
static void synchronize_irq(int irq) { assert(!spin && (irq==40 || irq==41)); syncs++; }
static void napi_disable(int *napi)
{
    assert(!spin && *napi && !(qdma->regs[0]&12)); *napi=0; off_napi++;
}
static void napi_enable(int *napi) { assert(!spin && !*napi); *napi=1; off_napi--; }
static void dev_kfree_skb_any(void *p) { assert(p && !spin && off_napi==2); }
static int airoha_qdma_rx_process(struct airoha_queue *q,int budget)
{
    assert(!spin && off_napi==2 && !(qdma->regs[0]&12) && q->pon_drain && budget==q->ndesc && !q->skb);
    discarded++; q->desc[q->tail].ctrl=dirty_tail ? QDMA_DESC_DONE_MASK : 0; return 0;
}
/* PRODUCTION */
static void reset(void)
{
    memset(&eth,0,sizeof(eth)); memset(&eth.qdma[0],0xa5,sizeof(eth.qdma[0]));
    qdma->eth=&eth; qdma->users=1; qdma->regs[0]=0xa5a5000d;
    for(int i=0;i<2;i++) {
        qdma->irq_banks[i].qdma=qdma; qdma->irq_banks[i].irq=40+i;
        for(int j=1;j<3;j++) {
            qdma->irq_banks[i].irqmask[j]=~0U;
            qdma->regs[REG_INT_ENABLE(i,j)]=~0U;
        }
        qdma->q_rx[i].ndesc=2; qdma->q_rx[i].napi=1;
        qdma->q_rx[i].skb=(void *)1;
        qdma->q_rx[i].pon_generation=99;
    }
    rtnl=1; spin=off_napi=discarded=syncs=clears=polls=dma_timeout=ignore_stop=allones=irq_writes=fail_irq=dirty_tail=0;
}
static void finish(void)
{
    assert(!spin && !off_napi);
    for(unsigned int i=0;i<sizeof(eth.qdma[0]);i++) assert(((unsigned char *)&eth.qdma[0])[i]==0xa5);
    for(int i=0;i<2;i++) assert(qdma->q_rx[i].napi && !qdma->q_rx[i].pon_drain);
    assert((qdma->regs[0]&~12U)==0xa5a50001);
}
int main(void)
{
    reset(); assert(!airoha_qdma_pon_drain_rx(qdma));
    assert(discarded==2 && clears==1 && syncs==4 && !qdma->q_rx[0].pon_generation);
    finish();
    for(int i=1;i<=4;i++) {
        reset(); fail_irq=i;
        assert(airoha_qdma_pon_drain_rx(qdma)==-EIO && !discarded); finish();
    }
    for(int i=9;i<=12;i++) {
        reset(); fail_irq=i;
        assert(airoha_qdma_pon_drain_rx(qdma)==-EIO && discarded==2); finish();
    }
    reset(); dma_timeout=1;
    assert(airoha_qdma_pon_drain_rx(qdma)==-ETIMEDOUT && !discarded && !syncs && !irq_writes); finish();
    reset(); ignore_stop=1;
    assert(airoha_qdma_pon_drain_rx(qdma)==-EIO && !discarded && !syncs); finish();
    reset(); allones=1;
    assert(airoha_qdma_pon_drain_rx(qdma)==-EIO && !clears && !discarded); finish();
    reset(); qdma->users=2;
    assert(airoha_qdma_pon_drain_rx(qdma)==-EBUSY && !clears); finish();
    reset(); dirty_tail=1;
    assert(airoha_qdma_pon_drain_rx(qdma)==-EIO && discarded==2); finish();
    return 0;
}
