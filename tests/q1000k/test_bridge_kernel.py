#!/usr/bin/env python3
"""Compile actual prepared-kernel error paths and PPE flag updates with fixtures."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

REPO=Path(__file__).resolve().parents[2]

def block(source,start):
    start=source.index(start);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

def compile_run(source):
    with tempfile.TemporaryDirectory(prefix='q1000k-l2-kernel-') as temp:
        c=Path(temp)/'test.c';exe=Path(temp)/'test';c.write_text(source)
        subprocess.run(['cc','-std=gnu11','-Wall','-Werror','-Wno-unused-parameter',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

class KernelBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        roots=list(REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*'))
        assert len(roots)==1,'Prepare one AN7581 kernel first'
        cls.kernel=roots[0]

    def test_refragmentation_packet_has_exactly_one_owner(self):
        source=(self.kernel/'net/bridge/netfilter/nf_conntrack_bridge.c').read_text()
        fn=block(source,'nf_ct_bridge_refrag(struct sk_buff')
        compile_run(r'''
#include <assert.h>
#include <stdint.h>
#define htons(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
typedef uint16_t __be16;
#define ETH_P_IP 0x800
#define ETH_P_IPV6 0x86dd
#define NF_DROP 0
#define NF_ACCEPT 1
#define NF_STOLEN 2
struct net {};
struct sock {};
struct nf_bridge_frag_data {};
struct sk_buff { int frag_max_size, freed, parsed, saved, pulled; };
struct nf_hook_state { struct net *net; struct sock *sk; };
#define BR_INPUT_SKB_CB(skb) (skb)
static int inner_offset, inner_proto, warnings;
static int nf_ct_bridge_inner(struct sk_buff *skb, struct nf_bridge_frag_data *data, __be16 *proto) {
    skb->parsed++; *proto=htons(inner_proto); return inner_offset;
}
static void nf_ct_bridge_frag_save(struct sk_buff *skb,struct nf_bridge_frag_data *data) { skb->saved++; }
static void __skb_pull(struct sk_buff *skb,int offset) { skb->pulled=offset; }
static void skb_reset_network_header(struct sk_buff *skb) {}
static void kfree_skb(struct sk_buff *skb) { assert(!skb->freed);skb->freed++; }
#define WARN_ON_ONCE(x) do { if(x) warnings++; } while(0)
static void nf_br_ip_fragment(struct net *net,struct sock *sk,struct sk_buff *skb,struct nf_bridge_frag_data *data,void *output) { kfree_skb(skb); }
#define nf_br_ip6_fragment nf_br_ip_fragment
static unsigned int
'''+fn+r'''
int main(void) {
    struct nf_hook_state state={0};
    int protocols[]={ETH_P_IP,ETH_P_IPV6,0x1234};
    for(int frag=0;frag<=1;frag++) for(int offset=-1;offset<=8;offset++) for(int p=0;p<3;p++) {
        struct sk_buff skb={.frag_max_size=frag};inner_offset=offset;inner_proto=protocols[p];warnings=0;
        int verdict=nf_ct_bridge_refrag(&skb,&state,0);
        if(!frag) { assert(verdict==NF_ACCEPT && !skb.parsed && !skb.freed);continue; }
        if(offset<0 || p==2) {
            assert(verdict==NF_DROP && !skb.freed);
            kfree_skb(&skb); /* nf_hook_slow owns NF_DROP */
        } else { assert(verdict==NF_STOLEN && skb.freed==1 && skb.saved==1); }
        assert(skb.freed==1);
    }
}
''')

    def test_ppe_preserves_unrelated_bits_and_ipv6_pppoe_id(self):
        root=self.kernel/'drivers/net/ethernet/airoha'
        source=(root/'airoha_ppe.c').read_text()
        headers=(root/'airoha_eth.h').read_text()
        definitions='\n'.join(re.findall(r'^#define (?:AIROHA_FOE_IB1_BIND_TTL|AIROHA_FOE_MAC_SMAC_ID)\s+.*$',headers,re.M))
        ipv6=re.search(r'PPE_PKT_TYPE_IPV6_ROUTE_5T = (\d+)',headers).group(1)
        code=block(source,'if (keep_ttl)')
        compile_run(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((~0U<<(l)) & (~0U>>(31-(h))))
#define FIELD_PREP(mask,v) (((v)<<__builtin_ctz(mask))&(mask))
'''+definitions+'\n#define PPE_PKT_TYPE_IPV6_ROUTE_5T '+ipv6+r'''
struct entry { uint32_t ib1; struct { struct {uint32_t src_mac_hi;} l2; } ipv6; };
static struct entry apply(struct entry hwe,bool keep_ttl,int offload_type) {
'''+code+r'''
return hwe;
}
int main(void) {
    for(int type=0;type<8;type++) for(int keep=0;keep<2;keep++) for(uint32_t n=0;n<100;n++) {
        struct entry before={.ib1=n*0x1234567U,.ipv6.l2.src_mac_hi=n*0x9876543U};
        struct entry after=apply(before,keep,type);
        assert(after.ib1==(keep ? before.ib1 & ~AIROHA_FOE_IB1_BIND_TTL : before.ib1));
        uint32_t expected=before.ipv6.l2.src_mac_hi;
        if(keep && type==PPE_PKT_TYPE_IPV6_ROUTE_5T) expected=(expected&~AIROHA_FOE_MAC_SMAC_ID)|FIELD_PREP(AIROHA_FOE_MAC_SMAC_ID,0xf);
        assert(after.ipv6.l2.src_mac_hi==expected);
    }
}
''')

if __name__=='__main__': unittest.main()
