// SPDX-License-Identifier: GPL-2.0-only
/* Real Linux workqueues/RCU/skbs; synthetic lower and native DMA provider. */
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/bitfield.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/soc/airoha/airoha_pon.h>
#include <net/net_namespace.h>
#ifndef CONFIG_UML
#error This test runs only in a disposable UML guest.
#endif

static bool fail_packet_alloc, fail_transport_alloc;
static void *test_kmalloc(size_t size, gfp_t flags)
{
    return READ_ONCE(fail_packet_alloc) ? NULL : kmalloc(size, flags);
}
static void *test_kzalloc(size_t size, gfp_t flags)
{
    return READ_ONCE(fail_transport_alloc) ? NULL : kzalloc(size, flags);
}
#pragma push_macro("kmalloc")
#pragma push_macro("kzalloc")
#undef kmalloc
#undef kzalloc
#define kmalloc test_kmalloc
#define kzalloc test_kzalloc
/* PRODUCTION */
#pragma pop_macro("kzalloc")
#pragma pop_macro("kmalloc")

struct airoha_pon {
    const struct airoha_pon_ops *ops;
    void *priv;
    bool connected;
    u64 epoch[32];
    u8 closed[32];
    u32 retiring;
};
static struct airoha_pon __rcu *fake_current;
static DEFINE_SPINLOCK(fake_tx_lock);
static struct sk_buff_head fake_dma;
static struct net_device *lower_dev;
static bool fake_busy, wake_during_busy, fail_attach, detach_during_attach, hold_dma;
static bool early_rx, record_order;
static int quiesce_error, channel_quiesce_error, tx_channel_error;
static atomic_t allocations, destructions, accepted, received, busy_calls, violations;
static unsigned int order[256];
static unsigned int expected_omci_mark;
static bool block_omci_submission;
static DECLARE_COMPLETION(omci_submission_entered);
static DECLARE_COMPLETION(omci_submission_release);
static DECLARE_COMPLETION(omci_epoch_closed);
static int omci_epoch_result;
static int close_auth_thread(void *unused)
{
    omci_epoch_result=q1000k_transport_set_auth_epoch(0);
    kthread_complete_and_exit(&omci_epoch_closed,0);
}
static u32 test_word0 = 0xfedcu << 14 | 27u << 3 | 5u;
static u32 test_word1 = 0x7f2007dfu | 27u << 15;

static void packet_destroy(struct sk_buff *skb)
{
    atomic_inc(&destructions);
}

static struct sk_buff *packet_new(unsigned int id)
{
    struct sk_buff *skb = alloc_skb(64, GFP_KERNEL);

    if (!skb)
        return NULL;
    memset(skb_put(skb, 60), 0x5a, 60);
    memset(skb->cb, 0xa5, sizeof(skb->cb));
    skb->mark = id;
    skb->dev = lower_dev;
    skb->destructor = packet_destroy;
    atomic_inc(&allocations);
    return skb;
}

static int receive_packet(void *msg, unsigned int msg_len,
                          struct sk_buff *skb, unsigned int len)
{
    u32 *words = msg;

    if (msg_len != 16 || len != 60 || skb->len != 60 || words[0] != BIT(8))
        atomic_inc(&violations);
    words[0] = 0; /* Must not overwrite the native metadata passed to RX. */
    atomic_inc(&received);
    kfree_skb(skb);
    return -EIO; /* Callback owns skb even when its return value is an error. */
}

static void fake_receive(void)
{
    struct airoha_pon_rx_meta meta = { .words = { BIT(8), 0, 0x01000000, 0 }, .omci = true };
    struct sk_buff *skb = packet_new(0);
    struct airoha_pon *pon;

    if (!skb)
        return;
    rcu_read_lock();
    pon = rcu_dereference(fake_current);
    if (pon)
        pon->ops->rx(pon->priv, skb, &meta);
    else
        kfree_skb(skb);
    if (meta.words[0] != BIT(8))
        atomic_inc(&violations);
    rcu_read_unlock();
}

