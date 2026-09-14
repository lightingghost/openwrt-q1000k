// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define FIELD_MAX(m) ((m)>>__builtin_ctz(m))
#define GDM_SHORT_LEN_MASK 0x3fffU
#define GDM_LONG_LEN_MASK 0x3fff0000U
#define TWRR_WEIGHT_BASE_MASK 8U
#define TWRR_WEIGHT_SCALE_MASK (1U<<31)
#define REG_TXWRR_MODE_CFG 0x1020
#define REG_GDM_LEN_CFG(n) 3
struct airoha_pon_port_config { u16 min_len,max_len; bool byte_mode,scale16; };
#define AIROHA_GDM2_IDX 2
#define GLOBAL_CFG_RX_DMA_EN_MASK 4
#define GLOBAL_CFG_RX_DMA_BUSY_MASK 8
#define GLOBAL_CFG_TX_DMA_BUSY_MASK 2
#define REG_QDMA_GLOBAL_CFG 4
#define REG_QUEUE_CLOSE_CFG(n) (0xa0+((n)/4)*4)
#define REG_GDM_TXCHN_EN(n) 0
#define REG_GDM_RXCHN_EN(n) 1
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
struct airoha_qdma { u32 global,closed[8],mode; };
struct airoha_eth { struct airoha_qdma qdma[2]; u32 fe[4]; };
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
static void check(void) { assert(rtnl && pon.admission_lock); }
static u32 airoha_qdma_rr(struct airoha_qdma *q,u32 reg)
{
    check(); assert(q==&eth.qdma[1]);
    if(reg==4) return q->global;
    if(reg==0x1020) return q->mode;
    assert(reg>=0xa0 && reg<=0xbc && !(reg&3)); return q->closed[(reg-0xa0)/4];
}
static void airoha_qdma_wr(struct airoha_qdma *q,u32 reg,u32 value)
{
    check(); assert(q==&eth.qdma[1] && reg==0x1020);
    if(++writes!=fail_write) q->mode=value;
    if(changed_other) q->mode^=0x100;
}
static u32 airoha_fe_rr(struct airoha_eth *e,unsigned int reg)
{
    check(); assert(e==&eth && reg<4); return e->fe[reg];
}
static void airoha_fe_wr(struct airoha_eth *e,unsigned int reg,u32 value)
{
    check(); assert(e==&eth && reg==3);
    if(++writes!=fail_write) e->fe[reg]=value;
    if(changed_other) e->fe[reg]^=0x4000;
}
/* PRODUCTION */
static void reset(void)
{
    memset(&pon,0,sizeof(pon)); memset(&eth,0,sizeof(eth));
    memset(&eth.qdma[0],0xa5,sizeof(eth.qdma[0]));
    memset(eth.qdma[1].closed,0xff,sizeof(eth.qdma[1].closed));
    eth.qdma[1].global=0xa5a50061; eth.qdma[1].mode=0x400;
    eth.fe[3]=0xc000c000|(16128U<<16)|60;
    pon.netdev=(void *)1; pon.dma_dev=&dev; pon.fe_retired=~0U;
    pon.paused=pon.pause_ready=pon.rx_closed=pon.rx_drained=true;
    irq=rtnl=writes=fail_write=changed_other=0;
}
static void finish(void)
{
    assert(!rtnl && !pon.admission_lock);
    for(unsigned int i=0;i<sizeof(eth.qdma[0]);i++) assert(((unsigned char *)&eth.qdma[0])[i]==0xa5);
}
int main(void)
{
    struct airoha_pon_port_config old,cfg={60,2000,true,true},actual,sentinel;
    memset(&sentinel,0xa5,sizeof(sentinel));
    reset(); assert(!airoha_pon_get_port_config(&pon,&old));
    assert(old.min_len==60 && old.max_len==16128 && !old.byte_mode && !old.scale16 && !writes);
    assert(!airoha_pon_configure_port(&pon,&old,&cfg));
    assert(eth.fe[3]==(0xc000c000|(2000U<<16)|60));
    assert(eth.qdma[1].mode==0x80000408 && writes==2);
    assert(airoha_pon_configure_port(&pon,&old,&cfg)==-ESTALE && writes==2);
    assert(!airoha_pon_get_port_config(&pon,&actual) && airoha_pon_port_equal(&actual,&cfg));
    pon.paused=pon.rx_closed=false;
    assert(!airoha_pon_configure_port(&pon,&actual,&actual) && writes==2);
    assert(airoha_pon_configure_port(&pon,&actual,&old)==-EBUSY && writes==2); finish();
    for(int i=0;i<8;i++) {
        reset(); eth.qdma[1].closed[i]^=1;
        assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EIO && !writes && pon.control_fault); finish();
    }
    for(int i=0;i<3;i++) {
        reset(); eth.fe[i]=1;
        assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EIO && !writes && pon.control_fault); finish();
    }
    for(int i=0;i<6;i++) {
        reset();
        switch(i) {
        case 0: pon.paused=false; break;
        case 1: pon.pause_ready=false; break;
        case 2: pon.rx_closed=false; break;
        case 3: pon.rx_drained=false; break;
        case 4: pon.pending=1; break;
        case 5: pon.fe_retired=0; break;
        }
        assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EBUSY && !writes && !pon.control_fault); finish();
    }
    reset(); pon.fe_retired=0; pon.epoch_ready=true;
    assert(!airoha_pon_configure_port(&pon,&old,&cfg)); finish();
    for(int i=1;i<=2;i++) {
        reset(); fail_write=i;
        assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EIO && pon.control_fault && pon.paused && pon.rx_closed);
        int count=writes;
        assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EIO && writes==count); finish();
    }
    reset(); changed_other=1;
    assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EIO && pon.control_fault); finish();
    for(int i=0;i<2;i++) {
        reset(); if(i) eth.fe[3]=~0U; else eth.qdma[1].mode=~0U;
        actual=sentinel;
        assert(airoha_pon_get_port_config(&pon,&actual)==-EIO && pon.control_fault && !writes);
        assert(!memcmp(&actual,&sentinel,sizeof(actual))); finish();
    }
    reset(); pon.netdev=NULL;
    assert(airoha_pon_get_port_config(&pon,&actual)==-ENODEV && !writes);
    reset(); irq=1;
    assert(airoha_pon_get_port_config(&pon,&actual)==-EWOULDBLOCK && !writes);
    assert(airoha_pon_configure_port(&pon,NULL,&cfg)==-EINVAL);
    reset(); cfg.min_len=0;
    assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EINVAL && !writes);
    cfg.min_len=60; cfg.max_len=59;
    assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EINVAL && !writes);
    cfg.max_len=16384;
    assert(airoha_pon_configure_port(&pon,&old,&cfg)==-EINVAL && !writes);
    return 0;
}
