#!/usr/bin/env python3
"""Exercise actual block release against surviving and unrelated flow owners."""
import unittest
from test_pon_ppe import ETH, function
from pon_test_utils import run_c


class PonFlowOwnerTests(unittest.TestCase):
    def test_release_retires_only_owned_flows_and_preserves_iterator(self):
        source = (ETH / 'airoha_ppe.c').read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
const char *__asan_default_options(void) { return "detect_leaks=0"; }
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(n) struct list_head n={&n,&n}
static void list_del(struct list_head *n) { n->prev->next=n->next; n->next->prev=n->prev; }
static void list_add_tail(struct list_head *n,struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_for_each_entry_safe(p,n,h,m) \
    for(p=container_of((h)->next,__typeof__(*p),m), \
        n=container_of(p->m.next,__typeof__(*p),m); &p->m!=(h); \
        p=n,n=container_of(n->m.next,__typeof__(*n),m))
struct rhash_head { struct rhash_head *next; };
struct airoha_flow_table_entry {
    struct rhash_head node; const void *block_owner;
    struct list_head pon_list; unsigned int slot;
};
struct table { struct rhash_head *head; };
struct airoha_ppe { unsigned int retired; bool bound[8]; };
struct airoha_eth { struct table flow_table; struct airoha_ppe *ppe; };
struct rhashtable_iter { struct table *table; struct rhash_head *current; bool retry; };
static bool flow_offload_mutex, walking;
static int airoha_flow_table_params;
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define PTR_ERR(p) ((intptr_t)(p))
static void mutex_lock(bool *p) { assert(!*p); *p=true; }
static void mutex_unlock(bool *p) { assert(*p); *p=false; }
static void rhashtable_walk_enter(struct table *t,struct rhashtable_iter *i) {
    assert(flow_offload_mutex); *i=(struct rhashtable_iter){.table=t,.retry=true};
}
static void rhashtable_walk_start(struct rhashtable_iter *i) { (void)i; walking=true; }
static void *rhashtable_walk_next(struct rhashtable_iter *i) {
    assert(walking);
    if(i->retry) { i->retry=false; return (void *)(intptr_t)-EAGAIN; }
    /* Like the kernel iterator, next() dereferences the previous node. */
    i->current=i->current ? i->current->next : i->table->head;
    return i->current ? container_of(i->current,struct airoha_flow_table_entry,node) : NULL;
}
static void rhashtable_walk_stop(struct rhashtable_iter *i) { (void)i; walking=false; }
static void rhashtable_walk_exit(struct rhashtable_iter *i) { (void)i; assert(!walking); }
static void rhashtable_remove_fast(struct table *t,struct rhash_head *n,int params) {
    (void)params; struct rhash_head **p=&t->head;
    while(*p!=n) { assert(*p); p=&(*p)->next; }
    *p=n->next;
}
static void airoha_ppe_foe_flow_remove_entry(struct airoha_ppe *ppe,struct airoha_flow_table_entry *e) {
    assert(flow_offload_mutex && ppe->bound[e->slot]);
    ppe->bound[e->slot]=false; ppe->retired++;
    list_del(&e->pon_list); e->pon_list.next=e->pon_list.prev=&e->pon_list;
}
static void kfree(void *p) { assert(!walking); free(p); }
''' + function(source, 'airoha_ppe_release_flow_block') + r'''
int main(void) {
    int owner_a,owner_b;
    struct airoha_ppe ppe={}; struct airoha_eth eth={.ppe=&ppe};
    LIST_HEAD(active);
    const void *owners[]={&owner_a,&owner_b,&owner_a,NULL,&owner_b,&owner_a};
    for(unsigned int n=0;n<6;n++) {
        struct airoha_flow_table_entry *e=calloc(1,sizeof(*e));
        e->slot=n; e->block_owner=owners[n]; ppe.bound[n]=true;
        e->node.next=eth.flow_table.head; eth.flow_table.head=&e->node;
        list_add_tail(&e->pon_list,&active);
    }
    airoha_ppe_release_flow_block(&eth,NULL); assert(!ppe.retired);
    airoha_ppe_release_flow_block(&eth,&owner_a); assert(ppe.retired==3);
    for(unsigned int n=0;n<6;n++) assert(ppe.bound[n]==(owners[n]!=&owner_a));
    airoha_ppe_release_flow_block(&eth,&owner_a); assert(ppe.retired==3);
    airoha_ppe_release_flow_block(&eth,&owner_b); assert(ppe.retired==5);
    assert(ppe.bound[3] && eth.flow_table.head && !eth.flow_table.head->next);
    struct airoha_flow_table_entry *last=container_of(eth.flow_table.head,struct airoha_flow_table_entry,node);
    assert(!last->block_owner); free(last);
    return 0;
}
''', flags=['-fsanitize=address'])


if __name__ == '__main__':
    unittest.main()