struct airoha_pon *airoha_pon_attach(struct net_device *dev,
                                     const struct airoha_pon_ops *ops, void *priv)
{
    struct airoha_pon *pon;

    if (fail_attach)
        return ERR_PTR(-EAGAIN);
    pon = kzalloc(sizeof(*pon), GFP_KERNEL);
    if (!pon)
        return ERR_PTR(-ENOMEM);
    memset(pon->closed,0xff,sizeof(pon->closed));
    for(int i=0;i<32;i++) pon->epoch[i]=1;
    pon->ops = ops;
    pon->priv = priv;
    rtnl_lock();
    if (rcu_access_pointer(fake_current)) {
        rtnl_unlock();
        kfree(pon);
        return ERR_PTR(-EBUSY);
    }
    pon->connected = true;
    rcu_assign_pointer(fake_current, pon);
    if (early_rx)
        fake_receive();
    if (detach_during_attach) {
        rcu_assign_pointer(fake_current, NULL);
        synchronize_rcu();
        pon->connected = false;
        ops->detached(priv);
    }
    rtnl_unlock();
    return pon;
}

static void fake_detach_locked(struct airoha_pon *pon)
{
    ASSERT_RTNL();
    if (!pon->connected)
        return;
    rcu_assign_pointer(fake_current, NULL);
    synchronize_rcu();
    pon->connected = false;
    pon->ops->detached(pon->priv);
}

int airoha_pon_quiesce(struct airoha_pon *pon, unsigned int timeout_ms)
{
    if (timeout_ms != Q1000K_TX_DRAIN_MS)
        atomic_inc(&violations);
    rtnl_lock();
    fake_detach_locked(pon);
    rtnl_unlock();
    return quiesce_error;
}

void airoha_pon_release(struct airoha_pon *pon)
{
    rtnl_lock();
    fake_detach_locked(pon);
    rtnl_unlock();
    kfree(pon);
}

int airoha_pon_set_queue_close(struct airoha_pon *pon,u8 channel,u8 closed)
{
    if(channel>31) return -EINVAL;
    spin_lock_bh(&fake_tx_lock);
    if((pon->retiring & BIT(channel)) && closed!=255) {
        spin_unlock_bh(&fake_tx_lock); return -ESHUTDOWN;
    }
    if(closed & ~pon->closed[channel]) pon->epoch[channel]++;
    pon->closed[channel]=closed;
    spin_unlock_bh(&fake_tx_lock);
    return 0;
}
/* Native channel counts are tested in the transport guest. This fixture
 * supplies native return codes to check the adapter's RCU/lifetime wrapper.
 */
