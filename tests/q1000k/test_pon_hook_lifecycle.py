#!/usr/bin/env python3
"""Exercise production hook writers against concurrent host RCU readers."""
from pathlib import Path
import re
import unittest
from test_pon_hooks import BSP
from pon_test_utils import run_c


def function(source, name):
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    match = re.search(r'^(?:static inline |__IMEM )?(?:int|void|ecnt_ret_val) ' +
                      name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


class PonHookLifecycleTests(unittest.TestCase):
    def test_unregistration_drains_readers_before_node_reuse(self):
        header = (BSP / 'include/ecnt_hook/ecnt_hook.h').read_text()
        types = header[header.index('typedef enum {'):header.index(
            '/************************************************************************', header.index('struct ecnt_hook_ops {'))]
        source = (BSP / 'core/ecnt_hook.c').read_text(errors='replace')
        production = ''.join(function(source, name) for name in [
            'ecnt_iterate', '__ECNT_HOOK', 'ecnt_hook_is_registered',
            'set_ecnt_hookfn_execute_or_not', 'get_ecnt_hookfn', 'show_all_ecnt_hookfn',
            'ecnt_register_hook', 'ecnt_unregister_hook', 'ecnt_ops_unregister',
            'ecnt_unregister_hooks', 'ecnt_register_hooks', 'ecnt_hook_init'])
        # Exercise the module entry point: manually calling the core helper
        # masked the missing module_init in the original port.
        metadata = (Path(__file__).resolve().parents[2] /
                    'package/kernel/airoha-pon/src/bsp/module/ecnt_hook_meta.c').read_text()
        production += re.sub(r'^#include[^\n]*\n', '', metadata, flags=re.M)
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#define __IMEM
#define __init
#define __exit
#define MODULE_DESCRIPTION(...)
#define MODULE_LICENSE(...)
#define module_init(fn) static int (*module_load)(void)=fn
#define module_exit(fn) static void (*module_unload)(void)=fn
#define U32_MAX UINT32_MAX
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ECNT_MAX_SUBTYPE 8
#define ECNT_REGISTER_FAIL -1
#define ECNT_REGISTER_SUCCESS 0
#define printk(...) ((void)0)
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_RELAXED)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELAXED)
/* Atomic links model RCU publication; readers do not take the writer mutex. */
struct list_head { _Atomic(struct list_head *) next,prev; };
#define entry(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_entry_rcu(p,t,m) entry(p,t,m)
#define list_for_each_entry(p,h,m) \
    for((p)=entry((h)->next,__typeof__(*(p)),m); &(p)->m!=(h); \
        (p)=entry((p)->m.next,__typeof__(*(p)),m))
#define list_for_each_entry_rcu list_for_each_entry
#define list_for_each_entry_continue_rcu(p,h,m) \
    for((p)=entry((p)->m.next,__typeof__(*(p)),m); &(p)->m!=(h); \
        (p)=entry((p)->m.next,__typeof__(*(p)),m))
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h; h->prev=h; }
static _Thread_local int reader_depth,writer_held;
static pthread_mutex_t ecnt_hook_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t grace_lock=PTHREAD_RWLOCK_INITIALIZER;
static atomic_bool grace_waiting;
static void mutex_lock(pthread_mutex_t *m) {
    assert(!reader_depth && !writer_held); pthread_mutex_lock(m); writer_held=1;
}
static void mutex_unlock(pthread_mutex_t *m) {
    assert(writer_held); writer_held=0; pthread_mutex_unlock(m);
}
static void rcu_read_lock(void) {
    assert(!writer_held); if(!reader_depth++) pthread_rwlock_rdlock(&grace_lock);
}
static void rcu_read_unlock(void) {
    assert(reader_depth); if(!--reader_depth) pthread_rwlock_unlock(&grace_lock);
}
static void synchronize_rcu(void) {
    /* This assertion detects the old unregister-by-ID self-deadlock. */
    assert(!reader_depth && writer_held);
    atomic_store(&grace_waiting,true);
    pthread_rwlock_wrlock(&grace_lock); pthread_rwlock_unlock(&grace_lock);
}
static void list_add_rcu(struct list_head *n,struct list_head *prev) {
    assert(writer_held);
    struct list_head *next=prev->next;
    n->next=next; n->prev=prev; next->prev=n; prev->next=n;
}
static void list_del_rcu(struct list_head *n) {
    assert(writer_held);
    struct list_head *prev=n->prev,*next=n->next;
    assert(prev && next); prev->next=next; next->prev=prev;
    /* Keep the forward link intact until the last old reader has left. */
}
''' + types + r'''
struct list_head ecnt_hooks[ECNT_NUM_MAINTYPE][ECNT_MAX_SUBTYPE];
static unsigned int hook_id;
struct ecnt_data { int calls; };
static atomic_bool blocking,entered,allow_return,removed,registered;
static ecnt_ret_val callback(struct ecnt_data *d) {
    assert(reader_depth); d->calls++;
    if(atomic_load(&blocking)) {
        atomic_store(&entered,true);
        while(!atomic_load(&allow_return)) sched_yield();
    }
    return ECNT_CONTINUE;
}
''' + production + r'''
static struct ecnt_hook_ops node={.maintype=ECNT_QDMA_WAN,.subtype=0,
    .hookfn=callback,.is_execute=1,.priority=10,.name="pon"};
