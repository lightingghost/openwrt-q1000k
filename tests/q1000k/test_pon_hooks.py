#!/usr/bin/env python3
"""Execute production hook dispatch/wrappers with host list and provider fixtures."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
BSP = Path(os.environ['Q1000K_PON_BSP']) if 'Q1000K_PON_BSP' in os.environ else next(
    REPO.glob('build_dir/target-*/linux-airoha_an7581/airoha-pon-v2/airoha-pon/bsp'))


def function(source, name):
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    match = re.search(r'^(?:static inline |__IMEM )?(?:int|ecnt_ret_val) ' + name + r'\(', source, re.M)
    if not match:
        raise AssertionError('Missing production function ' + name)
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def run_c(source):
    with tempfile.TemporaryDirectory(prefix='q1000k-pon-hooks-') as tmp:
        c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
        c.write_text(source)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


class PonHookTests(unittest.TestCase):
    def test_dispatch_and_readiness(self):
        header = (BSP / 'include/ecnt_hook/ecnt_hook.h').read_text()
        types = header[header.index('typedef enum {'):header.index('/************************************************************************', header.index('struct ecnt_hook_ops {'))]
        core = (BSP / 'core/ecnt_hook.c').read_text(errors='replace')
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#define ECNT_MAX_SUBTYPE 8
#define __IMEM
#define READ_ONCE(x) (x)
struct list_head { struct list_head *next, *prev; };
#define entry(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_entry_rcu(p,t,m) entry(p,t,m)
#define list_for_each_entry_rcu(p,h,m) \
    for ((p)=entry((h)->next,__typeof__(*(p)),m); &(p)->m!=(h); \
         (p)=entry((p)->m.next,__typeof__(*(p)),m))
#define list_for_each_entry_continue_rcu(p,h,m) \
    for ((p)=entry((p)->m.next,__typeof__(*(p)),m); &(p)->m!=(h); \
         (p)=entry((p)->m.next,__typeof__(*(p)),m))
static int locked, calls;
static void rcu_read_lock(void) { assert(!locked); locked=1; }
static void rcu_read_unlock(void) { assert(locked); locked=0; }
''' + types + r'''
struct list_head ecnt_hooks[ECNT_NUM_MAINTYPE][ECNT_MAX_SUBTYPE];
struct ecnt_data { int verdict; };
static ecnt_ret_val callback(struct ecnt_data *d) {
    assert(locked); calls++; return d->verdict;
}
static void add(struct list_head *h, struct ecnt_hook_ops *o) {
    o->list.next=h; o->list.prev=h->prev;
    h->prev->next=&o->list; h->prev=&o->list;
}
''' + ''.join(function(core, n) for n in ['ecnt_iterate', '__ECNT_HOOK', 'ecnt_hook_is_registered']) + r'''
int main(void) {
    unsigned int i,j;
    struct ecnt_hook_ops first={.is_execute=1,.hookfn=callback};
    struct ecnt_hook_ops second={.is_execute=1,.hookfn=callback};
    struct ecnt_data d={.verdict=ECNT_CONTINUE};
    for(i=0;i<ECNT_NUM_MAINTYPE;i++) for(j=0;j<ECNT_MAX_SUBTYPE;j++) {
        struct list_head *h=&ecnt_hooks[i][j]; h->next=h; h->prev=h;
        assert(!ecnt_hook_is_registered(i,j));
        assert(__ECNT_HOOK(i,j,&d)==ECNT_HOOK_ERROR);
    }
    assert(!ecnt_hook_is_registered(UINT32_MAX,0));
    assert(!ecnt_hook_is_registered(0,UINT32_MAX));
    assert(__ECNT_HOOK(UINT32_MAX,0,&d)==ECNT_HOOK_ERROR);
    assert(__ECNT_HOOK(0,UINT32_MAX,&d)==ECNT_HOOK_ERROR);
    add(&ecnt_hooks[ECNT_QDMA_WAN][0],&first);
    add(&ecnt_hooks[ECNT_QDMA_WAN][0],&second);
    assert(ecnt_hook_is_registered(ECNT_QDMA_WAN,0));
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==ECNT_CONTINUE && calls==2);
    first.is_execute=second.is_execute=0;
    assert(!ecnt_hook_is_registered(ECNT_QDMA_WAN,0));
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==ECNT_HOOK_ERROR && calls==2);
    first.is_execute=second.is_execute=1;
    d.verdict=ECNT_RETURN;
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==ECNT_RETURN && calls==3);
    d.verdict=ECNT_RETURN_DROP;
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==ECNT_RETURN_DROP && calls==4);
    assert(first.info.drop_num==1);
    d.verdict=-EIO;
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==-EIO && calls==5);
    /* Detach the provider: dispatch/readiness must no longer visit it. */
    ecnt_hooks[ECNT_QDMA_WAN][0].next=&ecnt_hooks[ECNT_QDMA_WAN][0];
    ecnt_hooks[ECNT_QDMA_WAN][0].prev=&ecnt_hooks[ECNT_QDMA_WAN][0];
    assert(!ecnt_hook_is_registered(ECNT_QDMA_WAN,0));
    assert(__ECNT_HOOK(ECNT_QDMA_WAN,0,&d)==ECNT_HOOK_ERROR && calls==5);
    assert(!locked);
    return 0;
}
''')

    def test_qdma_fe_errors_outputs_and_packet_ownership(self):
        qdma = (BSP / 'include/ecnt_hook/ecnt_hook_qdma.h').read_text()
        fe = (BSP / 'include/ecnt_hook/ecnt_hook_fe.h').read_text()
        for source in (qdma, fe):
            # Catch legacy reinitialization that would erase the error default.
            self.assertNotRegex(source, r'memset\s*\(\s*&in_data')
            self.assertNotRegex(source, r'struct (?:ECNT_QDMA_Data|ecnt_fe_data) in_data\s*;')
        functions = ''.join(function(qdma, n) for n in [
            'QDMA_API_INIT', 'QDMA_API_TX_DMA_MODE', 'QDMA_API_RX_DMA_MODE',
            'QDMA_API_TRANSMIT_PACKETS', 'QDMA_API_GET_TX_QOS_WEIGHT'])
        functions += ''.join(function(fe, n) for n in [
            'FE_API_SET_CHANNEL_ENABLE', 'FE_API_GET_CHN_RLS', 'FE_API_SET_MBI_ARB_RST'])
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned int uint;
typedef unsigned char unchar;
typedef int QDMA_Mode_t;
typedef int QDMA_TxQosWeightType_t;
typedef int QDMA_TxQosWeightScale_t;
typedef int FE_Gdma_Sel_t;
typedef int FE_TXRX_Sel_t;
typedef int FE_Enable_t;
typedef struct { int cookie; } QDMA_InitCfg_t;
typedef struct { uint txmsg0,txmsg1,txmsg2; } QDMA_TxMsg_Ex_T;
struct sk_buff { int owner; };
struct port_info { int cookie; };
struct ecnt_data;
enum { ECNT_QDMA_WAN, ECNT_FE, ECNT_DRIVER_API, ECNT_FE_API };
enum { QDMA_FUNCTION_INIT, QDMA_FUNCTION_TX_DMA_MODE, QDMA_FUNCTION_RX_DMA_MODE,
       QDMA_FUNCTION_TRANSMIT_PACKETS, QDMA_FUNCTION_GET_TX_QOS_WEIGHT,
       FE_SET_CHANNEL_ENABLE, FE_GET_CHN_RLS, FE_SET_MBI_ARB_RST };
struct ECNT_QDMA_Data {
    int function_id, retValue;
    union {
        QDMA_InitCfg_t *pInitCfg;
        QDMA_Mode_t mode;
        struct { struct sk_buff *skb; QDMA_TxMsg_Ex_T *pTxMsg; struct port_info *pMacInfo; } qdma_transmit;
        struct { int weightBase, weightScale; } qdma_tx_qos;
    } qdma_private;
};
struct ecnt_fe_data {
    int function_id,retValue,gdm_sel,txrx_sel;
    uint channel,reg_val;
    union { FE_Enable_t enable; } api_data;
};
/* Modes: absent, unhandled, provider error, success, dispatcher error. */
static int mode;
static QDMA_InitCfg_t cfg={42};
static struct sk_buff skb={1};
static struct port_info port={23};
static int __ECNT_HOOK(unsigned int main, unsigned int sub, struct ecnt_data *raw) {
    int *result;
    if (main==ECNT_QDMA_WAN) {
        struct ECNT_QDMA_Data *q=(void *)raw;
        assert(sub==ECNT_DRIVER_API);
        result=&q->retValue;
        switch(q->function_id) {
        case QDMA_FUNCTION_INIT: assert(q->qdma_private.pInitCfg==&cfg); break;
        case QDMA_FUNCTION_TX_DMA_MODE:
        case QDMA_FUNCTION_RX_DMA_MODE: assert(q->qdma_private.mode==1); break;
        case QDMA_FUNCTION_TRANSMIT_PACKETS:
            assert(q->qdma_private.qdma_transmit.skb==&skb);
            assert(q->qdma_private.qdma_transmit.pMacInfo==&port);
            assert(q->qdma_private.qdma_transmit.pTxMsg->txmsg0==0x12345678);
            assert(q->qdma_private.qdma_transmit.pTxMsg->txmsg1==0x87654321);
            assert(q->qdma_private.qdma_transmit.pTxMsg->txmsg2==0xffff);
            assert(skb.owner==1);
            if(mode==3) skb.owner=2;
            break;
        case QDMA_FUNCTION_GET_TX_QOS_WEIGHT:
            assert(!q->qdma_private.qdma_tx_qos.weightBase);
            assert(!q->qdma_private.qdma_tx_qos.weightScale);
            q->qdma_private.qdma_tx_qos.weightBase=7;
            q->qdma_private.qdma_tx_qos.weightScale=9;
            break;
        default: assert(0);
        }
    } else {
        struct ecnt_fe_data *f=(void *)raw;
        assert(main==ECNT_FE && sub==ECNT_FE_API && f->gdm_sel==2);
        result=&f->retValue;
        if(f->function_id==FE_SET_CHANNEL_ENABLE)
            assert(f->txrx_sel==1 && f->channel==31 && f->api_data.enable==1);
        else if(f->function_id==FE_GET_CHN_RLS) f->reg_val=0x9876;
        else assert(f->function_id==FE_SET_MBI_ARB_RST);
    }
    assert(*result==-EOPNOTSUPP);
    if(mode==0) return -1;
    if(mode==4) return -ENODEV;
    if(mode==2) *result=-EIO;
    if(mode==3) *result=0;
    return 1; /* ECNT_CONTINUE is allowed; only an explicit result is success. */
}
''' + functions + r'''
int main(void) {
    const int expected[]={-1,-EOPNOTSUPP,-EIO,0,-ENODEV};
    for(mode=0;mode<5;mode++) {
        int base=101,scale=102;
        uint val=0x5555;
        skb.owner=1;
        assert(QDMA_API_INIT(ECNT_QDMA_WAN,&cfg)==expected[mode]);
        assert(QDMA_API_TX_DMA_MODE(ECNT_QDMA_WAN,1)==expected[mode]);
        assert(QDMA_API_RX_DMA_MODE(ECNT_QDMA_WAN,1)==expected[mode]);
        assert(QDMA_API_TRANSMIT_PACKETS(ECNT_QDMA_WAN,&skb,0x12345678,0x87654321,&port)==expected[mode]);
        assert(skb.owner==(mode==3 ? 2 : 1));
        assert(QDMA_API_GET_TX_QOS_WEIGHT(ECNT_QDMA_WAN,&base,&scale)==expected[mode]);
        assert(base==(mode==3 ? 7 : 101) && scale==(mode==3 ? 9 : 102));
        assert(FE_API_SET_CHANNEL_ENABLE(2,1,31,1)==expected[mode]);
        assert(FE_API_SET_MBI_ARB_RST(2)==expected[mode]);
        assert(FE_API_GET_CHN_RLS(2,&val)==expected[mode]);
        assert(val==(mode==3 ? 0x9876 : 0x5555));
    }
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