int airoha_pon_set_tx_channel(struct airoha_pon *pon,u8 channel,bool enabled)
{
    if(channel>31) return -EINVAL;
    RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "FE wrapper outside RCU");
    if(!pon->connected) return -ENODEV;
    /* The native guest tests FE register/admission behavior; this checks
     * argument forwarding and exact native error propagation. */
    WARN_ON(channel!=29 || !enabled);
    return tx_channel_error;
}
int airoha_pon_set_qos(struct airoha_pon *pon,u8 channel,const struct airoha_pon_qos *qos)
{
    RCU_LOCKDEP_WARN(rcu_read_lock_held(), "Sleepable QoS wrapper inside RCU");
    WARN_ON(irqs_disabled() || in_interrupt());
    if(channel>31 || !qos) return -EINVAL;
    if(!pon->connected) return -ENODEV;
    WARN_ON(channel!=29 || qos->mode!=1);
    msleep(1);
    return tx_channel_error;
}
int airoha_pon_get_qos(struct airoha_pon *pon,u8 channel,struct airoha_pon_qos *qos)
{
    RCU_LOCKDEP_WARN(rcu_read_lock_held(), "Sleepable QoS wrapper inside RCU");
    WARN_ON(irqs_disabled() || in_interrupt());
    if(channel>31 || !qos) return -EINVAL;
    if(!pon->connected) return -ENODEV;
    WARN_ON(channel!=29);
    msleep(1);
    if(tx_channel_error) return tx_channel_error;
    memset(qos,0,sizeof(*qos)); qos->mode=1; qos->byte_mode=true;
    return 0;
}
int airoha_pon_quiesce_channel(struct airoha_pon *pon,u8 channel)
{
    if(channel>31) return -EINVAL;
    spin_lock_bh(&fake_tx_lock);
    pon->retiring |= BIT(channel);
    if(pon->closed[channel]!=255) pon->epoch[channel]++;
    pon->closed[channel]=255;
    spin_unlock_bh(&fake_tx_lock);
    return channel_quiesce_error;
}
static int fake_lifecycle(struct airoha_pon *pon)
{
    RCU_LOCKDEP_WARN(rcu_read_lock_held(), "Sleepable lifecycle wrapper inside RCU");
    WARN_ON(irqs_disabled() || in_interrupt());
    if(!pon->connected) return -ENODEV;
    msleep(1);
    return tx_channel_error;
}
int airoha_pon_get_port_config(struct airoha_pon *pon,struct airoha_pon_port_config *config)
{
    int ret=fake_lifecycle(pon);
    if(!ret) { memset(config,0,sizeof(*config)); config->min_len=60; config->max_len=2000; }
    return ret;
}
int airoha_pon_configure_port(struct airoha_pon *pon,
        const struct airoha_pon_port_config *expected,const struct airoha_pon_port_config *config)
{
    WARN_ON(expected->min_len!=60 || config->max_len!=2000);
    return fake_lifecycle(pon);
}
int airoha_pon_pause(struct airoha_pon *pon,unsigned int timeout_ms)
{
    WARN_ON(timeout_ms!=750);
    return fake_lifecycle(pon);
}
int airoha_pon_resume(struct airoha_pon *pon)
{
    return fake_lifecycle(pon);
}
int airoha_pon_retire_fe(struct airoha_pon *pon,u8 channel)
{
    WARN_ON(channel!=29);
    return fake_lifecycle(pon);
}
int airoha_pon_drain_rx(struct airoha_pon *pon)
{
    return fake_lifecycle(pon);
}
int airoha_pon_reset_epoch(struct airoha_pon *pon)
{
    return fake_lifecycle(pon);
}
int airoha_pon_activate_rx(struct airoha_pon *pon,u32 channels)
{
    WARN_ON(channels!=BIT(29));
    return fake_lifecycle(pon);
}
int airoha_pon_get_queue_close(struct airoha_pon *pon,u8 channel,u8 *closed)
{
    if(channel>31 || !closed) return -EINVAL;
    spin_lock_bh(&fake_tx_lock);
    *closed=pon->closed[channel];
    spin_unlock_bh(&fake_tx_lock);
    return 0;
}
int airoha_pon_prepare_tx(struct airoha_pon *pon,struct airoha_pon_tx_meta *meta)
{
    int ret=0;
    if(meta->channel>31 || meta->queue>7) return -EINVAL;
    spin_lock_bh(&fake_tx_lock);
    if(pon->closed[meta->channel] & BIT(meta->queue)) ret=-ESHUTDOWN;
    else meta->epoch=pon->epoch[meta->channel];
    spin_unlock_bh(&fake_tx_lock);
    return ret;
}

netdev_tx_t airoha_pon_xmit(struct airoha_pon *pon, struct sk_buff *skb,
                            const struct airoha_pon_tx_meta *meta)
{
    netdev_tx_t ret = NETDEV_TX_OK;
    struct airoha_pon *active_pon;
    unsigned int i, n;

    /* A stalled call models submission already inside the adapter barrier.
     * Sleep before RCU/queue locks; no hardware or native DMA is executed. */
    if(meta->omci && READ_ONCE(block_omci_submission)) {
        complete(&omci_submission_entered);
        wait_for_completion(&omci_submission_release);
    }
    rcu_read_lock();
    active_pon = rcu_dereference(fake_current);
    if (active_pon != pon) {
        kfree_skb(skb);
        goto unlock;
    }
    spin_lock_bh(&fake_tx_lock);
    if ((pon->closed[meta->channel] & BIT(meta->queue)) ||
        pon->epoch[meta->channel] != meta->epoch) {
        kfree_skb(skb);
        goto unlock_queue;
    }
    if (READ_ONCE(fake_busy)) {
        atomic_inc(&busy_calls);
        if (READ_ONCE(wake_during_busy))
            pon->ops->tx_wake(pon->priv); /* Exercise the native lock order. */
        ret = NETDEV_TX_BUSY;
        goto unlock_queue;
    }
    if(meta->omci) {
        if(meta->gem!=17 || meta->channel || meta->queue || meta->cpu_queue ||
           meta->mic_index!=1 || skb->mark!=expected_omci_mark || skb->len!=60)
            atomic_inc(&violations);
    } else if (meta->gem != 0xfedc || meta->channel != 27 || meta->queue != 5 ||
        meta->cpu_queue || meta->mic_index || skb->len != 60) {
        atomic_inc(&violations);
    }
    for (i = 0; i < skb->len; i++)
        if (skb->data[i] != 0x5a)
            atomic_inc(&violations);
    for (i = 0; i < sizeof(skb->cb); i++)
        if (skb->cb[i] != 0xa5)
            atomic_inc(&violations);
    n = atomic_inc_return(&accepted) - 1;
    if (record_order && n < ARRAY_SIZE(order))
        order[n] = skb->mark;
    skb->dev = lower_dev; /* Native TX switches away from the submitting upper. */
    if (hold_dma)
        skb_queue_tail(&fake_dma, skb);
    else
        kfree_skb(skb);
unlock_queue:
    spin_unlock_bh(&fake_tx_lock);
unlock:
    rcu_read_unlock();
    return ret;
}

