/* Local fixture: protocol bytes and packet ownership reach the real callback. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned int uint;
typedef unsigned char unchar;
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define ETH_HLEN 14
#define CHECKSUM_NONE 0
#define CHECKSUM_UNNECESSARY 1
#define PACKET_HOST 0
#define Q1000K_PON_IDENTITY
#define TCSUPPORT_WAN_GPON
#define TCSUPPORT_CPU_EN7581
#undef __BIG_ENDIAN
#define PON_LINK_STATUS_GPON 1
#define PON_MSG(...) ((void)0)
#define printk(...) ((void)0)
#define ECNT_CONTINUE 1
#define XPON_DROP_PRINT ((void)0)
#define GPON_MULTICAST_GEM 1
#define PWAN_IF_OAM 0
#define PWAN_IF_OMCI 1
#define PWAN_IF_EAPOL 2
#define PWAN_IF_DATA 3
typedef unsigned int PWAN_IfType_t;
struct net_device;
struct sk_buff {
    unsigned len; unsigned char bytes[16130],*data,cb[48];
    struct net_device *dev; int ip_summed,pkt_type,protocol;
    bool freed,nonlinear,linearize_fail; unsigned mac_header,network_header;
};
struct stats { unsigned rx_packets,rx_bytes; };
typedef struct { struct stats stats; } PWAN_NetPriv_T;
struct net_device { PWAN_NetPriv_T priv; bool running; };
static int linearizations,ethernet_calls,parsers,deliveries,frees,loopback;
static unsigned delivered_len;
static unsigned char delivered[16130];
static struct net_device devices[4];
static struct { struct net_device *pPonNetDev[4]; unsigned dropUnknownPackets; } wan,*gpWanPriv=&wan;
static struct { int sysLinkStatus; } sys={.sysLinkStatus=PON_LINK_STATUS_GPON},*gpPonSysData=&sys;
static void *netdev_priv(struct net_device *d) { return &d->priv; }
static bool netif_running(struct net_device *d) { return d->running; }
static int skb_linearize(struct sk_buff *s) {
    linearizations++;
    if(s->linearize_fail) return -ENOMEM;
    s->nonlinear=false; return 0;
}
static void skb_reset_mac_header(struct sk_buff *s) { s->mac_header=s->data-s->bytes; }
static void skb_reset_network_header(struct sk_buff *s) { s->network_header=s->data-s->bytes; }
static int eth_type_trans(struct sk_buff *s,struct net_device *d) {
    assert(s->len>=14 && !s->nonlinear); ethernet_calls++; s->len-=14; s->data+=14; return 0x800;
}
static void dev_kfree_skb_any(struct sk_buff *s) { if(s) { assert(!s->freed); s->freed=true; frees++; } }
#define dev_kfree_skb(s) dev_kfree_skb_any(s)
static void netif_rx(struct sk_buff *s) {
    assert(!s->freed); deliveries++; delivered_len=s->len;
    memcpy(delivered,s->data,s->len); dev_kfree_skb_any(s);
}
#define netif_receive_skb(s) netif_rx(s)
static int FH_VLAN_FILTER(struct sk_buff *s) { assert(s->dev==&devices[PWAN_IF_DATA]); return 0; }
static int FH_VLAN_RX_PROC(struct sk_buff *s) { return 0; }
static void FH_VLAN_PARSER(struct sk_buff *s) {}
static void CALL_USER_HOOK_MULTICAST_RX_DATA(struct sk_buff *s) {}
static int ECNT_API_BBF247_PKT_DS_HANDLE(struct sk_buff *s) { return 0; }
static int ECNT_HOOK_MULTICAST_DATA_HANLDE(struct sk_buff *s) { return 1; }
static int check_and_do_1toN_vlan_opreation(struct sk_buff *s,char idx) { return 0; }
static int pwan_net_start_xmit(struct sk_buff *s,struct net_device *d) { netif_rx(s); return 0; }
/* TYPES */
_Static_assert(sizeof(PWAN_FERxMsg_T)==16,"AN7581 RX metadata ABI");
static int gwan_process_rx_message(PWAN_FERxMsg_T *msg,struct sk_buff *s,uint len,unchar *flag) {
    assert(s->len==len && !s->nonlinear && !s->freed); parsers++; *flag=loopback;
    return msg->raw.oam?PWAN_IF_OMCI:PWAN_IF_DATA;
}
/* HELPERS */
/* CALLBACK */
static void setup(struct sk_buff *s,unsigned len,bool omci) {
    memset(s,0,sizeof(*s)); s->len=len; s->data=s->bytes; s->ip_summed=CHECKSUM_UNNECESSARY;
    memset(s->cb,0xa5,sizeof(s->cb));
    for(unsigned i=0;i<sizeof(s->bytes);i++) s->bytes[i]=i;
    s->bytes[3]=omci?0x0a:0;
    linearizations=ethernet_calls=parsers=deliveries=frees=loopback=0;
    for(int i=0;i<4;i++) { devices[i].running=true; wan.pPonNetDev[i]=&devices[i]; }
}
int main(void) {
    struct sk_buff skb; u32 words[4]={0,0,0x01000000,0};
    for(int error=0;error<9;error++) {
        setup(&skb,48,true); words[0]=BIT(8)|BIT(30); words[2]=0x01000000;
        void *msg=words; unsigned msg_len=16,pkt_len=48;
        if(error==0) msg=NULL;
        if(error==1) msg_len=15;
        if(error==2) pkt_len=49;
        if(error==3) { skb.len=0; pkt_len=0; }
        if(error==4) { skb.len=16129; pkt_len=16129; }
        if(error==5) words[0]|=BIT(11);
        if(error==6) words[2]=0x02000000;
        if(error==7) skb.linearize_fail=true;
        if(error==8) skb.bytes[3]=0xff;
        assert(pwan_cb_rx_packet(msg,msg_len,&skb,pkt_len)<0);
        assert(skb.freed && frees==1 && !parsers && !deliveries);
    }
    for(int mic=0;mic<2;mic++) {
        words[0]=BIT(8)|(mic?BIT(30):0); words[2]=0x01000000;
        setup(&skb,44+4*mic,true); skb.nonlinear=true;
        skb.bytes[12]=0x88; skb.bytes[13]=0x8e;
        assert(!pwan_cb_rx_packet(words,16,&skb,skb.len));
        assert(deliveries==1 && delivered_len==44+4*mic && !ethernet_calls && frees==1);
        assert(delivered[0]==0 && delivered[3]==0x0a && delivered[12]==0x88 && delivered[13]==0x8e);
        assert(skb.ip_summed==CHECKSUM_NONE && skb.mac_header==0 && skb.network_header==0);
        setup(&skb,43+4*mic,true); assert(q1000k_pwan_rx_prepare(&skb,words,16,skb.len)==-EMSGSIZE);
        setup(&skb,512,true); skb.bytes[3]=0x0b;
        for(unsigned declared=0;declared<65536;declared++) {
            skb.bytes[8]=declared>>8; skb.bytes[9]=declared;
            int ret=q1000k_pwan_rx_prepare(&skb,words,16,skb.len);
            assert(ret==(declared+10+4*mic>512?-EMSGSIZE:0));
        }
    }
    setup(&skb,60,false); words[0]=0; skb.bytes[12]=0x88; skb.bytes[13]=0x8e;
    assert(!pwan_cb_rx_packet(words,16,&skb,60));
    assert(parsers==1 && deliveries==1 && ethernet_calls==1 && delivered_len==46 && frees==1);
    assert(skb.dev==&devices[PWAN_IF_DATA] && skb.ip_summed==CHECKSUM_NONE);
    for(unsigned i=0;i<sizeof(skb.cb);i++) assert(!skb.cb[i]);
    setup(&skb,13,false); assert(pwan_cb_rx_packet(words,16,&skb,13)<0 && !parsers && frees==1);
    setup(&skb,60,false); loopback=1; assert(!pwan_cb_rx_packet(words,16,&skb,60));
    assert(deliveries==1 && delivered_len==60 && !ethernet_calls && frees==1);
    setup(&skb,60,false); devices[PWAN_IF_DATA].running=false;
    assert(pwan_cb_rx_packet(words,16,&skb,60)<0 && frees==1 && !deliveries);
    setup(&skb,60,false); wan.pPonNetDev[PWAN_IF_DATA]=NULL;
    assert(pwan_cb_rx_packet(words,16,&skb,60)<0 && frees==1 && !deliveries);
    assert(q1000k_pwan_rx_prepare(NULL,words,16,60)==-EINVAL);
    return 0;
}
