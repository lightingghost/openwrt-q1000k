// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int uint;
typedef uint16_t u16;
typedef uint32_t u32;
#define Q1000K_PON_IDENTITY
#define BIT(n) (UINT32_C(1)<<(n))
#define PWAN_IF_DATA 3
#define CHECKSUM_NONE 0
typedef struct { struct { unsigned int rx_packets,rx_bytes,rx_dropped; } stats; } PWAN_NetPriv_T;
static PWAN_NetPriv_T stats;
struct net_device { bool running; };
struct sk_buff { unsigned int len; int ip_summed,protocol; };
static struct net_device dev={.running=true};
static struct { struct net_device *pPonNetDev[4]; } wan,*gpWanPriv=&wan;
static int prepared, service_error, live, freed, delivered, omci, readers;
static bool ready;
static void rcu_read_lock(void) { assert(!readers); readers++; }
static void rcu_read_unlock(void) { assert(readers==1); readers--; }
static bool xpon_is_ready(void) { return ready; }
static void *netdev_priv(struct net_device *d) { assert(d==&dev); return &stats; }
static bool netif_running(struct net_device *d) { return d->running; }
static void dev_kfree_skb_any(struct sk_buff *skb) { assert(readers && live==1); live--; freed++; free(skb); }
static int q1000k_pwan_rx_prepare(struct sk_buff *skb,void *msg,uint n,uint bytes) {
    assert(readers && n==16 && skb->len==bytes); return prepared;
}
static int q1000k_services_rx(struct sk_buff *skb,u16 gem) { assert(readers && gem==500); return service_error; }
static void q1000k_omci_receive(struct sk_buff *skb,u16 gem,bool crc_error) {
    assert(readers && gem==500); omci++; dev_kfree_skb_any(skb);
}
static int eth_type_trans(struct sk_buff *skb,struct net_device *d) {
    assert(d==&dev && skb->len>=14); skb->len-=14; return 0x800;
}
static void netif_rx(struct sk_buff *skb) {
    assert(skb->ip_summed==CHECKSUM_NONE && skb->protocol==0x800);
    delivered++; dev_kfree_skb_any(skb);
}
/* PRODUCTION */
static int receive(u32 flags) {
    u32 msg[4]={500U<<14|flags}; struct sk_buff *skb=calloc(1,sizeof(*skb));
    assert(!live); live=1; skb->len=60; skb->ip_summed=99;
    int ret=xpondrv_rx_packet(msg,sizeof(msg),skb,skb->len);
    assert(!live && !readers); return ret;
}
int main(void) {
    assert(receive(0)==-ENODEV && freed==1 && !stats.stats.rx_dropped);
    ready=true; wan.pPonNetDev[3]=&dev;
    prepared=-EMSGSIZE; assert(receive(0)==-EMSGSIZE && stats.stats.rx_dropped==1);
    prepared=0;
    for(int bit=11;bit<=13;bit++) assert(receive(BIT(bit))==-EBADMSG);
    assert(stats.stats.rx_dropped==4 && !delivered);
    dev.running=false; assert(receive(0)==-ENETDOWN && stats.stats.rx_dropped==5);
    dev.running=true; service_error=-ENOENT;
    assert(receive(0)==-ENOENT && stats.stats.rx_dropped==6);
    service_error=0; assert(!receive(0) && delivered==1);
    assert(stats.stats.rx_packets==1 && stats.stats.rx_bytes==60 && stats.stats.rx_dropped==6);
    dev.running=false; assert(!receive(BIT(8)|BIT(11)) && omci==1 && delivered==1);
    assert(stats.stats.rx_packets==1 && stats.stats.rx_dropped==6);
    wan.pPonNetDev[3]=NULL; assert(receive(0)==-ENETDOWN && !live);
    return 0;
}