static unsigned int queued(void)
{
    struct q1000k_transport *transport;
    unsigned int count = 0;

    rcu_read_lock();
    transport = rcu_dereference(q1000k_current);
    if (transport) {
        spin_lock_bh(&transport->lock);
        count = transport->count;
        spin_unlock_bh(&transport->lock);
    }
    rcu_read_unlock();
    return count;
}

static int wait_empty(unsigned int timeout_ms)
{
    unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

    do {
        if (!queued())
            return 0;
        msleep(1);
    } while (time_before(jiffies, end));
    return -ETIMEDOUT;
}

#define CHECK(condition) do { if (!(condition)) { \
    pr_err("Q1000K_PON_ADAPTER_KERNEL_FAIL line=%d\n", __LINE__); return -EINVAL; \
} } while (0)

static int metadata_test(void)
{
    struct airoha_pon_tx_meta meta, before;
    unsigned int i, channel, queue;
    u32 w0, w1;

    for (channel = 0; channel < 32; channel++) for (queue = 0; queue < 8; queue++) {
        w0 = 0xffffu << 14 | channel << 3 | queue;
        w1 = 0x7f2007dfu | channel << 15;
        CHECK(!q1000k_tx_meta(w0, w1, &meta));
        CHECK(meta.gem == 65535 && meta.channel == channel && meta.queue == queue && !meta.cpu_queue);
    }
    CHECK(!q1000k_tx_meta(BIT(8) | BIT(30) | 17u << 14, 0xff2007ff, &meta));
    CHECK(meta.omci && meta.mic_index == 1 && meta.gem == 17 && !meta.channel);
    for (i = 0; i < 12; i++) {
        w0 = test_word0; w1 = test_word1;
        if (i < 5) w0 |= BIT(9 + i);
        if (i == 5) w0 |= BIT(31);
        if (i == 6) w0 |= BIT(30);
        if (i == 7) w1 |= BIT(31);
        if (i == 8) w1 ^= BIT(20);
        if (i == 9) w1 ^= BIT(24);
        if (i == 10) w1 |= BIT(11);
        if (i == 11) w1 ^= BIT(15);
        memset(&meta, 0xa5, sizeof(meta)); before = meta;
        CHECK(q1000k_tx_meta(w0, w1, &meta) == -EOPNOTSUPP);
        CHECK(!memcmp(&meta, &before, sizeof(meta)));
    }
    return 0;
}

static int producer_thread(void *unused)
{
    unsigned int i = 0;

    while (!kthread_should_stop()) {
        struct sk_buff *skb = packet_new(i++);

        if (skb && q1000k_transport_xmit(skb, test_word0, test_word1))
            kfree_skb(skb);
        fake_receive();
        cond_resched();
    }
    return 0;
}

