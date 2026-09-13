#!/usr/bin/env python3
"""Exercise native PON attachment and packet ownership without hardware."""
from pathlib import Path
import os
import re
import unittest
from pon_test_utils import run_c
REPO = Path(__file__).resolve().parents[2]

ETH = Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(
    REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))


class PonTransportTests(unittest.TestCase):
    def test_attachment_generations_metadata_and_skb_ownership(self):
        source = (ETH / 'airoha_pon.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        # Keep the wire masks from the real driver; numeric expectations below
        # are independent of FIELD_PREP and the production encode/decode code.
        regs = (ETH / 'airoha_regs.h').read_text()
        masks = '\n'.join(line for line in regs.splitlines() if re.match(
            r'#define (?:QDMA_ETH_(?:TXMSG_|RXMSG_AGG_COUNT_MASK)|REG_QUEUE_CLOSE_CFG)', line))
        masks += '\n' + re.search(r'#define GDM_BASE\(_n\).*?(?=\n\n)', regs, re.S).group()
        masks += '\n' + '\n'.join(line for line in regs.splitlines() if re.match(
            r'#define (?:GDM[1-4]_BASE|REG_GDM_(?:TXCHN_EN|LPBK_CFG)|LPBK_EN_MASK)', line))
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int netdev_tx_t;
#define __rcu
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 1
#define NETREG_REGISTERED 1
#define CHECKSUM_NONE 0
#define CHECKSUM_PARTIAL 1
#define AIROHA_NUM_TX_RING 32
#define AIROHA_GDM2_IDX 2
#define AIROHA_NUM_QOS_CHANNELS 4
#define U32_MAX UINT32_MAX
#define AIROHA_MAX_RX_SIZE 16128
#define NETIF_F_GRO_HW 1
#define NETIF_F_LRO 2
#define GFP_KERNEL 0
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((intptr_t)(p))
#define EXPORT_SYMBOL_GPL(...)
static int rtnl_held,rcu_readers,graces,alloc_fail,allocations,releases;
#define ASSERT_RTNL() assert(rtnl_held)
static void rtnl_lock(void) { assert(!rtnl_held && !rcu_readers); rtnl_held=1; }
static void rtnl_unlock(void) { assert(rtnl_held); rtnl_held=0; }
static void rcu_read_lock(void) { rcu_readers++; }
static void rcu_read_unlock(void) { assert(rcu_readers); rcu_readers--; }
#define rcu_access_pointer(p) (p)
#define rcu_dereference(p) (assert(rcu_readers), (p))
#define rcu_dereference_protected(p,c) (assert(c), (p))
#define rcu_assign_pointer(p,v) ((p)=(v))
#define lockdep_rtnl_is_held() (rtnl_held)
static void synchronize_net(void) { assert(rtnl_held && !rcu_readers); graces++; }
static void *kzalloc(size_t n,int flags) {
    if(alloc_fail) return NULL; allocations++; return calloc(1,n);
}
static void kfree(void *p) { if(p) { releases++; free(p); } }
typedef struct { int n; } atomic_t;
typedef struct { int n; } refcount_t;
typedef struct { int wakes; } wait_queue_head_t;
static int atomic_read(atomic_t *p) { return p->n; }
#define atomic_read_acquire atomic_read
static void atomic_set(atomic_t *p,int n) { p->n=n; }
static void atomic_inc(atomic_t *p) { p->n++; }
static void atomic_dec(atomic_t *p) { assert(p->n>0); p->n--; }
static int atomic_dec_return_release(atomic_t *p) { atomic_dec(p); return p->n; }
static bool atomic_dec_and_test(atomic_t *p) { atomic_dec(p); return !p->n; }
static void refcount_set(refcount_t *p,int n) { p->n=n; }
static void refcount_inc(refcount_t *p) { assert(p->n>0); p->n++; }
static bool refcount_dec_and_test(refcount_t *p) { assert(p->n>0); return !--p->n; }
static void init_waitqueue_head(wait_queue_head_t *p) { p->wakes=0; }
static void wake_up_all(wait_queue_head_t *p) { p->wakes++; }
#define msecs_to_jiffies(ms) (ms)
static void (*wait_progress)(void);
#define wait_event_timeout(wq,condition,timeout) ({ \
    assert(!rtnl_held && !rcu_readers); \
    if (!(condition) && (timeout) && wait_progress) wait_progress(); \
    (condition) ? 1 : 0; })
