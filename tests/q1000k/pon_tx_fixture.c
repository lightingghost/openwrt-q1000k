/* Local DMA/queue fixtures. PRODUCTION is replaced with the native functions. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __be16;
typedef uint16_t __sum16;
typedef uintptr_t dma_addr_t;
typedef int netdev_tx_t;
#define __force
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 1
#define CHECKSUM_PARTIAL 1
#define SKB_GSO_TCPV4 1
#define SKB_GSO_TCPV6 2
#define DMA_TO_DEVICE 1
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define WRITE_ONCE(p,v) ((p)=(v))
#define cpu_to_le32(v) (v)
#define cpu_to_be16(v) __builtin_bswap16(v)
#define unlikely(v) (v)
#define WARN_ON_ONCE(v) (v)
#define AIROHA_NUM_QOS_QUEUES 8
#define REG_TX_CPU_IDX(n) (0x100+(n)*4)
#define REG_TX_DMA_IDX(n) (0x200+(n)*4)
#define TX_RING_CPU_IDX_MASK 0xffff
#define TX_RING_DMA_IDX_MASK 0xffff
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(n) struct list_head n={&n,&n}
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
#define list_for_each_entry(p,h,m) \
    for(p=list_first_entry(h,__typeof__(*p),m); &p->m!=(h); p=list_first_entry(&p->m,__typeof__(*p),m))
static void init_list(struct list_head *h) { h->next=h->prev=h; }
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_add_tail(struct list_head *e,struct list_head *h) {
    e->prev=h->prev; e->next=h; h->prev->next=e; h->prev=e;
}
static void list_move_tail(struct list_head *e,struct list_head *h) {
    e->prev->next=e->next; e->next->prev=e->prev; list_add_tail(e,h);
}
static void list_splice(struct list_head *s,struct list_head *h) {
    if(list_empty(s)) return;
    s->next->prev=h; s->prev->next=h->next; h->next->prev=s->prev; h->next=s->next;
}
struct airoha_gdm_dev;
struct netdev_queue { bool stopped; int sent; };
struct net_device {
    struct airoha_gdm_dev *priv;
    struct { void *parent; } dev;
    struct { int tx_dropped; } stats;
    struct netdev_queue txq[32];
};
typedef struct { u8 data[16]; u32 size; } skb_frag_t;
struct skb_shared_info { u32 nr_frags,gso_type,gso_size; skb_frag_t frags[4]; };
struct sk_buff {
    struct net_device *dev;
    u32 len,headlen; u16 queue;
    bool gso,freed,orphaned;
    int ip_summed; u8 data[128];
    struct skb_shared_info shinfo;
    struct { __sum16 check; } tcp;
};
struct airoha_qdma_desc { u32 ctrl,addr,data,msg0,msg1,msg2; };
struct airoha_queue_entry { struct list_head list; struct sk_buff *skb; dma_addr_t dma_addr; u16 dma_len; };
struct airoha_qdma;
struct airoha_queue {
    struct airoha_qdma *qdma; bool lock,txq_stopped;
    unsigned int ndesc,queued,free_thr;
    struct list_head tx_list;
    struct airoha_queue_entry entry[8]; struct airoha_qdma_desc desc[8];
};
struct airoha_eth { void *dev; };
struct airoha_qdma { struct airoha_eth *eth; struct airoha_queue q_tx[32]; };
struct airoha_gdm_dev { struct airoha_qdma *qdma; bool pon_port; u8 nbq; };
static int rcu_readers,maps,unmaps,map_fail,freed,orphans,doorbells,dsa_calls;
static bool batching;
static bool dma_live[64];
static void rcu_read_lock(void) { rcu_readers++; }
static void rcu_read_unlock(void) { assert(rcu_readers); rcu_readers--; }
#define rcu_dereference(p) (assert(rcu_readers),(p))
static void spin_lock_bh(bool *l) { assert(!*l); *l=true; }
static void spin_unlock_bh(bool *l) { assert(*l); *l=false; }
static struct airoha_gdm_dev *netdev_priv(struct net_device *n) { return n->priv; }
static u16 skb_get_queue_mapping(struct sk_buff *s) { return s->queue; }
static int airoha_qdma_get_txq(struct airoha_qdma *q,u16 n) { return n%32; }
static u32 airoha_get_dsa_tag(struct sk_buff *s,struct net_device *n) { dsa_calls++; return 0x1234; }
static bool skb_is_gso(struct sk_buff *s) { return s->gso; }
static int skb_cow_head(struct sk_buff *s,int n) { return 0; }
#define skb_shinfo(s) (&(s)->shinfo)
#define tcp_hdr(s) (&(s)->tcp)
static u8 airoha_get_fe_port(struct airoha_gdm_dev *d) { return 2; }
static struct netdev_queue *skb_get_tx_queue(struct net_device *n,struct sk_buff *s) { return &n->txq[s->queue]; }
static void netif_tx_stop_queue(struct netdev_queue *q) { q->stopped=true; }
static bool netif_xmit_stopped(struct netdev_queue *q) { return q->stopped; }
static void skb_orphan(struct sk_buff *s) { assert(!s->orphaned && !s->freed); s->orphaned=true; orphans++; }
static u32 skb_headlen(struct sk_buff *s) { return s->headlen; }
static dma_addr_t dma_map_single(void *dev,void *data,u32 len,int dir) {
    assert(data && len && dir==DMA_TO_DEVICE); maps++;
    if(maps==map_fail) return 0;
    assert(maps<64 && !dma_live[maps]); dma_live[maps]=true; return maps;
}
static bool dma_mapping_error(void *d,dma_addr_t addr) { return !addr; }
static void dma_unmap_single(void *d,dma_addr_t addr,u32 len,int dir) {
    assert(addr<64 && dma_live[addr] && len); dma_live[addr]=false; unmaps++;
}
static void *skb_frag_address(skb_frag_t *f) { return f->data; }
static u32 skb_frag_size(skb_frag_t *f) { return f->size; }
static void skb_tx_timestamp(struct sk_buff *s) { assert(!s->freed); }
static void netdev_tx_sent_queue(struct netdev_queue *q,u32 len) { q->sent+=len; }
static bool netdev_xmit_more(void) { return batching; }
static void airoha_qdma_rmw(struct airoha_qdma *q,u32 reg,u32 mask,u32 val) { doorbells++; }
static void dev_kfree_skb_any(struct sk_buff *s) { if(s) { assert(!s->freed); s->freed=true; freed++; } }
/* MASKS */
/* PRODUCTION */
static void setup(struct airoha_qdma *qdma,struct airoha_eth *eth,struct net_device *dev,struct sk_buff *skb,int fragments) {
    memset(qdma,0,sizeof(*qdma)); qdma->eth=eth;
    for(int q=0;q<32;q++) {
        struct airoha_queue *ring=&qdma->q_tx[q]; ring->qdma=qdma; ring->ndesc=8; ring->free_thr=2;
        init_list(&ring->tx_list);
        for(int i=0;i<8;i++) list_add_tail(&ring->entry[i].list,&ring->tx_list);
    }
    memset(skb,0,sizeof(*skb)); skb->dev=dev; skb->queue=31; skb->headlen=48;
    skb->shinfo.nr_frags=fragments; skb->len=48+fragments*16;
    for(int i=0;i<fragments;i++) skb->shinfo.frags[i].size=16;
    memset(dev->txq,0,sizeof(dev->txq)); maps=unmaps=freed=orphans=doorbells=dsa_calls=map_fail=0;
    memset(dma_live,0,sizeof(dma_live));
}
static int free_entries(struct airoha_queue *q) {
    int n=0; struct list_head *p;
    for(p=q->tx_list.next;p!=&q->tx_list;p=p->next) { assert(++n<=8); assert(p->next->prev==p); }
    return n;
}
int main(void) {
    struct airoha_eth eth={0}; struct airoha_qdma qdma;
    struct airoha_gdm_dev gdm={.qdma=&qdma,.pon_port=true}; struct net_device dev={.priv=&gdm};
    struct sk_buff skb,before; u32 msg=0x40afc15d; struct airoha_queue *q=&qdma.q_tx[31];
    for(int frags=0;frags<=3;frags++) for(int failure=0;failure<=frags+1;failure++) {
        setup(&qdma,&eth,&dev,&skb,frags); map_fail=failure; batching=true;
        assert(__airoha_dev_xmit(&skb,&dev,&msg)==NETDEV_TX_OK && !rcu_readers && !q->lock && !dsa_calls);
        assert(orphans==1);
        if(failure) {
            assert(maps==failure && unmaps==failure-1 && freed==1 && !q->queued && !doorbells);
            assert(free_entries(q)==8 && !dev.txq[31].sent);
            for(int i=0;i<8;i++) assert(!q->entry[i].dma_addr);
        } else {
            assert(maps==frags+1 && !unmaps && !freed && q->queued==frags+1 && doorbells==1);
            assert(free_entries(q)==8-frags-1 && dev.txq[31].sent==(int)skb.len);
            for(int i=0;i<=frags;i++) {
                assert(q->desc[i].msg0==msg && q->desc[i].msg1==0x7f2007ff);
                assert(q->desc[i].msg2==0xffff && q->entry[i].skb==(i==frags?&skb:NULL));
                assert(!!(q->desc[i].ctrl&(1u<<29))==(i<frags));
            }
            airoha_qdma_cleanup_tx_queue(q);
            assert(unmaps==frags+1 && freed==1 && !q->queued && free_entries(q)==8);
            airoha_qdma_cleanup_tx_queue(q); assert(freed==1 && unmaps==frags+1);
        }
        for(int i=0;i<64;i++) assert(!dma_live[i]);
    }
    setup(&qdma,&eth,&dev,&skb,3); q->queued=4; before=skb;
    assert(__airoha_dev_xmit(&skb,&dev,&msg)==NETDEV_TX_BUSY && !memcmp(&skb,&before,sizeof(skb)));
    assert(!maps && !freed && !orphans && !doorbells && dev.txq[31].stopped && q->txq_stopped);
    for(int mode=0;mode<4;mode++) {
        setup(&qdma,&eth,&dev,&skb,0); gdm.pon_port=mode!=0;
        if(mode==2) skb.gso=true;
        if(mode==3) skb.ip_summed=CHECKSUM_PARTIAL;
        assert(__airoha_dev_xmit(&skb,&dev,mode==1?NULL:&msg)==NETDEV_TX_OK);
        assert(freed==1 && !maps && !orphans);
    }
    setup(&qdma,&eth,&dev,&skb,0); gdm.pon_port=false; batching=false;
    assert(!__airoha_dev_xmit(&skb,&dev,NULL));
    assert(dsa_calls==1 && !orphans && q->desc[0].msg0==0x048d001f && doorbells==1);
    airoha_qdma_cleanup_tx_queue(q); assert(freed==1 && maps==unmaps);
    return 0;
}