static void *reader(void *p) {
    struct ecnt_data d={0};
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==ECNT_CONTINUE && d.calls==1);
    return NULL;
}
static void *remove_node(void *by_id) {
    if(by_id) assert(ecnt_ops_unregister(node.maintype,node.subtype,node.hook_id)==1);
    else ecnt_unregister_hook(&node);
    atomic_store(&removed,true); return NULL;
}
static void *register_node(void *p) {
    assert(!ecnt_register_hook(&node));
    /* A registration may return before the remover thread records its return,
     * but it must never return while the old reader still owns the node.
     */
    assert(atomic_load(&allow_return)); atomic_store(&registered,true); return NULL;
}
static void drain_case(bool by_id) {
    pthread_t r,u,a;
    atomic_store(&blocking,true); atomic_store(&entered,false);
    atomic_store(&allow_return,false); atomic_store(&removed,false);
    atomic_store(&registered,false); atomic_store(&grace_waiting,false);
    assert(!ecnt_register_hook(&node));
    unsigned old_id=node.hook_id;
    assert(!pthread_create(&r,NULL,reader,NULL));
    while(!atomic_load(&entered)) sched_yield();
    assert(!pthread_create(&u,NULL,remove_node,by_id ? &node : NULL));
    while(!atomic_load(&grace_waiting)) sched_yield();
    assert(!atomic_load(&removed) && node.list.next);
    assert(!ecnt_hook_is_registered(node.maintype,node.subtype));
    assert(!pthread_create(&a,NULL,register_node,NULL));
    /* Completion is impossible until the old callback is released. */
    assert(!atomic_load(&registered));
    atomic_store(&allow_return,true);
    assert(!pthread_join(r,NULL) && !pthread_join(u,NULL) && !pthread_join(a,NULL));
    assert(atomic_load(&registered) && node.hook_id>old_id);
    assert(ecnt_hook_is_registered(node.maintype,node.subtype));
    atomic_store(&blocking,false);
    ecnt_unregister_hook(&node);
    assert(!node.list.next && !node.list.prev);
}
static void *duplicate_registration(void *p) {
    *(int *)p=ecnt_register_hook(&node); return NULL;
}
int main(void) {
    struct ecnt_data data={0};
    struct ecnt_hook_ops zero={0},bad={.hookfn=callback,.maintype=UINT32_MAX};
    assert(!module_load());
    for(unsigned int i=0;i<ECNT_NUM_MAINTYPE;i++)
        for(unsigned int j=0;j<ECNT_MAX_SUBTYPE;j++) {
            assert(ecnt_hooks[i][j].next==&ecnt_hooks[i][j]);
            assert(ecnt_hooks[i][j].prev==&ecnt_hooks[i][j]);
        }
    ecnt_unregister_hook(NULL); ecnt_unregister_hook(&zero); ecnt_unregister_hook(&bad);
    assert(ecnt_register_hook(NULL)==-1 && ecnt_register_hook(&zero)==-1 && ecnt_register_hook(&bad)==-1);
    assert(!ecnt_ops_unregister(UINT32_MAX,0,1) && !ecnt_ops_unregister(0,UINT32_MAX,1));
    assert(!set_ecnt_hookfn_execute_or_not(UINT32_MAX,0,0,1));
    assert(!set_ecnt_hookfn_execute_or_not(0,UINT32_MAX,0,1));
    assert(!get_ecnt_hookfn(UINT32_MAX,0) && !get_ecnt_hookfn(0,UINT32_MAX));
    assert(show_all_ecnt_hookfn()==1);
    assert(!ecnt_register_hook(&node));
    assert(ecnt_register_hook(&node)==-1);
    assert(set_ecnt_hookfn_execute_or_not(node.maintype,0,node.hook_id,0));
    assert(!ecnt_hook_is_registered(node.maintype,0));
    assert(__ECNT_HOOK(node.maintype,0,&data)==ECNT_HOOK_ERROR && !data.calls);
    assert(set_ecnt_hookfn_execute_or_not(node.maintype,0,node.hook_id,1));
    assert(get_ecnt_hookfn(node.maintype,0));
    assert(ecnt_ops_unregister(node.maintype,0,node.hook_id)==1);
    assert(!ecnt_ops_unregister(node.maintype,0,node.hook_id));
    ecnt_unregister_hook(&node);
    /* Partial batch registration must remove exactly the completed prefix. */
    struct ecnt_hook_ops batch[2]={0}; batch[0].hookfn=callback;
    assert(ecnt_register_hooks(batch,2)==-1 && !batch[0].list.next);
    for(int i=0;i<50;i++) { drain_case(false); drain_case(true); }
    for(int i=0;i<100;i++) {
        pthread_t a,b; int ra=99,rb=99;
        assert(!pthread_create(&a,NULL,duplicate_registration,&ra));
        assert(!pthread_create(&b,NULL,duplicate_registration,&rb));
        assert(!pthread_join(a,NULL) && !pthread_join(b,NULL));
        assert((!ra && rb==-1) || (!rb && ra==-1));
        ecnt_unregister_hook(&node);
    }
    hook_id=U32_MAX; assert(ecnt_register_hook(&node)==-1 && !node.list.next);
    module_unload();
    /* Reload allocates fresh zeroed static storage before module init. */
    memset(ecnt_hooks,0,sizeof(ecnt_hooks)); hook_id=0;
    assert(!module_load());
    assert(!ecnt_register_hook(&node));
    ecnt_unregister_hook(&node); module_unload();
    return 0;
}
''', flags=['-pthread', '-Wno-sign-compare'])


if __name__ == '__main__':
    unittest.main()