struct airoha_pon;
struct airoha_eth;
struct airoha_gdm_dev;
struct net_device_ops {};
static const struct net_device_ops airoha_netdev_ops;
struct netdev_queue { bool locked,stopped; };
struct net_device {
    const struct net_device_ops *netdev_ops;
    int reg_state,features;
    bool running,upper;
    struct airoha_gdm_dev *priv;
    struct netdev_queue txq[32];
    struct { int rx_dropped; } stats;
};
typedef struct { bool held; } spinlock_t;
#define lockdep_assert_held(l) assert((l)->held)
static void spin_lock_init(spinlock_t *l) { l->held=false; }
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!(l)->held); (l)->held=true; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert((l)->held); (l)->held=false; } while(0)
static bool bitmap_empty(unsigned long *map,int bits) { return !(*map & ((1UL<<bits)-1)); }
struct airoha_qdma { struct airoha_eth *eth; unsigned long qos_channel_map[1]; u32 regs[8]; };
static unsigned int reg_reads,reg_writes;
static int fault_register=-1;
static unsigned int reg_index(u32 offset) {
    assert(offset>=0xa0 && offset<=0xbc && !(offset&3)); return (offset-0xa0)/4;
}
static u32 airoha_qdma_rr(struct airoha_qdma *q,u32 offset) {
    reg_reads++; return q->regs[reg_index(offset)] ^ ((int)offset==fault_register ? 1 : 0);
}
static void airoha_qdma_wr(struct airoha_qdma *q,u32 offset,u32 value) {
    reg_writes++; q->regs[reg_index(offset)]=value;
}
static void airoha_qdma_rmw(struct airoha_qdma *q,u32 offset,u32 mask,u32 value) {
    airoha_qdma_wr(q,offset,(airoha_qdma_rr(q,offset)&~mask)|value);
}
struct airoha_gdm_dev {
    struct net_device *netdev;
    struct airoha_eth *eth;
    struct airoha_qdma *qdma;
    struct airoha_pon *pon;
    bool pon_port;
    u64 pon_generation;
    atomic_t pon_tx_pending;
};
struct airoha_gdm_port { struct airoha_gdm_dev *devs[2]; };
struct airoha_eth { struct airoha_gdm_port *ports[4]; struct airoha_qdma qdma[2]; u32 fe_tx,fe_loopback; };
static unsigned int fe_reads,fe_writes;
static bool fe_fault,fe_ignore_write;
static u32 airoha_fe_rr(struct airoha_eth *eth,u32 reg) {
    assert(rtnl_held || rcu_readers); fe_reads++;
    assert(reg==0x151c || reg==0x1524);
    return reg==0x151c ? eth->fe_loopback : eth->fe_tx ^ (fe_fault ? 1u<<19 : 0);
}
static void airoha_fe_wr(struct airoha_eth *eth,u32 reg,u32 value) {
    assert((rtnl_held || rcu_readers) && reg==0x1524); fe_writes++;
    if(!fe_ignore_write) eth->fe_tx=value;
}
struct sk_buff {
    struct net_device *dev;
    u16 queue;
    int len,ip_summed;
    bool gso,freed,frag_list,empty_head;
    u8 data[64];
};
struct airoha_pon_tx_meta { u64 epoch; u16 gem; u8 channel,queue,cpu_queue,mic_index; bool omci; };
struct airoha_pon_rx_meta { u32 words[4]; u16 gem; u8 channel; bool omci,no_mic; };
struct airoha_pon_ops {
    void (*rx)(void *,struct sk_buff *,const struct airoha_pon_rx_meta *);
    void (*tx_wake)(void *);
    void (*detached)(void *);
};
static struct airoha_gdm_dev *netdev_priv(struct net_device *d) { return d->priv; }
static struct net_device *netdev_from_priv(struct airoha_gdm_dev *d) { return d->netdev; }
static bool netif_running(struct net_device *d) { return d->running; }
static bool netdev_has_any_upper_dev(struct net_device *d) { return d->upper; }
static int skb_headlen(struct sk_buff *s) { return s->empty_head ? 0 : s->len; }
static bool skb_has_frag_list(struct sk_buff *s) { return s->frag_list; }
static bool skb_is_gso(struct sk_buff *s) { return s->gso; }
static int freed,rx_calls,wake_calls,detach_calls,xmit_calls;
static u32 last_msg;
static int xmit_result;
static void dev_kfree_skb_any(struct sk_buff *s) { assert(!s->freed); s->freed=true; freed++; }
static struct netdev_queue *netdev_get_tx_queue(struct net_device *n,u16 q) {
    assert(q<32); return &n->txq[q];
}
static void __netif_tx_lock_bh(struct netdev_queue *q) { assert(rcu_readers && !q->locked); q->locked=true; }
static void __netif_tx_unlock_bh(struct netdev_queue *q) { assert(q->locked); q->locked=false; }
static bool netif_xmit_stopped(struct netdev_queue *q) { return q->stopped; }
static u16 skb_get_queue_mapping(struct sk_buff *s) { return s->queue; }
static void skb_set_queue_mapping(struct sk_buff *s,u16 q) { s->queue=q; }
static netdev_tx_t airoha_pon_dev_xmit(struct sk_buff *s,struct net_device *n,u32 msg,struct airoha_pon *pon) {
    assert(s->dev==n && s->queue<32 && rcu_readers);
    xmit_calls++; last_msg=msg;
    if(xmit_result==NETDEV_TX_OK) dev_kfree_skb_any(s);
    return xmit_result;
}
''' + masks + '\n' + source + r'''
static struct airoha_pon *draining;
static void finish_pending(void) {
    while (atomic_read(&draining->pending)) airoha_pon_tx_complete(draining,0);
}
static int context;
static struct airoha_pon_rx_meta last_rx;
static void rx(void *p,struct sk_buff *skb,const struct airoha_pon_rx_meta *meta) {
    assert(p==&context && rcu_readers && !skb->freed);
    last_rx=*meta; rx_calls++; dev_kfree_skb_any(skb);
}
static void wake(void *p) { assert(p==&context && rcu_readers); wake_calls++; }
static void detached(void *p) { assert(p==&context && rtnl_held && !rcu_readers); detach_calls++; }
static struct airoha_pon_ops ops={rx,wake,detached};
static void reset_skb(struct sk_buff *skb,struct net_device *dev) {
    memset(skb,0,sizeof(*skb)); skb->dev=dev; skb->queue=7; skb->len=48;
    for(int i=0;i<64;i++) skb->data[i]=i;
}
int main(void) {
    struct airoha_eth eth={0}; struct airoha_gdm_port port={0};
    struct net_device dev={.netdev_ops=&airoha_netdev_ops,.reg_state=NETREG_REGISTERED,.running=true};
    struct airoha_gdm_dev gdm={.netdev=&dev,.eth=&eth,.qdma=&eth.qdma[1],.pon_port=true};
    struct net_device peer={0},upper={0};
    struct airoha_gdm_dev peer_gdm={.netdev=&peer,.eth=&eth,.qdma=&eth.qdma[1]};
    struct airoha_pon *pon,*next;
    struct sk_buff skb,before;
    struct airoha_pon_tx_meta tx={.gem=65535,.channel=31,.queue=7,.cpu_queue=31,.mic_index=1,.omci=true};
    u32 msg=0xa5a5a5a5,words[]={0x7fffc1f8,0x12345678,0x01000000,0xabcdef01};
    struct airoha_pon_rx_meta meta,saved;
    dev.priv=&gdm; eth.ports[1]=&port; port.devs[0]=&gdm; port.devs[1]=&peer_gdm;
    eth.qdma[0].eth=eth.qdma[1].eth=&eth;
    assert(PTR_ERR(airoha_pon_attach(NULL,&ops,&context))==-EINVAL);
    assert(PTR_ERR(airoha_pon_attach(&dev,NULL,&context))==-EINVAL);
    struct airoha_pon_ops incomplete=ops; incomplete.detached=NULL;
    assert(PTR_ERR(airoha_pon_attach(&dev,&incomplete,&context))==-EINVAL);
    alloc_fail=1; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-ENOMEM); alloc_fail=0;
    gdm.pon_port=false; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-ENODEV); gdm.pon_port=true;
    dev.running=false; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-ENODEV); dev.running=true;
    dev.reg_state=0; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-ENODEV); dev.reg_state=NETREG_REGISTERED;
    gdm.qdma=&eth.qdma[0]; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-ENODEV); gdm.qdma=&eth.qdma[1];
    dev.upper=true; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY); dev.upper=false;
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY); /* down QDMA1 peer */
    peer.features=NETIF_F_GRO_HW; assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY);
    peer_gdm.qdma=&eth.qdma[0]; /* LAN QDMA LRO does not block PON. */
    eth.qdma[1].qos_channel_map[0]=1;
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY);
    eth.qdma[1].qos_channel_map[0]=0;
    assert(reg_writes==0 && reg_reads==0 && !fe_reads && !fe_writes); /* failed reservations must not touch MMIO */
    eth.fe_loopback=1;
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY && !reg_writes && !fe_writes);
    eth.fe_loopback=0;
    fe_fault=true;
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EIO && !gdm.pon);
    fe_fault=false; fe_ignore_write=true; eth.fe_tx=BIT(5);
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EIO && !gdm.pon);
    fe_ignore_write=false;
    for(int reg=0xa0;reg<=0xbc;reg+=4) {
        fault_register=reg;
        assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EIO && !gdm.pon);
    }
    fault_register=-1;
    pon=airoha_pon_attach(&dev,&ops,&context); assert(pon==gdm.pon);
    for(int i=0;i<8;i++) assert(eth.qdma[1].regs[i]==UINT32_MAX && !eth.qdma[0].regs[i]);
    for(int ch=0;ch<32;ch++) {
        u8 closed=0;
        assert(!airoha_pon_get_queue_close(pon,ch,&closed) && closed==255);
    }
    tx.epoch=123;
    assert(airoha_pon_prepare_tx(pon,&tx)==-ESHUTDOWN && tx.epoch==123);
    assert(airoha_pon_set_queue_close(pon,31,0)==-ESHUTDOWN);
    for(int ch=0;ch<32;ch++) {
        assert(!airoha_pon_set_tx_channel(pon,ch,true));
        assert(eth.fe_tx==(UINT32_MAX>>(31-ch)) && pon->tx_enabled==eth.fe_tx);
    }
    assert(!airoha_pon_set_queue_close(pon,31,0));
    assert(!airoha_pon_prepare_tx(pon,&tx) && tx.epoch==1);
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY);
    rtnl_lock(); assert(airoha_pon_qdma_busy(&eth.qdma[1]) && !airoha_pon_qdma_busy(&eth.qdma[0])); rtnl_unlock();
    assert(!airoha_pon_tx_msg(&tx,&msg) && msg==0x7fffc1ff);
    for(int channel=0;channel<32;channel++) for(int queue=0;queue<8;queue++) {
        tx.channel=channel; tx.queue=queue;
        assert(!airoha_pon_tx_msg(&tx,&msg));
        assert(msg==(0x7fffc100u | (channel<<3) | queue));
    }
    msg=0xa5a5a5a5; tx.channel=32;
    assert(airoha_pon_tx_msg(&tx,&msg)==-EINVAL && msg==0xa5a5a5a5); tx.channel=31;
    tx.queue=8; assert(airoha_pon_tx_msg(&tx,&msg)==-EINVAL); tx.queue=7;
    tx.cpu_queue=32; assert(airoha_pon_tx_msg(&tx,&msg)==-EINVAL); tx.cpu_queue=31;
    tx.mic_index=2; assert(airoha_pon_tx_msg(&tx,&msg)==-EINVAL); tx.mic_index=1;
    tx.omci=false; assert(airoha_pon_tx_msg(&tx,&msg)==-EINVAL); tx.omci=true;
    reset_skb(&skb,&upper); before=skb; xmit_result=NETDEV_TX_BUSY;
    assert(airoha_pon_xmit(pon,&skb,&tx)==NETDEV_TX_BUSY && !memcmp(&skb,&before,sizeof(skb)));
    dev.txq[31].stopped=true;
    assert(airoha_pon_xmit(pon,&skb,&tx)==NETDEV_TX_BUSY && xmit_calls==1 && !memcmp(&skb,&before,sizeof(skb)));
    dev.txq[31].stopped=false; xmit_result=NETDEV_TX_OK;
    assert(airoha_pon_xmit(pon,&skb,&tx)==NETDEV_TX_OK && skb.freed && last_msg==0x7fffc1ff);
    for(int error=0;error<6;error++) {
        reset_skb(&skb,&upper); int old=xmit_calls;
        if(error==0) skb.gso=true;
        if(error==1) skb.ip_summed=CHECKSUM_PARTIAL;
        if(error==2) skb.len=0;
        if(error==3) skb.len=AIROHA_MAX_RX_SIZE+1;
        if(error==4) skb.frag_list=true;
        if(error==5) skb.empty_head=true;
        assert(airoha_pon_xmit(pon,&skb,&tx)==NETDEV_TX_OK && skb.freed && xmit_calls==old);
    }
    /* Every byte in all eight registers preserves its three neighbours. */
    for(int ch=0;ch<32;ch++) for(int bits=0;bits<256;bits++) {
        u32 saved_regs[8]; memcpy(saved_regs,eth.qdma[1].regs,sizeof(saved_regs));
        assert(!airoha_pon_set_queue_close(pon,ch,bits));
        for(int i=0;i<8;i++) {
            u32 expected=saved_regs[i];
            if(i==ch/4) expected=(expected & ~(255u<<((ch%4)*8))) | ((u32)bits<<((ch%4)*8));
            assert(eth.qdma[1].regs[i]==expected && !eth.qdma[0].regs[i]);
        }
        u8 result=0;
        assert(!airoha_pon_get_queue_close(pon,ch,&result) && result==bits);
        for(int q=0;q<8;q++) {
            struct airoha_pon_tx_meta lease={.channel=ch,.queue=q,.epoch=12345};
            int ret=airoha_pon_prepare_tx(pon,&lease);
            assert(ret==((bits & (1<<q)) ? -ESHUTDOWN : 0));
            if(ret) assert(lease.epoch==12345); else assert(lease.epoch==pon->epoch[ch]);
        }
    }
    unsigned int writes=reg_writes;
    u8 untouched=0xa5;
    assert(airoha_pon_set_queue_close(NULL,0,0)==-EINVAL);
    assert(airoha_pon_set_queue_close(pon,32,0)==-EINVAL);
    assert(airoha_pon_get_queue_close(pon,255,&untouched)==-EINVAL && untouched==0xa5);
    assert(airoha_pon_get_queue_close(pon,0,NULL)==-EINVAL && writes==reg_writes);
    assert(airoha_pon_prepare_tx(NULL,&tx)==-EINVAL);
    assert(airoha_pon_prepare_tx(pon,NULL)==-EINVAL);
    assert(!airoha_pon_set_queue_close(pon,31,0));
    assert(!airoha_pon_prepare_tx(pon,&tx));
    u64 epoch=tx.epoch;
    reset_skb(&skb,&upper); before=skb; xmit_result=NETDEV_TX_BUSY;
    assert(airoha_pon_xmit(pon,&skb,&tx)==NETDEV_TX_BUSY && !memcmp(&skb,&before,sizeof(skb)));
    assert(!airoha_pon_set_queue_close(pon,31,1)); /* closing another queue invalidates whole channel */
    assert(!airoha_pon_set_queue_close(pon,31,0));
    int sent=xmit_calls;
    assert(airoha_pon_xmit(pon,&skb,&tx)==NETDEV_TX_OK && skb.freed && xmit_calls==sent);
    assert(!airoha_pon_prepare_tx(pon,&tx) && tx.epoch!=epoch);
    xmit_result=NETDEV_TX_OK;
    reset_skb(&skb,&upper); assert(!airoha_pon_xmit(pon,&skb,&tx) && xmit_calls==sent+1);
    /* Native drain is per channel and permanent for this attachment. */
    assert(airoha_pon_quiesce_channel(NULL,0)==-EINVAL);
    assert(airoha_pon_quiesce_channel(pon,32)==-EINVAL);
    airoha_pon_tx_get(pon,31); airoha_pon_tx_get(pon,31); airoha_pon_tx_get(pon,4);
    assert(airoha_pon_quiesce_channel(pon,31)==-EAGAIN);
    writes=reg_writes;
    assert(airoha_pon_set_queue_close(pon,31,0)==-ESHUTDOWN && reg_writes==writes);
    assert(airoha_pon_prepare_tx(pon,&tx)==-ESHUTDOWN);
    reset_skb(&skb,&upper);
    assert(!airoha_pon_xmit(pon,&skb,&tx) && skb.freed && xmit_calls==sent+1);
    assert(!airoha_pon_set_queue_close(pon,4,0)); /* Other channel still operates. */
    airoha_pon_tx_complete(pon,31);
    assert(airoha_pon_quiesce_channel(pon,31)==-EAGAIN);
    airoha_pon_tx_complete(pon,31);
    assert(!airoha_pon_quiesce_channel(pon,31) && atomic_read(&pon->pending)==1);
    assert(!airoha_pon_quiesce_channel(pon,31));
    assert(airoha_pon_set_queue_close(pon,31,254)==-ESHUTDOWN);
    airoha_pon_tx_complete(pon,4);
    /* Verified state is not reported after an MMIO fault; TX stays blocked. */
    fault_register=0xbc;
    assert(airoha_pon_set_queue_close(pon,28,0)==-EIO);
    assert(airoha_pon_quiesce_channel(pon,31)==-EIO);
    assert(airoha_pon_get_queue_close(pon,31,&untouched)==-EIO && untouched==0xa5);
    assert(airoha_pon_prepare_tx(pon,&tx)==-EIO);
    writes=reg_writes; fault_register=-1;
    assert(airoha_pon_set_queue_close(pon,31,0)==-EIO && reg_writes==writes);
    reset_skb(&skb,&upper); assert(!airoha_pon_xmit(pon,&skb,&tx) && xmit_calls==sent+1 && skb.freed);
    assert(!airoha_pon_rx_meta(words,&meta));
    assert(meta.gem==65535 && meta.channel==31 && meta.omci && meta.no_mic && !memcmp(meta.words,words,sizeof(words)));
    for(int bit=11;bit<=13;bit++) {
        memset(&meta,0xa5,sizeof(meta)); saved=meta; words[0]|=1u<<bit;
        assert(airoha_pon_rx_meta(words,&meta)==-EBADMSG && !memcmp(&meta,&saved,sizeof(meta))); words[0]&=~(1u<<bit);
    }
    words[2]=0x02000000; assert(airoha_pon_rx_meta(words,&meta)==-EBADMSG); words[2]=0x01000000;
    u64 generation=airoha_pon_generation(&gdm); assert(generation);
    reset_skb(&skb,&dev); airoha_pon_rx(&gdm,&skb,words,generation);
    assert(skb.freed && rx_calls==1 && last_rx.gem==65535);
    airoha_pon_tx_wake(&gdm); assert(wake_calls==1);
    rtnl_lock(); airoha_pon_stop(&gdm); airoha_pon_stop(&gdm); rtnl_unlock();
    assert(detach_calls==1 && !gdm.pon && !airoha_pon_generation(&gdm));
    writes=reg_writes;
    assert(airoha_pon_quiesce_channel(pon,31)==-ENODEV && reg_writes==writes);
    assert(airoha_pon_set_queue_close(pon,31,0)==-ENODEV && reg_writes==writes);
    assert(airoha_pon_get_queue_close(pon,31,&untouched)==-ENODEV && untouched==0xa5);
    assert(airoha_pon_prepare_tx(pon,&tx)==-ENODEV);

    reset_skb(&skb,&upper); assert(!airoha_pon_xmit(pon,&skb,&tx) && skb.freed);
    next=airoha_pon_attach(&dev,&ops,&context); assert(next==gdm.pon && next->generation!=generation);
    reset_skb(&skb,&dev); airoha_pon_rx(&gdm,&skb,words,generation);
    assert(skb.freed && rx_calls==1 && dev.stats.rx_dropped==1);
    airoha_pon_release(pon); assert(gdm.pon==next); /* old release cannot detach a new consumer */
    airoha_pon_release(next); assert(!gdm.pon && detach_calls==1 && graces==2);
    airoha_pon_tx_wake(&gdm); assert(wake_calls==1);
    airoha_pon_release(NULL);
    pon=airoha_pon_attach(&dev,&ops,&context); assert(pon==gdm.pon);
    for(int i=0;i<3;i++) airoha_pon_tx_get(pon,0);
    assert(atomic_read(&pon->pending)==3 && atomic_read(&gdm.pon_tx_pending)==3);
    assert(airoha_pon_quiesce(pon,0)==-ETIMEDOUT && !gdm.pon && !pon->netdev);
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY);
    rtnl_lock(); assert(airoha_pon_qdma_busy(&eth.qdma[1])); rtnl_unlock();
    airoha_pon_tx_complete(pon,0);
    assert(airoha_pon_quiesce(pon,0)==-ETIMEDOUT);
    draining=pon; wait_progress=finish_pending;
    assert(!airoha_pon_quiesce(pon,10) && !atomic_read(&pon->pending) && pon->drained.wakes==1);
    assert(!airoha_pon_quiesce(pon,0));
    next=airoha_pon_attach(&dev,&ops,&context); assert(next==gdm.pon);
    airoha_pon_release(pon); assert(gdm.pon==next);
    airoha_pon_tx_get(next,0); airoha_pon_tx_get(next,0);
    rtnl_lock(); airoha_pon_stop(&gdm); rtnl_unlock();
    int before_release=releases;
    airoha_pon_release(next); assert(releases==before_release);
    assert(PTR_ERR(airoha_pon_attach(&dev,&ops,&context))==-EBUSY);
    before_release=releases;
    airoha_pon_tx_complete(next,0); assert(releases==before_release);
    airoha_pon_tx_complete(next,0); assert(releases==before_release+1);
    assert(!atomic_read(&gdm.pon_tx_pending));
    next=airoha_pon_attach(&dev,&ops,&context); assert(next==gdm.pon);
    airoha_pon_release(next);
    assert(airoha_pon_quiesce(NULL,1)==-EINVAL);
    airoha_pon_tx_get(NULL,0); airoha_pon_tx_complete(NULL,0);
    /* Independent attachments model physically quiescent hardware. */
    pon=airoha_pon_attach(&dev,&ops,&context); assert(pon==gdm.pon && !eth.fe_tx);
    unsigned int fw=fe_writes;
    assert(airoha_pon_set_tx_channel(NULL,0,true)==-EINVAL);
    assert(airoha_pon_set_tx_channel(pon,32,true)==-EINVAL && fw==fe_writes);
    for(int ch=0;ch<32;ch++) assert(!airoha_pon_set_tx_channel(pon,ch,true));
    assert(!airoha_pon_set_queue_close(pon,8,0));
    assert(airoha_pon_set_tx_channel(pon,8,true)==-EBUSY);
    struct airoha_pon_tx_meta lease={.channel=8};
    assert(!airoha_pon_prepare_tx(pon,&lease));
    airoha_pon_tx_get(pon,8);
    assert(!airoha_pon_set_queue_close(pon,8,255));
    assert(airoha_pon_set_tx_channel(pon,8,true)==-EAGAIN);
    assert(!airoha_pon_set_tx_channel(pon,8,false));
    assert(!(eth.fe_tx&BIT(8)) && (pon->retiring&BIT(8)) && atomic_read(&pon->channel_pending[8])==1);
    assert(airoha_pon_set_tx_channel(pon,8,true)==-ESHUTDOWN);
    assert(airoha_pon_set_queue_close(pon,8,0)==-ESHUTDOWN);
    airoha_pon_tx_complete(pon,8);
    assert(airoha_pon_set_tx_channel(pon,8,true)==-ESHUTDOWN);
    reset_skb(&skb,&upper); sent=xmit_calls;
    assert(!airoha_pon_xmit(pon,&skb,&lease) && skb.freed && xmit_calls==sent);
    for(int ch=0;ch<32;ch++) {
        u32 expected=eth.fe_tx & ~BIT(ch);
        assert(!airoha_pon_set_tx_channel(pon,ch,false));
        assert(eth.fe_tx==expected && pon->tx_enabled==expected);
    }
    airoha_pon_release(pon);
    for(int failure=0;failure<4;failure++) {
        pon=airoha_pon_attach(&dev,&ops,&context); assert(pon==gdm.pon);
        assert(!airoha_pon_set_tx_channel(pon,7,true));
        if(failure==3) assert(!airoha_pon_set_queue_close(pon,7,0));
        if(failure==0 || failure==3) fe_ignore_write=true;
        if(failure==1) fe_fault=true;
        if(failure==2) fault_register=0xa0;
        int err=failure==3 ? airoha_pon_set_tx_channel(pon,7,false) :
            failure==2 ? airoha_pon_set_tx_channel(pon,0,false) : airoha_pon_set_tx_channel(pon,0,true);
        assert(err==-EIO && pon->control_fault);
        if(failure==3) {
            assert((pon->retiring & BIT(7)) && pon->closed[7]==255);
            assert(eth.fe_tx==BIT(7)); /* failed disable cannot claim FE stopped */
        }
        fw=fe_writes;
        assert(airoha_pon_set_tx_channel(pon,1,true)==-EIO && fe_writes==fw);
        lease.channel=7; lease.epoch=123;
        assert(airoha_pon_prepare_tx(pon,&lease)==-EIO && lease.epoch==123);
        reset_skb(&skb,&upper); sent=xmit_calls;
        assert(!airoha_pon_xmit(pon,&skb,&lease) && skb.freed && xmit_calls==sent);
        if(failure==3) {
            assert(airoha_pon_set_tx_channel(pon,7,false)==-EIO);
            assert(eth.fe_tx==BIT(7) && pon->tx_enabled==BIT(7) && pon->control_fault);
        }
        fe_ignore_write=fe_fault=false; fault_register=-1;
        /* A later disable attempts all-off but never clears the fault. */
        assert(airoha_pon_set_tx_channel(pon,7,false)==-EIO && !eth.fe_tx && !pon->tx_enabled);
        lease.channel=7; lease.epoch=123;
        assert(airoha_pon_prepare_tx(pon,&lease)==-EIO && lease.epoch==123);
        assert(!airoha_pon_quiesce(pon,0)); fw=fe_writes;
        assert(airoha_pon_set_tx_channel(pon,0,false)==-ENODEV && fe_writes==fw);
        airoha_pon_release(pon);
    }
    assert(allocations==releases && !rtnl_held && !rcu_readers);
    return 0;
}
''', flags=['-Wno-misleading-indentation', '-Wno-sign-compare'])


if __name__ == '__main__':
    unittest.main()
