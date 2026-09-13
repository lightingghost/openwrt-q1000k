#!/usr/bin/env python3
"""Generate a UML-only test of native PON lifetime with real netdevices/RCU."""
from pathlib import Path
import os
import re

repo = Path(__file__).resolve().parents[2]
eth = Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(
    repo.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))
header = Path(os.environ['Q1000K_PON_HEADER']) if 'Q1000K_PON_HEADER' in os.environ else (
    eth.parents[3] / 'include/linux/soc/airoha/airoha_pon.h')
print(r'''
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/bitfield.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#ifndef CONFIG_UML
#error This test is for a disposable UML guest only.
#endif
#define AIROHA_NUM_TX_RING 32
#define AIROHA_MAX_RX_SIZE 16128
''')
print(header.read_text())
print(r'''
/* Only hardware-owner storage and DMA submission are fixtures. net_device,
 * packet queues, RTNL, RCU, skb allocation and stop/unregister are real.
 */
struct airoha_eth;
struct airoha_qdma { struct airoha_eth *eth; };
struct airoha_gdm_dev {
    struct airoha_eth *eth;
    struct airoha_qdma __rcu *qdma;
    struct airoha_pon __rcu *pon;
    u64 pon_generation;
    atomic_t pon_tx_pending;
    bool pon_port;
};
struct airoha_gdm_port { struct airoha_gdm_dev *devs[1]; };
struct airoha_eth { struct airoha_qdma qdma[2]; struct airoha_gdm_port *ports[4]; };
static const struct net_device_ops airoha_netdev_ops;
bool airoha_pon_qdma_busy(struct airoha_qdma *qdma);
void airoha_pon_stop(struct airoha_gdm_dev *dev);
void airoha_pon_netdev_uninit(struct net_device *netdev);
u64 airoha_pon_generation(struct airoha_gdm_dev *dev);
void airoha_pon_rx(struct airoha_gdm_dev *dev, struct sk_buff *skb, const u32 words[4], u64 generation);
void airoha_pon_tx_wake(struct airoha_gdm_dev *dev);
void airoha_pon_tx_get(struct airoha_pon *pon);
void airoha_pon_tx_complete(struct airoha_pon *pon);
struct pending_tx { struct list_head list; struct airoha_pon *pon; struct sk_buff *skb; };
static LIST_HEAD(pending_tx);
static DEFINE_SPINLOCK(pending_lock);
static unsigned int pending_count;
static atomic_t hold_dma=ATOMIC_INIT(0);
static netdev_tx_t airoha_pon_dev_xmit(struct sk_buff *skb, struct net_device *netdev,
                                     u32 msg, struct airoha_pon *pon)
{
    struct pending_tx *tx=kmalloc(sizeof(*tx),GFP_ATOMIC);
    RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "PON TX outside RCU");
    if(!tx) { dev_kfree_skb_any(skb); return NETDEV_TX_OK; }
    spin_lock_bh(&pending_lock);
    if(pending_count==64) {
        spin_unlock_bh(&pending_lock); kfree(tx); return NETDEV_TX_BUSY;
    }
    /* Model two descriptors completing after submission returns. */
    tx->pon=pon; tx->skb=skb;
    airoha_pon_tx_get(pon); airoha_pon_tx_get(pon);
    list_add_tail(&tx->list,&pending_tx); pending_count++;
    spin_unlock_bh(&pending_lock);
    return NETDEV_TX_OK;
}
static int dma_worker(void *unused)
{
    for(;;) {
        struct pending_tx *tx=NULL;
        spin_lock_bh(&pending_lock);
        if(!list_empty(&pending_tx) && (!atomic_read(&hold_dma) || kthread_should_stop())) {
            tx=list_first_entry(&pending_tx,struct pending_tx,list);
            list_del(&tx->list); pending_count--;
        }
        spin_unlock_bh(&pending_lock);
        if(tx) {
            dev_kfree_skb_any(tx->skb);
            airoha_pon_tx_complete(tx->pon); airoha_pon_tx_complete(tx->pon);
            kfree(tx);
        } else if(kthread_should_stop()) break;
        else msleep(1);
    }
    return 0;
}
''')
print('\n'.join(line for line in (eth / 'airoha_regs.h').read_text().splitlines()
                if re.match(r'#define QDMA_ETH_(?:TXMSG_|RXMSG_AGG_COUNT_MASK)', line)))