static int upper_lifetime_test(void)
{
    struct net_device *upper;
    struct sk_buff *skb;
    int i, ret;

    upper = alloc_netdev(0, "qponupper0", NET_NAME_UNKNOWN, ether_setup);
    if (!upper) return -ENOMEM;
    upper->netdev_ops = lower_dev->netdev_ops;
    eth_hw_addr_random(upper);
    ret = register_netdev(upper);
    if (ret) { free_netdev(upper); return ret; }
    WRITE_ONCE(fake_busy, true);
    for (i = 0; i < 4; i++) {
        skb = packet_new(i);
        if (!skb) { ret = -ENOMEM; break; }
        skb->dev = upper;
        ret = q1000k_transport_xmit(skb, test_word0, test_word1);
        if (ret) { kfree_skb(skb); break; }
    }
    /* Core unregister waits for the queued skb's explicit device reference.
     * The worker needs no RTNL and expires every packet within its age bound.
     */
    unregister_netdev(upper);
    free_netdev(upper);
    if (!ret && queued()) ret = -EBUSY;
    return ret;
}

static int run_tests(void)
{
    struct sk_buff *skb;
    struct task_struct *producer;
    struct airoha_pon *pon;
    int i, ret, count, rx;
    u8 closed=0xa5;
    struct airoha_pon_qos qos={.mode=1},saved;
    struct airoha_pon_port_config config={.min_len=60,.max_len=2000};

    CHECK(!metadata_test());
    CHECK(!q1000k_transport_stop());
    CHECK(q1000k_transport_get_queue_close(27,&closed)==-ENODEV && closed==0xa5);
    CHECK(q1000k_transport_set_auth_epoch(1)==-ENODEV);
    CHECK(q1000k_transport_set_queue_close(27,0)==-ENODEV);
    CHECK(q1000k_transport_quiesce_channel(27)==-ENODEV);
    CHECK(q1000k_transport_set_tx_channel(29,true)==-ENODEV);
    CHECK(q1000k_transport_set_qos(29,&qos)==-ENODEV);
    CHECK(q1000k_transport_get_qos(29,&qos)==-ENODEV);
    CHECK(q1000k_transport_get_port_config(&config)==-ENODEV);
    CHECK(q1000k_transport_configure_port(&config,&config)==-ENODEV);
    CHECK(q1000k_transport_pause(750)==-ENODEV);
    CHECK(q1000k_transport_retire_fe(29)==-ENODEV);
    CHECK(q1000k_transport_resume()==-ENODEV);
    CHECK(q1000k_transport_drain_rx()==-ENODEV);
    CHECK(q1000k_transport_reset_epoch()==-ENODEV);
    CHECK(q1000k_transport_activate_rx(BIT(29))==-ENODEV);
    CHECK(q1000k_transport_start(NULL, receive_packet) == -EINVAL);
    CHECK(q1000k_transport_start("", receive_packet) == -EINVAL);
    CHECK(q1000k_transport_start("bad/name", receive_packet) == -EINVAL);
    CHECK(q1000k_transport_start("absent0", receive_packet) == -ENODEV);
    fail_transport_alloc = true;
    CHECK(q1000k_transport_start("qponlower0", receive_packet) == -ENOMEM);
    fail_transport_alloc = false; fail_attach = true;
    CHECK(q1000k_transport_start("qponlower0", receive_packet) == -EAGAIN);
    fail_attach = false; detach_during_attach = true;
    CHECK(q1000k_transport_start("qponlower0", receive_packet) == -ENODEV);
    detach_during_attach = false; early_rx = true;
    rx = atomic_read(&received);
    CHECK(!q1000k_transport_start("qponlower0", receive_packet));
    CHECK(atomic_read(&received) == rx); /* Early callback safely dropped. */
    CHECK(q1000k_transport_running());
    CHECK(q1000k_transport_start("qponlower0", receive_packet) == -EBUSY);
    fake_receive(); CHECK(atomic_read(&received) == rx + 1);
    early_rx = false;
    CHECK(!q1000k_transport_get_queue_close(27,&closed) && closed==255);
    CHECK(q1000k_transport_set_tx_channel(32,true)==-EINVAL);
    for(i=0;i<6;i++) {
        const int errors[]={0,-EAGAIN,-ESHUTDOWN,-EIO,-ETIMEDOUT,-EBUSY};
        tx_channel_error=errors[i];
        CHECK(q1000k_transport_set_tx_channel(29,true)==errors[i]);
        CHECK(q1000k_transport_set_qos(29,&qos)==errors[i]);
        CHECK(q1000k_transport_pause(750)==errors[i]);
        CHECK(q1000k_transport_retire_fe(29)==errors[i]);
        CHECK(q1000k_transport_resume()==errors[i]);
        CHECK(q1000k_transport_drain_rx()==errors[i]);
        CHECK(q1000k_transport_reset_epoch()==errors[i]);
        CHECK(q1000k_transport_activate_rx(BIT(29))==errors[i]);
        saved=qos;
        CHECK(q1000k_transport_get_qos(29,&qos)==errors[i]);
        CHECK(q1000k_transport_get_port_config(&config)==errors[i]);
        CHECK(q1000k_transport_configure_port(&config,&config)==errors[i]);
        CHECK(errors[i] ? !memcmp(&saved,&qos,sizeof(qos)) : qos.mode==1);
    }
    tx_channel_error=0;
    CHECK(q1000k_transport_quiesce_channel(32)==-EINVAL);
    channel_quiesce_error=-EAGAIN;
    CHECK(q1000k_transport_quiesce_channel(30)==-EAGAIN);
    CHECK(q1000k_transport_set_queue_close(30,0)==-ESHUTDOWN);
    channel_quiesce_error=-EIO;
    CHECK(q1000k_transport_quiesce_channel(30)==-EIO);
    channel_quiesce_error=0;
    CHECK(!q1000k_transport_quiesce_channel(30));
    skb=packet_new(1); CHECK(skb);
    CHECK(q1000k_transport_xmit(skb,test_word0,test_word1)==-ESHUTDOWN);
    CHECK(skb->len==60 && skb->data[0]==0x5a && skb->cb[0]==0xa5);
    kfree_skb(skb);
    CHECK(!q1000k_transport_set_queue_close(27,0));


    fail_packet_alloc = true;
    skb = packet_new(1); CHECK(skb);
    CHECK(q1000k_transport_xmit(skb, test_word0, test_word1) == -ENOMEM);
    CHECK(skb->len == 60 && skb->data[0] == 0x5a && skb->cb[0] == 0xa5);
    kfree_skb(skb); fail_packet_alloc = false;

    /* Saturate, receive a wake inside BUSY, then recover without another wake. */
    fake_busy = true; wake_during_busy = true; record_order = true;
    atomic_set(&accepted, 0);
    for (i = 0; i < Q1000K_TX_LIMIT; i++) {
        skb = packet_new(i); CHECK(skb);
        ret = q1000k_transport_xmit(skb, test_word0, test_word1);
        if (ret) kfree_skb(skb);
        CHECK(!ret);
    }
    CHECK(queued() == Q1000K_TX_LIMIT);
    skb = packet_new(128); CHECK(skb);
    CHECK(q1000k_transport_xmit(skb, test_word0, test_word1) == -ENOBUFS);
    CHECK(skb->len == 60 && skb->data[0] == 0x5a && skb->cb[0] == 0xa5);
    kfree_skb(skb); msleep(10);
    CHECK(atomic_read(&busy_calls) > 0);
    wake_during_busy = false; WRITE_ONCE(fake_busy, false);
    CHECK(!wait_empty(500)); CHECK(atomic_read(&accepted) == Q1000K_TX_LIMIT);
    for (i = 0; i < Q1000K_TX_LIMIT; i++) CHECK(order[i] == i);
    record_order = false;

    /* Reopen after BUSY must not refresh admission for old queued frames. */
    WRITE_ONCE(fake_busy,true); count=atomic_read(&accepted);
    for(i=0;i<10;i++) {
        skb=packet_new(i); CHECK(skb);
        ret=q1000k_transport_xmit(skb,test_word0,test_word1);
        if(ret) kfree_skb(skb);
        CHECK(!ret);
    }
    msleep(2);
    CHECK(!q1000k_transport_set_queue_close(27,255));
    CHECK(!q1000k_transport_set_queue_close(27,0));
    WRITE_ONCE(fake_busy,false);
    CHECK(!wait_empty(500) && atomic_read(&accepted)==count);
    skb=packet_new(0); CHECK(skb);
    ret=q1000k_transport_xmit(skb,test_word0,test_word1);
    if(ret) kfree_skb(skb);
    CHECK(!ret && !wait_empty(500) && atomic_read(&accepted)==count+1);

    /* Old authenticated OMCI retries are purged; data packets survive rekey. */
    CHECK(!q1000k_transport_set_queue_close(0,0));
    skb=packet_new(1); CHECK(skb);
    CHECK(q1000k_transport_xmit(skb,BIT(8)|(17U<<14),0xff2007ff)==-EKEYREJECTED);
    CHECK(q1000k_transport_xmit_omci(skb,17,1,0)==-EKEYREJECTED);
    CHECK(q1000k_transport_xmit_omci(skb,17,1,1)==-EKEYREJECTED);
    CHECK(skb->len==60 && skb->cb[0]==0xa5); kfree_skb(skb);
    for(unsigned int epoch=1;epoch<=20;epoch++) {
        unsigned int old_count=atomic_read(&accepted);
        CHECK(!q1000k_transport_set_auth_epoch(epoch));
        CHECK(!q1000k_transport_set_auth_epoch(epoch));
        WRITE_ONCE(fake_busy,true); wake_during_busy=true;
        for(unsigned int n=0;n<20;n++) {
            skb=packet_new(epoch); CHECK(skb);
            ret=(n&1) ? q1000k_transport_xmit(skb,test_word0,test_word1) :
                q1000k_transport_xmit_omci(skb,17,1,epoch);
            if(ret) kfree_skb(skb);
            CHECK(!ret);
        }
        msleep(2);
        CHECK(!q1000k_transport_set_auth_epoch(0));
        CHECK(q1000k_transport_set_auth_epoch(epoch)==-ESTALE);
        skb=packet_new(epoch); CHECK(skb);
        CHECK(q1000k_transport_xmit_omci(skb,17,1,epoch)==-EKEYREJECTED);
        kfree_skb(skb);
        wake_during_busy=false; WRITE_ONCE(fake_busy,false);
        CHECK(!wait_empty(500));
        CHECK(atomic_read(&accepted)==old_count+10);
    }
    CHECK(!q1000k_transport_set_auth_epoch(21)); expected_omci_mark=21;
    skb=packet_new(21); CHECK(skb);
    ret=q1000k_transport_xmit_omci(skb,17,1,21);
    if(ret) kfree_skb(skb);
    CHECK(!ret && !wait_empty(500));
    /* An in-progress submission cannot outlive the software close barrier. */
    WRITE_ONCE(block_omci_submission,true);
    reinit_completion(&omci_submission_entered);
    reinit_completion(&omci_submission_release);
    reinit_completion(&omci_epoch_closed);
    skb=packet_new(21); CHECK(skb);
    ret=q1000k_transport_xmit_omci(skb,17,1,21);
    if(ret) kfree_skb(skb);
    CHECK(!ret);
    count=wait_for_completion_timeout(&omci_submission_entered,HZ);
    if(!count) { complete(&omci_submission_release); CHECK(count); }
    producer=kthread_run(close_auth_thread,NULL,"pon-auth-close");
    if(IS_ERR(producer)) { complete(&omci_submission_release); CHECK(!IS_ERR(producer)); }
    msleep(5); count=completion_done(&omci_epoch_closed);
    complete(&omci_submission_release);
    wait_for_completion(&omci_epoch_closed);
    WRITE_ONCE(block_omci_submission,false);
    CHECK(!count && !omci_epoch_result && !wait_empty(500));
    CHECK(!q1000k_transport_set_auth_epoch(22)); expected_omci_mark=22;
    skb=packet_new(21); CHECK(skb);
    CHECK(q1000k_transport_xmit_omci(skb,17,1,21)==-EKEYREJECTED); kfree_skb(skb);
    skb=packet_new(22); CHECK(skb);
    ret=q1000k_transport_xmit_omci(skb,17,1,22);
    if(ret) kfree_skb(skb);
    CHECK(!ret && !wait_empty(500));

    /* Expiry bounds memory and time during a permanently full native ring. */
    fake_busy = true; count = atomic_read(&accepted);
    for (i = 0; i < 10; i++) {
        skb = packet_new(i); CHECK(skb);
        ret = q1000k_transport_xmit(skb, test_word0, test_word1);
        if (ret) kfree_skb(skb);
        CHECK(!ret);
    }
    CHECK(!wait_empty(Q1000K_TX_AGE_MS + 500));
    CHECK(atomic_read(&accepted) == count);
    CHECK(!upper_lifetime_test());

    /* Unsolicited lower detach closes ingress and purges software packets. */
    skb = packet_new(0); CHECK(skb);
    ret = q1000k_transport_xmit(skb, test_word0, test_word1);
    if (ret) kfree_skb(skb);
    CHECK(!ret);
    rtnl_lock(); pon = rcu_dereference_protected(fake_current, 1);
    fake_detach_locked(pon); rtnl_unlock();
    CHECK(!q1000k_transport_running()); CHECK(!wait_empty(500));
    closed=0xa5;
    CHECK(q1000k_transport_get_queue_close(27,&closed)==-ENODEV && closed==0xa5);
    CHECK(q1000k_transport_set_queue_close(27,0)==-ENODEV);
    CHECK(q1000k_transport_quiesce_channel(27)==-ENODEV);
    CHECK(q1000k_transport_set_tx_channel(29,true)==-ENODEV);
    CHECK(!q1000k_transport_stop()); CHECK(!q1000k_transport_stop());

    /* Accepted native DMA can outlive adapter stop after a reported timeout. */
    fake_busy = false; hold_dma = true; quiesce_error = -ETIMEDOUT;
    CHECK(!q1000k_transport_start("qponlower0", receive_packet));
    CHECK(!q1000k_transport_set_queue_close(27,0));
    skb = packet_new(0); CHECK(skb);
    ret = q1000k_transport_xmit(skb, test_word0, test_word1);
    if (ret) kfree_skb(skb);
    CHECK(!ret); CHECK(!wait_empty(500));
    CHECK(skb_queue_len(&fake_dma) == 1);
    CHECK(q1000k_transport_stop() == -ETIMEDOUT);
    CHECK(!rcu_access_pointer(fake_current) && !q1000k_transport_running());
    skb_queue_purge(&fake_dma); /* No adapter callbacks/private pointer retained. */
    hold_dma = false; quiesce_error = 0;

    /* Producers and RX race repeated attach/detach/stop; accepted skbs have one owner. */
    producer = kthread_run(producer_thread, NULL, "pon-adapter-producer");
    CHECK(!IS_ERR(producer));
    for (i = 0; i < 50; i++) {
        ret = q1000k_transport_start("qponlower0", receive_packet);
        if (ret) break;
        ret=q1000k_transport_set_queue_close(27,0);
        if(ret) break;
        WRITE_ONCE(fake_busy, i & 1);
        msleep(1);
        ret=q1000k_transport_set_queue_close(27,255);
        if(ret) break;
        ret=q1000k_transport_set_queue_close(27,0);
        if(ret) break;
        msleep(1);
        ret = q1000k_transport_stop();
        if (ret) break;
    }
    kthread_stop(producer);
    CHECK(!ret); CHECK(!q1000k_transport_stop());
    CHECK(!atomic_read(&violations));
    CHECK(atomic_read(&allocations) == atomic_read(&destructions));
    pr_info("Q1000K_PON_ADAPTER_KERNEL_PASS cycles=50 packets=%d busy=%d rx=%d\n",
            atomic_read(&allocations), atomic_read(&busy_calls), atomic_read(&received));
    return 0;
}

static netdev_tx_t unused_tx(struct sk_buff *skb, struct net_device *dev)
{
    kfree_skb(skb); return NETDEV_TX_OK;
}
static const struct net_device_ops test_ops = { .ndo_start_xmit = unused_tx };

static int __init pon_adapter_test_init(void)
{
    struct net_device *dev;
    int ret;

    if (!strstr(init_utsname()->release, "-q1000k-pon-adapter-test"))
        return -EPERM;
    skb_queue_head_init(&fake_dma);
    dev = alloc_netdev(0, "qponlower0", NET_NAME_UNKNOWN, ether_setup);
    if (!dev) return -ENOMEM;
    dev->netdev_ops = &test_ops;
    lower_dev = dev;
    eth_hw_addr_random(dev);
    ret = register_netdev(dev);
    if (ret) { free_netdev(dev); return ret; }
    ret = run_tests();
    q1000k_transport_stop();
    skb_queue_purge(&fake_dma);
    unregister_netdev(dev);
    free_netdev(dev);
    return ret;
}
static void __exit pon_adapter_test_exit(void) {}
module_init(pon_adapter_test_init);
module_exit(pon_adapter_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only native Q1000K packet adapter test");
