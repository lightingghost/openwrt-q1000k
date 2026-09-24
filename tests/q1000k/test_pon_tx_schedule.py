#!/usr/bin/env python3
"""Exercise the real PON worker's scheduling across DMA completion races."""
from pathlib import Path
import os
import re
import unittest
from pon_test_utils import run_c

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/src/q1000k_transport.c'


def function(source, name):
    start = source.index('static void ' + name + '(')
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class PonTxScheduleTests(unittest.TestCase):
    def test_completion_races_and_budget_rescheduling(self):
        source = Path(os.environ.get('Q1000K_TX_SOURCE', SOURCE)).read_text()
        budget = re.search(r'#define Q1000K_TX_BUDGET\s+\d+', source).group()
        diagnostic = (SOURCE.parent.parent / 'inc/common/q1000k_dhcp6_diag.h').read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
''' + diagnostic + r'''
typedef int netdev_tx_t;
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 1
#define READ_ONCE(x) (x)
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
static unsigned long jiffies;
struct list_head { struct list_head *next, *prev; };
static void list_init(struct list_head *l) { l->next=l->prev=l; }
static bool list_empty(struct list_head *l) { return l->next==l; }
static void list_add(struct list_head *n,struct list_head *h) {
    n->next=h->next; n->prev=h; h->next->prev=n; h->next=n;
}
static void list_del(struct list_head *n) { n->prev->next=n->next; n->next->prev=n->prev; }
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_first_entry(p,t,m) container_of((p)->next,t,m)
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; unsigned long delay; };
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define system_unbound_wq NULL
/* Linux queue_delayed_work leaves an already pending deadline alone;
 * mod_delayed_work moves that deadline, including when work is running.
 */
static bool queue_delayed_work(void *q,struct delayed_work *w,unsigned long delay) {
    if(w->pending) return false;
    w->pending=true; w->delay=delay; return true;
}
static bool mod_delayed_work(void *q,struct delayed_work *w,unsigned long delay) {
    bool was=w->pending; w->pending=true; w->delay=delay; return was;
}
typedef struct { bool held; } spinlock_t;
static void spin_lock_bh(spinlock_t *l) { assert(!l->held); l->held=true; }
static void spin_unlock_bh(spinlock_t *l) { assert(l->held); l->held=false; }
struct mutex { bool held; };
static void mutex_lock(struct mutex *l) { assert(!l->held); l->held=true; }
static void mutex_unlock(struct mutex *l) { assert(l->held); l->held=false; }
struct sk_buff { int id; };
struct net_device { int refs; };
struct airoha_pon_tx_meta { bool omci; uint64_t epoch; unsigned gem; };
struct q1000k_tx_packet {
    struct list_head list; struct sk_buff *skb; struct net_device *origin;
    struct airoha_pon_tx_meta meta; uint64_t auth_epoch;
    unsigned long expires; bool deferred, dhcp6;
    struct q6d_sample dhcp6_sample;
};
struct q1000k_transport {
    void *pon; spinlock_t lock; struct mutex auth_lock;
    uint64_t auth_epoch; struct list_head packets; struct delayed_work work;
    unsigned count; bool active, omci_paused;
};
static unsigned freed, submissions;
static unsigned diagnostic_calls, diagnostic_expired;
static void q1000k_dhcp6_record(enum q6d_stage stage,
                              const struct q6d_sample *sample, int ret) {
    assert(stage==Q6D_TX_NATIVE && sample);
    diagnostic_calls++;
    if(ret==-ETIME) diagnostic_expired++;
}
static void dev_kfree_skb_any(struct sk_buff *s) { freed++; free(s); }
static void dev_put(struct net_device *d) { assert(d->refs); d->refs--; }
#define kfree free
static int airoha_pon_prepare_tx(void *p,struct airoha_pon_tx_meta *m) { return 0; }
static void q1000k_native_wake(void *priv);
static int busy, completion_during_submit;
static netdev_tx_t airoha_pon_xmit(void *p,struct sk_buff *skb,struct airoha_pon_tx_meta *m) {
    struct q1000k_transport *t=p;
    assert(!t->lock.held); submissions++;
    if(completion_during_submit) q1000k_native_wake(t);
    if(busy) return NETDEV_TX_BUSY;
    dev_kfree_skb_any(skb); return NETDEV_TX_OK;
}
''' + budget + '\n' + function(source, 'q1000k_tx_work') + '\n' +
              function(source, 'q1000k_native_wake') + r'''
static void init(struct q1000k_transport *t,unsigned count) {
    *t=(struct q1000k_transport){}; t->pon=t; t->active=true;
    list_init(&t->packets);
    for(unsigned i=0;i<count;i++) {
        struct q1000k_tx_packet *p=calloc(1,sizeof(*p));
        p->skb=calloc(1,sizeof(*p->skb)); p->expires=1000;
        p->dhcp6=true;
        list_add(&p->list,&t->packets); t->count++;
    }
}
static void run(struct q1000k_transport *t) {
    t->work.pending=false; q1000k_tx_work(&t->work.work);
}
int main(void) {
    struct q1000k_transport t;
    /* Completion between a stopped-queue check and BUSY handling must
     * retain its immediate wake, rather than wait an extra timer tick.
     */
    init(&t,1); busy=1; completion_during_submit=1; run(&t);
    assert(t.count==1 && t.work.pending && t.work.delay==0);
    busy=0; completion_during_submit=0; run(&t); assert(!t.count);
    /* No completion: avoid a tight retry loop and retain the fallback. */
    init(&t,1); busy=1; run(&t);
    assert(t.count==1 && t.work.pending && t.work.delay==1);
    /* Completion after arming that fallback must expedite it too. */
    q1000k_native_wake(&t); assert(t.work.delay==0);
    busy=0; run(&t); assert(!t.count);
    /* A busy backlog yields after the budget, without adding a tick. */
    init(&t,Q1000K_TX_BUDGET+1); run(&t);
    assert(t.count==1 && t.work.pending && t.work.delay==0);
    run(&t); assert(!t.count && !t.work.pending);
    /* Shutdown discards queued work without rescheduling. */
    init(&t,1); t.active=false; run(&t);
    assert(!t.count && !t.work.pending);
    assert(freed==Q1000K_TX_BUDGET+4);
    assert(diagnostic_calls==submissions+1 && diagnostic_expired==1);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
