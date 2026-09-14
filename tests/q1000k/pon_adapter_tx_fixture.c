// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#define Q1000K_PON_IDENTITY
#define PWAN_IF_DATA 3
#define NETDEV_TX_OK 0
struct sk_buff { unsigned int len; bool freed; };
typedef struct { unsigned int netIdx; struct { unsigned long tx_packets,tx_bytes,tx_dropped; } stats; } PWAN_NetPriv_T;
struct net_device { PWAN_NetPriv_T priv; };
static int depth, result, calls, frees;
static bool ready;
static void rcu_read_lock(void) { assert(!depth); depth++; }
static void rcu_read_unlock(void) { assert(depth==1); depth--; }
static bool xpon_is_ready(void) { assert(depth); return ready; }
static void *netdev_priv(struct net_device *dev) { return &dev->priv; }
static void dev_kfree_skb_any(struct sk_buff *skb) { assert(depth && !skb->freed); skb->freed=true; frees++; }
static int q1000k_services_tx(struct sk_buff *skb) {
    assert(depth); calls++; if(!result) dev_kfree_skb_any(skb); return result;
}
/* PRODUCTION */
int main(void)
{
    struct net_device dev={.priv.netIdx=3}; struct sk_buff skb={.len=60};
    assert(!pwan_net_start_xmit(&skb,&dev) && skb.freed && !calls && !depth);
    ready=true; skb.freed=false;
    assert(!pwan_net_start_xmit(&skb,&dev) && skb.freed && calls==1 && !depth);
    assert(dev.priv.stats.tx_packets==1 && dev.priv.stats.tx_bytes==60);
    result=-ENOBUFS; skb.freed=false;
    assert(!pwan_net_start_xmit(&skb,&dev) && skb.freed && calls==2 && !depth);
    dev.priv.netIdx=1; skb.freed=false;
    assert(!pwan_net_start_xmit(&skb,&dev) && skb.freed && calls==2 && !depth);
    assert(frees==4 && dev.priv.stats.tx_dropped==3);
    return 0;
}
