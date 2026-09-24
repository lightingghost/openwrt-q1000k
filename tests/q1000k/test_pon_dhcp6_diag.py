#!/usr/bin/env python3
"""Bounded diagnostic parser: real wire layout, truncation and exclusions."""
from pathlib import Path
import unittest
from pon_test_utils import run_c

ROOT = Path(__file__).resolve().parents[2]


class Dhcp6DiagnosticTests(unittest.TestCase):
    def test_headers_bounds_and_non_dhcp_exclusions(self):
        header = (ROOT / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/inc/common/q1000k_dhcp6_diag.h').read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
/* LSan cannot inspect tasks inside ptrace sandboxes; ASan bounds stay on. */
const char *__asan_default_options(void) { return "detect_leaks=0"; }
''' + header + r'''
static void be16(u8 *p, unsigned int n) { p[0]=n>>8; p[1]=n; }
static unsigned int frame(u8 *p, unsigned int tags, unsigned int ext, bool tx)
{
    unsigned int i, off=14;
    memset(p,0,256);
    be16(p+12,tags ? 0x8100 : 0x86dd);
    for(i=0;i<tags;i++) {
        be16(p+off,122+i); be16(p+off+2,i+1<tags ? 0x88a8 : 0x86dd); off+=4;
    }
    p[off]=0x60; be16(p+off+4,ext*8+9); p[off+6]=ext ? 0 : 17; off+=40;
    for(i=0;i<ext;i++) { p[off]=i+1<ext ? 60 : 17; off+=8; }
    be16(p+off,tx ? 546 : 547); be16(p+off+2,tx ? 547 : 546);
    be16(p+off+4,9); p[off+8]=tx ? 1 : 2;
    return off+9;
}
int main(void)
{
    u8 p[256]; struct q6d_sample s, before; struct q6d_l2 l2;
    unsigned int tags, ext, tx, n, i, off;
    for(tags=0;tags<=2;tags++) for(ext=0;ext<=4;ext++) for(tx=0;tx<=1;tx++) {
        n=frame(p,tags,ext,tx);
        assert(q6d_parse(p,n,n,&s));
        assert(q6d_parse_l2(p,n,&l2) && l2.proto==0x86dd && l2.tags==tags);
        assert(l2.outer==(tags ? 0x8100 : 0x86dd));
        assert(s.tags==tags && s.tx==tx && s.type==(tx ? 1 : 2) && s.length==n && s.gem==65535);
        for(i=0;i<tags;i++) assert(s.vlan[i]==122+i);
        for(i=0;i<n;i++) {
            u8 *shortbuf=malloc(i ? i : 1);
            memcpy(shortbuf,p,i); memset(&s,0xa5,sizeof(s)); before=s;
            assert(!q6d_parse(shortbuf,i,n,&s));
            assert(!memcmp(&s,&before,sizeof(s))); free(shortbuf);
        }
        assert(!q6d_parse(p,n,n-1,&s));
    }
    n=frame(p,3,0,1); assert(!q6d_parse(p,n,n,&s));
    n=frame(p,0,5,1); assert(!q6d_parse(p,n,n,&s));
    n=frame(p,0,0,1); off=54;
    p[14]=0x40; assert(!q6d_parse(p,n,n,&s)); p[14]=0x60;
    p[20]=44; assert(!q6d_parse(p,n,n,&s)); p[20]=17;
    be16(p+off,53); assert(!q6d_parse(p,n,n,&s)); be16(p+off,546);
    be16(p+off+4,8); assert(!q6d_parse(p,n,n,&s));
    be16(p+off+4,10); assert(!q6d_parse(p,n,n,&s)); be16(p+off+4,9);
    p[off+8]=0; assert(!q6d_parse(p,n,n,&s)); p[off+8]=1;
    be16(p+18,10); assert(!q6d_parse(p,n,n,&s));
    n=frame(p,0,1,1); p[55]=255; assert(!q6d_parse(p,n,n,&s));
    /* Trailing Ethernet padding and bounded prefix of a larger packet work. */
    n=frame(p,1,0,0); assert(q6d_parse(p,n,n+20,&s));
    be16(p+22,150); be16(p+62,150);
    assert(q6d_parse(p,Q6D_HEADER_BYTES,208,&s));
    n=frame(p,2,0,0); be16(p+20,0x0800);
    assert(q6d_parse_l2(p,22,&l2) && l2.outer==0x8100 && l2.proto==0x0800 && l2.tags==2);
    assert(l2.vlan[0]==122 && l2.vlan[1]==123);
    assert(!q6d_parse(p,n,n,&s));
    return 0;
}
''', flags=['-fsanitize=address'])

    def test_receive_result_survives_consumed_skb(self):
        root = ROOT / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
        header = (root / 'inc/common/q1000k_dhcp6_diag.h').read_text()
        source = (root / 'src/q1000k_transport.c').read_text()
        receive = source[source.index('static void q1000k_native_rx('):source.index('static void q1000k_native_wake(')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
const char *__asan_default_options(void) { return "detect_leaks=0"; }
''' + header + r'''
struct sk_buff { unsigned int len; u8 data[63]; };
struct airoha_pon_rx_meta { u32 words[4]; u16 gem; bool omci; };
struct q1000k_transport { bool active; int (*receive)(void *,unsigned int,struct sk_buff *,unsigned int); };
#define READ_ONCE(x) (x)
#define dev_kfree_skb_any(skb) free(skb)
static unsigned int events, consumed;
static bool q1000k_dhcp6_sample(const struct sk_buff *skb,struct q6d_sample *sample) {
    return q6d_parse(skb->data,skb->len,skb->len,sample);
}
static void q4d_receive(const struct sk_buff *skb,const struct airoha_pon_rx_meta *meta) {}
static void q1000k_dhcp6_record(enum q6d_stage stage,const struct q6d_sample *s,int result) {
    assert(s->type==2 && s->gem==1023 && !s->tx);
    if(stage==Q6D_RX_PRE) assert(!consumed && !result);
    else { assert(stage==Q6D_RX_CONSUMER && consumed && result==-7); }
    events++;
}
static int consume(void *words,unsigned int n,struct sk_buff *skb,unsigned int len) {
    assert(n==16 && len==63); free(skb); consumed++; return -7;
}
''' + receive + r'''
int main(void) {
    struct sk_buff *skb=calloc(1,sizeof(*skb));
    struct q1000k_transport t={.active=true,.receive=consume};
    struct airoha_pon_rx_meta m={.gem=1023};
    skb->len=63; skb->data[12]=0x86; skb->data[13]=0xdd; skb->data[14]=0x60;
    skb->data[19]=9; skb->data[20]=17;
    skb->data[54]=2; skb->data[55]=0x23; skb->data[56]=2; skb->data[57]=0x22;
    skb->data[59]=9; skb->data[62]=2;
    q1000k_native_rx(&t,skb,&m);
    assert(events==2 && consumed==1);
    t.active=false; skb=calloc(1,sizeof(*skb)); q1000k_native_rx(&t,skb,&m);
    assert(events==2 && consumed==1);
    return 0;
}
''', flags=['-fsanitize=address'])
