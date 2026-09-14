/* The real vendor ndo runs; native queue and flow preparation are fixtures. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
static int rcu_depth;
typedef unsigned int uint;
typedef uint16_t uint16;
typedef uint16_t u16;
#define Q1000K_PON_IDENTITY
#define TCSUPPORT_WAN_GPON
#define TCSUPPORT_CPU_EN7581
#undef __BIG_ENDIAN
#define NETDEV_TX_OK 0
#define GFP_ATOMIC 0
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define PWAN_IF_DATA 0
#define PWAN_IF_OMCI 1
#define PWAN_IF_OAM 2
#define PWAN_IF_EAPOL 3
#define PON_LINK_STATUS_OFF 0
#define PON_LINK_STATUS_GPON 1
#define PON_LINK_STATUS_EPON 2
#define GPON_CURR_STATE 5
#define GPON_10G_STATE_O5 5
#define GPON_10G_STATE_O9 9
#define GPON_OMCC_ID 5
#define GPON_GEM_IDX_MASK 7
#define CONFIG_GPON_10G_MAX_GEMPORT 8
#define XPON_DROP_PRINT ((void)0)
#define PON_MSG(...) ((void)0)
#define printk(...) ((void)0)
#define unlikely(x) (x)
#define SOFT_LOOPBACK_MODE(p) 0
#define CHECKSUM_UNNECESSARY 1
#define FH_VLAN_OPERATION(s) ((void)0)
#define FH_DSCP_OPERATION(s) ((void)0)
static bool ready, unshare_fail, linearize_fail, pad_fail, flow_fail;
static int submit_error, submits, frees, drops, updates;
static unsigned submitted_len;
static int ng2_o4_to_09, xgpon_fast_mode_flag;
struct stats { unsigned tx_packets, tx_bytes, tx_dropped; };
typedef struct { int netIdx; struct stats stats; } PWAN_NetPriv_T;
struct net_device { PWAN_NetPriv_T priv; };
struct sk_buff {
    unsigned len; unsigned char data[128]; struct net_device *dev;
    struct { u16 gem_port; } cb;
    bool freed, nonlinear; int protocol,ip_summed;
};
struct port_info { unsigned fast; };
static struct { int sysLinkStatus; } sys,*gpPonSysData=&sys;
static struct {
    struct { unsigned gemIdToIndex[65536]; struct { struct stats stats; } gemPort[8]; } gpon;
} wan,*gpWanPriv=&wan;
#define XPON_SKB_CB(s) (&(s)->cb)
static bool xpon_is_ready(void) { return ready; }
static void *netdev_priv(struct net_device *d) { return &d->priv; }
static void dev_kfree_skb_any(struct sk_buff *s) { assert(!s->freed); s->freed=true; frees++; }
static struct sk_buff *skb_unshare(struct sk_buff *s,int flags) {
    assert(!s->freed);
    if(unshare_fail) { dev_kfree_skb_any(s); return NULL; }
    return s;
}
static int skb_linearize(struct sk_buff *s) {
    assert(!s->freed);
    if(linearize_fail) return -ENOMEM;
    s->nonlinear=false; return 0;
}
static void skb_put(struct sk_buff *s,unsigned len) {
    assert(!s->freed && !s->nonlinear && s->len+len<=sizeof(s->data)); s->len+=len;
}
static int skb_padto(struct sk_buff *s,unsigned len) {
    assert(!s->freed);
    if(pad_fail) { dev_kfree_skb_any(s); return -ENOMEM; }
    return 0;
}
static void dec_pwan_tx_data_cnt(void) { drops++; }
static int mirror_post_tx_prepare(void) { return 0; }
static struct sk_buff *copy_pwan_skb(struct sk_buff *s,int index) { assert(0); return NULL; }
static int eth_type_trans(struct sk_buff *s,struct net_device *d) { assert(0); return 0; }
static void netif_rx(struct sk_buff *s) { assert(0); }
static void netif_trans_update(struct net_device *d) { updates++; }
/* TYPES */
_Static_assert(sizeof(PWAN_FETxMsg_T)==8,"AN7581 TX metadata ABI");
static int gwan_prepare_tx_message(PWAN_FETxMsg_T *msg,int index,struct sk_buff *s,
                                  int q,struct port_info *info) {
    assert(rcu_depth==1 && !s->freed && !s->nonlinear);
    if(flow_fail) return -EOPNOTSUPP;
    msg->raw.gem=5; msg->raw.fport=2; msg->raw.mtr_g=127;
    msg->raw.acnt_g0=31; msg->raw.acnt_g1=31;
    msg->raw.oam=index==PWAN_IF_OMCI; msg->raw.ndp=index==PWAN_IF_OMCI;
    s->cb.gem_port=5; return 0;
}
static int q1000k_transport_xmit(struct sk_buff *s,uint word0,uint word1) {
    assert(rcu_depth==1 && !s->freed && !s->nonlinear);
    assert(word0==(5u<<14 | (s->dev->priv.netIdx==PWAN_IF_OMCI?1u<<8:0)));
    assert(word1==(0x7f2007dfu | (s->dev->priv.netIdx==PWAN_IF_OMCI?1u<<31:0)));
    submitted_len=s->len; submits++;
    s->cb.gem_port=0xffff; /* Post-submit accounting must use descriptor metadata. */
    if(!submit_error) dev_kfree_skb_any(s); /* Can complete before ndo returns. */
    return submit_error;
}
static void q1000k_gwan_account(u16 gem,bool tx,unsigned int bytes) {
    assert(gem==5 && tx && bytes==60);
    wan.gpon.gemPort[3].stats.tx_packets++;
    wan.gpon.gemPort[3].stats.tx_bytes+=bytes;
}
static void rcu_read_lock(void) { rcu_depth++; }
static void rcu_read_unlock(void) { assert(rcu_depth==1); rcu_depth--; }
/* PRODUCTION */
static void setup(struct sk_buff *s,struct net_device *d,int index) {
    memset(s,0,sizeof(*s)); memset(d,0,sizeof(*d)); memset(&wan,0,sizeof(wan));
    s->len=60; s->nonlinear=true; d->priv.netIdx=index;
    sys.sysLinkStatus=PON_LINK_STATUS_GPON; wan.gpon.gemIdToIndex[5]=3;
    ready=true; unshare_fail=linearize_fail=pad_fail=flow_fail=false;
    submit_error=submits=frees=drops=updates=0; submitted_len=0;
}
int main(void) {
    struct sk_buff skb; struct net_device dev;
    int errors[]={0,-ENOBUFS,-ENOMEM,-ENODEV,-EOPNOTSUPP};
    for(int index=PWAN_IF_DATA;index<=PWAN_IF_OMCI;index++) for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        setup(&skb,&dev,index); submit_error=errors[i];
        assert(pwan_net_start_xmit(&skb,&dev)==NETDEV_TX_OK && !rcu_depth);
        assert(frees==1 && skb.freed && submits==1 && submitted_len==60);
        assert(dev.priv.stats.tx_packets==!errors[i] && dev.priv.stats.tx_bytes==(!errors[i]?60:0));
        assert(dev.priv.stats.tx_dropped==!!errors[i]);
        assert(wan.gpon.gemPort[3].stats.tx_packets==!errors[i]);
    }
    for(int fault=0;fault<9;fault++) {
        setup(&skb,&dev,PWAN_IF_DATA);
        if(fault==0) ready=false;
        if(fault==1) skb.len=0;
        if(fault==2) skb.len=16129;
        if(fault==3) unshare_fail=true;
        if(fault==4) linearize_fail=true;
        if(fault==5) skb.len=13;
        if(fault==6) flow_fail=true;
        if(fault==7) sys.sysLinkStatus=PON_LINK_STATUS_OFF;
        if(fault==8) { skb.len=14; pad_fail=true; }
        assert(pwan_net_start_xmit(&skb,&dev)==NETDEV_TX_OK && !rcu_depth);
        assert(frees==1 && skb.freed && !submits && dev.priv.stats.tx_dropped==1);
    }
    setup(&skb,&dev,PWAN_IF_DATA); skb.len=14;
    assert(pwan_net_start_xmit(&skb,&dev)==NETDEV_TX_OK && !rcu_depth);
    assert(frees==1 && submitted_len==60 && dev.priv.stats.tx_bytes==60);
    return 0;
}