source = (eth / 'airoha_pon.c').read_text()
print(re.sub(r'^#include[^\n]*\n', '', source, flags=re.M))
print(r'''
static struct airoha_pon __rcu *active;
static struct airoha_eth eth;
static struct airoha_gdm_port port;
static struct net_device *lower;
static atomic_t rx_calls=ATOMIC_INIT(0), wake_calls=ATOMIC_INIT(0), detached_calls=ATOMIC_INIT(0);
static atomic_t live=ATOMIC_INIT(0), bad_calls=ATOMIC_INIT(0);
static void check_callback(void)
{
    RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "PON callback outside RCU");
    if (!atomic_read(&live))
        atomic_inc(&bad_calls);
}
static void rx(void *priv, struct sk_buff *skb, const struct airoha_pon_rx_meta *meta)
{
    check_callback(); atomic_inc(&rx_calls); dev_kfree_skb_any(skb);
}
static void tx_wake(void *priv) { check_callback(); atomic_inc(&wake_calls); }
static void detached(void *priv)
{
    ASSERT_RTNL(); atomic_inc(&detached_calls); atomic_set(&live, 0);
}
static const struct airoha_pon_ops ops={ .rx=rx, .tx_wake=tx_wake, .detached=detached };
static netdev_tx_t lower_xmit(struct sk_buff *skb, struct net_device *dev)
{
    dev_kfree_skb_any(skb); return NETDEV_TX_OK;
}
static int lower_open(struct net_device *dev) { netif_tx_start_all_queues(dev); return 0; }
static int lower_stop(struct net_device *dev)
{
    airoha_pon_stop(netdev_priv(dev)); netif_tx_disable(dev); return 0;
}
static const struct net_device_ops airoha_netdev_ops={
    .ndo_start_xmit=lower_xmit, .ndo_open=lower_open, .ndo_stop=lower_stop, .ndo_uninit=airoha_pon_netdev_uninit,
};
static int reader(void *unused)
{
    const struct airoha_pon_tx_meta tx={ .gem=0x1357,.channel=5,.queue=3,.cpu_queue=31,.omci=true };
    const u32 words[4]={0x40054108,0,0x01000000,0};
    struct airoha_gdm_dev *gdm=netdev_priv(lower);
    while (!kthread_should_stop()) {
        struct airoha_pon *pon;
        struct sk_buff *skb;
        rcu_read_lock();
        pon=rcu_dereference(active);
        if (pon) {
            skb=alloc_skb(64,GFP_ATOMIC);
            if (skb) {
                skb_put(skb,48);
                if (airoha_pon_xmit(pon,skb,&tx)==NETDEV_TX_BUSY)
                    dev_kfree_skb_any(skb);
            }
        }
        skb=alloc_skb(64,GFP_ATOMIC);
        if (skb) {
            skb_put(skb,48);
            airoha_pon_rx(gdm,skb,words,airoha_pon_generation(gdm));
        }
        airoha_pon_tx_wake(gdm);
        rcu_read_unlock();
        cond_resched();
    }
    return 0;
}
#define CHECK(expr) do { if (!(expr)) { \
    pr_err("Q1000K_PON_TRANSPORT_KERNEL_FAIL line=%d: %s\n",__LINE__,#expr); \
    ret=-EINVAL; goto out; } } while (0)
static int __init pon_transport_test_init(void)
{
    struct task_struct *task=NULL, *dma=NULL;
    struct airoha_pon *pon=NULL;
    struct airoha_gdm_dev *gdm;
    struct sk_buff *skb;
    const struct airoha_pon_tx_meta tx={ .gem=5,.channel=1,.cpu_queue=3 };
    bool registered=false;
    int ret=0,i;
    if (!strstr(init_utsname()->release,"-q1000k-pon-transport-test")) return -EPERM;
    lower=alloc_netdev_mqs(sizeof(*gdm),"pontest%d",NET_NAME_UNKNOWN,ether_setup,32,1);
    if (!lower) return -ENOMEM;
    lower->netdev_ops=&airoha_netdev_ops;
    eth_hw_addr_random(lower);
    gdm=netdev_priv(lower); gdm->eth=&eth; gdm->pon_port=true;
    eth.qdma[0].eth=eth.qdma[1].eth=&eth;
    rcu_assign_pointer(gdm->qdma,&eth.qdma[1]);
    eth.ports[1]=&port; port.devs[0]=gdm;
    ret=register_netdev(lower); if (ret) goto out; registered=true;
    rtnl_lock(); ret=dev_open(lower,NULL); rtnl_unlock(); CHECK(!ret);
    dma=kthread_run(dma_worker,NULL,"pon-dma-test");
    if(IS_ERR(dma)) { ret=PTR_ERR(dma); dma=NULL; goto out; }
    atomic_set(&hold_dma,1); atomic_set(&live,1);
    pon=airoha_pon_attach(lower,&ops,NULL);
    if(IS_ERR(pon)) { ret=PTR_ERR(pon); pon=NULL; goto out; }
    skb=alloc_skb(64,GFP_KERNEL); CHECK(skb); skb_put(skb,48);
    CHECK(airoha_pon_xmit(pon,skb,&tx)==NETDEV_TX_OK);
    CHECK(atomic_read(&gdm->pon_tx_pending)==2);
    CHECK(airoha_pon_quiesce(pon,0)==-ETIMEDOUT);
    CHECK(PTR_ERR(airoha_pon_attach(lower,&ops,NULL))==-EBUSY);
    CHECK(airoha_pon_quiesce(pon,1)==-ETIMEDOUT);
    atomic_set(&hold_dma,0);
    CHECK(!airoha_pon_quiesce(pon,1000) && !atomic_read(&gdm->pon_tx_pending));
    CHECK(!airoha_pon_quiesce(pon,0));
    airoha_pon_release(pon); pon=NULL;
    task=kthread_run(reader,NULL,"pon-transport-test");
    if (IS_ERR(task)) { ret=PTR_ERR(task); task=NULL; goto out; }
    for(i=0;i<100;i++) {
        atomic_set(&live,1);
        pon=airoha_pon_attach(lower,&ops,NULL);
        if(IS_ERR(pon)) { ret=PTR_ERR(pon); pon=NULL; goto out; }
        rcu_assign_pointer(active,pon);
        msleep(1);
        if (i&1) {
            rtnl_lock(); dev_close(lower); rtnl_unlock();
            CHECK(!rcu_access_pointer(gdm->pon));
        }
        rcu_assign_pointer(active,NULL);
        synchronize_rcu(); /* Stop borrowed TX handle users before release. */
        CHECK(!airoha_pon_quiesce(pon,1000));
        airoha_pon_release(pon); pon=NULL;
        atomic_set(&live,0);
        if (i&1) {
            rtnl_lock(); ret=dev_open(lower,NULL); rtnl_unlock(); CHECK(!ret);
        }
    }
    kthread_stop(task); task=NULL;
    CHECK(atomic_read(&rx_calls)>0 && atomic_read(&wake_calls)>0);
    CHECK(atomic_read(&detached_calls)==50 && !atomic_read(&bad_calls));
    atomic_set(&live,1);
    pon=airoha_pon_attach(lower,&ops,NULL);
    if(IS_ERR(pon)) { ret=PTR_ERR(pon); pon=NULL; goto out; }
    atomic_set(&hold_dma,1);
    skb=alloc_skb(64,GFP_KERNEL); CHECK(skb); skb_put(skb,48);
    CHECK(airoha_pon_xmit(pon,skb,&tx)==NETDEV_TX_OK);
    CHECK(atomic_read(&gdm->pon_tx_pending)==2);
    /* Unregister with a still-owned handle must not hang on a netdev ref. */
    unregister_netdev(lower); registered=false;
    CHECK(!rcu_access_pointer(pon->netdev) && atomic_read(&detached_calls)==51);
    CHECK(airoha_pon_quiesce(pon,0)==-ETIMEDOUT);
    airoha_pon_release(pon); pon=NULL;
    /* Callback storage is no longer live; only descriptor refs remain. */
    atomic_set(&hold_dma,0);
    kthread_stop(dma); dma=NULL;
    CHECK(!atomic_read(&gdm->pon_tx_pending) && !atomic_read(&bad_calls));
out:
    if(task) kthread_stop(task);
    rcu_assign_pointer(active,NULL);
    synchronize_rcu();
    if(pon) airoha_pon_release(pon);
    if(registered) unregister_netdev(lower);
    if(dma) kthread_stop(dma);
    free_netdev(lower);
    if(!ret) pr_info("Q1000K_PON_TRANSPORT_KERNEL_PASS cycles=100 drain_timeout_retry=pass late_dma=pass rx=%d wake=%d detach=%d\n",
        atomic_read(&rx_calls),atomic_read(&wake_calls),atomic_read(&detached_calls));
    return ret;
}
static void __exit pon_transport_test_exit(void) {}
module_init(pon_transport_test_init);
module_exit(pon_transport_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K native PON transport lifetime test");
''')
