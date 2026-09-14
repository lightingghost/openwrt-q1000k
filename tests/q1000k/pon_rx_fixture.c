/* RX pages and networking primitives are fixtures; the assembly path is real. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(v) (v)
#define le32_to_cpu(v) (v)
#define CHECKSUM_NONE 0
#define CHECKSUM_UNNECESSARY 1
#define AIROHA_RX_HEADROOM 64
#define AIROHA_RX_LEN(n) ((n)-AIROHA_RX_HEADROOM)
#define AIROHA_MAX_RX_SIZE 16128
#define IS_ERR(v) (!(v))
#define PKT_HASH_TYPE_L4 1
#define PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED 15
#define EBADMSG 74
struct page { u8 data[512]; bool freed; };
static struct page pages[16];
struct net_device;
struct skb_shared_info { unsigned nr_frags; struct page *frags[4]; };
struct sk_buff {
    struct net_device *dev; struct page *head; struct skb_shared_info shinfo;
    u8 *data; unsigned len; int ip_summed,protocol; bool freed;
};
struct airoha_gdm_dev;
struct net_device { struct airoha_gdm_dev *priv; };
struct airoha_gdm_port { struct { int dst; } *dsa_meta[8]; };
struct airoha_gdm_dev { struct net_device *netdev; struct airoha_gdm_port *port; bool pon_port; };
struct airoha_qdma_desc { u32 ctrl,msg0,msg1,msg2,msg3; };
struct airoha_queue_entry { u8 *buf; unsigned dma_addr,dma_len; };
struct airoha_qdma;
struct airoha_queue {
    struct airoha_qdma *qdma;
    struct airoha_queue_entry entry[16]; struct airoha_qdma_desc desc[16];
    int queued,tail,ndesc,buf_size,napi,page_pool;
    struct sk_buff *skb; bool pon_frame,rx_discard,pon_drain;
    u64 pon_generation; u32 pon_words[4];
};
struct airoha_eth { void *dev; struct { int dev; } *ppe; };
struct airoha_qdma { struct airoha_eth *eth; struct airoha_queue q_rx[1]; };
enum dma_data_direction { DIR };
static struct sk_buff skbs[16];
static struct net_device devices[3];
static struct airoha_gdm_dev gdms[3];
static int allocations,frees,ethernet_calls,lro_calls,gro_calls,pon_calls,pon_drops,alloc_fail;
static u64 generation=1;
static u32 delivered_words[4];
static unsigned delivered_len;
static u8 delivered_data[48];
static enum dma_data_direction page_pool_get_dma_dir(int pool) { return DIR; }
static void dma_rmb(void) {}
static void dma_sync_single_for_cpu(void *dev,unsigned addr,unsigned len,enum dma_data_direction d) {}
static struct page *virt_to_head_page(void *p) {
    for(int i=0;i<16;i++) if(p==pages[i].data+AIROHA_RX_HEADROOM) return &pages[i];
    assert(false); return NULL;
}
static u8 *page_address(struct page *p) { return p->data; }
static void page_pool_put_full_page(int pool,struct page *p,bool allow_direct) { assert(!p->freed); p->freed=true; }
static struct airoha_gdm_dev *airoha_qdma_get_gdm_dev(struct airoha_eth *e,struct airoha_qdma_desc *d) {
    unsigned i=d->msg1&3; return i<3?&gdms[i]:NULL;
}
static struct airoha_gdm_dev *netdev_priv(struct net_device *n) { return n->priv; }
static struct net_device *netdev_from_priv(struct airoha_gdm_dev *g) { return g->netdev; }
static struct sk_buff *napi_build_skb(void *data,unsigned size) {
    if(alloc_fail) return NULL;
    struct sk_buff *s=&skbs[allocations++]; assert(allocations<=16);
    s->data=data; s->head=virt_to_head_page((u8 *)data+AIROHA_RX_HEADROOM); return s;
}
static void skb_reserve(struct sk_buff *s,unsigned n) { s->data+=n; }
static void __skb_put(struct sk_buff *s,unsigned n) { s->len+=n; }
static void skb_mark_for_recycle(struct sk_buff *s) {}
static void skb_record_rx_queue(struct sk_buff *s,int q) { assert(q==0); }
static int eth_type_trans(struct sk_buff *s,struct net_device *n) {
    assert(!n->priv->pon_port && s->len>=14); ethernet_calls++; s->data+=14; s->len-=14; return 0x800;
}
static struct sk_buff *airoha_qdma_lro_rx_skb(struct airoha_queue *q,struct airoha_qdma_desc *d,struct airoha_queue_entry *e) {
    lro_calls++; return NULL;
}
static struct skb_shared_info *skb_shinfo(struct sk_buff *s) { return &s->shinfo; }
static void skb_add_rx_frag(struct sk_buff *s,int n,struct page *p,ptrdiff_t offset,int len,int size) {
    assert(n<4 && !p->freed); s->shinfo.frags[n]=p; s->shinfo.nr_frags++; s->len+=len;
}
static void dev_kfree_skb(struct sk_buff *s) {
    assert(!s->freed); s->freed=true; frees++;
    page_pool_put_full_page(0,s->head,true);
    for(unsigned i=0;i<s->shinfo.nr_frags;i++) page_pool_put_full_page(0,s->shinfo.frags[i],true);
}
static bool netdev_uses_dsa(struct net_device *n) { return false; }
static void skb_dst_set_noref(struct sk_buff *s,int *dst) { assert(false); }
static unsigned jhash_1word(unsigned hash,unsigned seed) { return hash; }
static void skb_set_hash(struct sk_buff *s,unsigned hash,int type) { assert(!s->dev->priv->pon_port); }
static void airoha_ppe_check_skb(int *ppe,struct sk_buff *s,unsigned hash,bool flow) { assert(!s->dev->priv->pon_port); }
static void napi_gro_receive(int *napi,struct sk_buff *s) { assert(!s->dev->priv->pon_port); gro_calls++; dev_kfree_skb(s); }
#define dev_kfree_skb_any dev_kfree_skb
static void airoha_qdma_fill_rx_queue(struct airoha_queue *q) {
    if(q->pon_drain) for(int i=0;i<16;i++) if(pages[i].freed) q->desc[i].ctrl=0;
}
struct airoha_pon_rx_meta { u32 words[4]; u16 gem; u8 channel; bool omci,no_mic; };
/* MASKS */
/* RX_META */
static u64 airoha_pon_generation(struct airoha_gdm_dev *g) { return generation; }
static void airoha_pon_rx(struct airoha_gdm_dev *g,struct sk_buff *s,const u32 words[4],u64 gen) {
    struct airoha_pon_rx_meta meta;
    assert(g->pon_port && s->dev==g->netdev && s->ip_summed==CHECKSUM_NONE);
    if(gen!=generation || !gen || airoha_pon_rx_meta(words,&meta)) pon_drops++;
    else {
        pon_calls++; delivered_len=s->len;
        memcpy(delivered_words,words,sizeof(delivered_words)); memcpy(delivered_data,s->data,48);
    }
    dev_kfree_skb(s);
}
/* PRODUCTION */
static struct airoha_eth eth;
static struct airoha_qdma qdma;
static struct airoha_queue *q=&qdma.q_rx[0];
static void setup(void) {
    memset(&qdma,0,sizeof(qdma)); qdma.eth=&eth; q->qdma=&qdma; q->ndesc=16; q->buf_size=512;
    memset(pages,0,sizeof(pages)); memset(skbs,0,sizeof(skbs));
    allocations=frees=ethernet_calls=lro_calls=gro_calls=pon_calls=pon_drops=alloc_fail=0; generation=1;
    for(int i=0;i<3;i++) { devices[i].priv=&gdms[i]; gdms[i].netdev=&devices[i]; gdms[i].pon_port=i!=1; }
    for(int i=0;i<16;i++) {
        q->entry[i].buf=pages[i].data+AIROHA_RX_HEADROOM; q->entry[i].dma_len=448; q->entry[i].dma_addr=i+1;
        for(int j=0;j<448;j++) q->entry[i].buf[j]=j;
    }
}
static void packet(int index,int port,int len,bool more) {
    q->desc[index]=(struct airoha_qdma_desc){.ctrl=BIT(31)|(more?BIT(29):0)|len,
        .msg0=0x40154108,.msg1=0x12345678u|port,.msg2=0x01000000,.msg3=0xa5a55a5a};
    q->queued++;
}
static void pages_freed(int n) { for(int i=0;i<n;i++) assert(pages[i].freed); }
int main(void) {
    setup(); packet(0,0,24,true); assert(!airoha_qdma_rx_process(q,8) && q->skb);
    packet(1,0,24,false); packet(2,1,48,false); packet(3,0,12,true);
    assert(!airoha_qdma_pon_discard_rx(q));
    assert(!q->skb && !q->rx_discard && !q->pon_frame && !q->pon_drain);
    assert(!pon_calls && !gro_calls && !ethernet_calls && !q->pon_generation);
    pages_freed(4);
    setup(); for(int i=0;i<16;i++) packet(i,0,48,true);
    assert(!airoha_qdma_pon_discard_rx(q) && !q->queued && !q->rx_discard);
    pages_freed(16); assert(!allocations && !pon_calls && !gro_calls);

    setup(); packet(0,0,48,false); assert(airoha_qdma_rx_process(q,8)==1);
    assert(pon_calls==1 && delivered_len==48 && !ethernet_calls && !lro_calls && !gro_calls);
    assert(!memcmp(delivered_words,&q->desc[0].msg0,16));
    for(int i=0;i<48;i++) assert(delivered_data[i]==i);
    pages_freed(1);
    setup(); packet(0,1,62,false); assert(airoha_qdma_rx_process(q,8)==1);
    assert(ethernet_calls==1 && gro_calls==1 && !pon_calls); pages_freed(1);
    setup(); packet(0,0,48,true); packet(1,0,16,false); q->desc[1].msg0=0;
    assert(airoha_qdma_rx_process(q,8)==1 && pon_calls==1 && delivered_len==64);
    assert(delivered_words[0]==0x40154108); pages_freed(2);
    for(int bit=11;bit<=13;bit++) {
        setup(); packet(0,0,48,true); packet(1,0,16,false); q->desc[1].msg0|=1u<<bit;
        assert(airoha_qdma_rx_process(q,8)==1 && !pon_calls && pon_drops==1); pages_freed(2);
    }
    for(int error=0;error<6;error++) {
        setup(); packet(0,0,48,true); packet(1,0,16,true); packet(2,0,16,false); packet(3,0,48,false);
        if(error==0) q->desc[1].msg1|=1; /* mixed Ethernet/PON chain */
        if(error==1) q->desc[1].msg1|=2; /* another PON port */
        if(error==2) q->desc[1].ctrl|=BIT(30); /* RX overflow */
        if(error==3) q->desc[1].ctrl&=~0xffffu; /* zero length */
        if(error==4) q->desc[1].msg2=0x02000000; /* aggregated tail */
        if(error==5) q->desc[1].msg1|=3; /* missing port */
        assert(airoha_qdma_rx_process(q,8)==2 && pon_calls==1 && !q->rx_discard && !q->skb);
        assert(allocations==2 && frees==2 && !ethernet_calls); pages_freed(4);
    }
    setup(); packet(0,0,48,true); alloc_fail=1; assert(!airoha_qdma_rx_process(q,8) && q->rx_discard);
    alloc_fail=0; q->desc[0].ctrl=0; packet(1,0,16,false); packet(2,0,48,false);
    assert(airoha_qdma_rx_process(q,8)==2 && pon_calls==1 && allocations==1); pages_freed(3);
    setup(); packet(0,0,48,true); q->desc[0].msg2=0x02000000; packet(1,0,16,false);
    assert(airoha_qdma_rx_process(q,8)==1 && !lro_calls && !allocations); pages_freed(2);
    setup(); packet(0,0,48,true); assert(!airoha_qdma_rx_process(q,8) && q->skb);
    generation=2; q->desc[0].ctrl=0; packet(1,0,16,false);
    assert(airoha_qdma_rx_process(q,8)==1 && !pon_calls && pon_drops==1); pages_freed(2);
    /* A ring full of invalid frames must stop at its available descriptors. */
    setup(); for(int i=0;i<16;i++) packet(i,0,0,false);
    assert(airoha_qdma_rx_process(q,8)==8 && q->queued==8);
    assert(airoha_qdma_rx_process(q,32)==8 && !q->queued && !allocations); pages_freed(16);
    return 0;
}
