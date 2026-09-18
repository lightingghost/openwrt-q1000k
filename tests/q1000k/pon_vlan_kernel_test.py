#!/usr/bin/env python3
"""Real Linux skb tag edits; no optical drivers or fake skb primitives."""
from pathlib import Path
import re
from test_pon_vlan import MAC, vlan_types, vlan_source
print(r'''
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <linux/unaligned.h>
#include <linux/utsname.h>
#ifndef CONFIG_UML
#error Disposable UML kernel only
#endif
''')
print(vlan_types() + vlan_source())
source = (MAC / 'src/q1000k_services.c').read_text()
source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
for name in ('qs_frame', 'qs_rewrite'):
    m = re.search(r'^static int ' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert m
    end, depth = m.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    print(source[m.start():end])
print(r'''
static int run_case(const struct omci_service_config *s, const u8 *wire,
                    unsigned int len, const u8 *expected, unsigned int expected_len,
                    bool offloaded)
{
    struct q1000k_vlan_program p;
    struct q1000k_vlan_frame in, out;
    struct sk_buff *original, *tail, *skb;
    unsigned int split, room;
    u8 bytes[128];
    int ret;
    ret = q1000k_vlan_compile(s, &p);
    if (ret) return ret;
    for (room = 0; room <= 32; room += 8) for (split = 0; split <= len; split++) {
        original=alloc_skb(room+len+32,GFP_KERNEL);
        tail=alloc_skb(len,GFP_KERNEL);
        if (!original || !tail) { kfree_skb(original); kfree_skb(tail); return -ENOMEM; }
        skb_reserve(original,room);
        skb_put_data(original,wire,split);
        skb_put_data(tail,wire+split,len-split);
        skb_shinfo(original)->frag_list=tail;
        original->len+=tail->len; original->data_len+=tail->len; original->truesize+=tail->truesize;
        original->protocol=htons(get_unaligned_be16(wire+12));
        skb_reset_mac_header(original);
        skb_set_network_header(original,ETH_HLEN);
        if (offloaded) __vlan_hwaccel_put_tag(original,htons(0x8100),5<<13);
        skb=skb_clone(original,GFP_KERNEL);
        if (!skb) { kfree_skb(original); return -ENOMEM; }
        ret=qs_frame(skb,&in);
        if (!ret) ret=q1000k_vlan_apply(&p,true,&in,&out);
        if (!ret) ret=qs_rewrite(skb,&in,&out);
        if (!ret && (skb->len!=expected_len || skb_vlan_tag_present(skb) ||
                     skb_mac_header(skb)!=skb->data ||
                     skb_copy_bits(skb,0,bytes,expected_len) || memcmp(bytes,expected,expected_len))) ret=-EBADMSG;
        if (!ret && (skb_copy_bits(original,0,bytes,len) || memcmp(bytes,wire,len) ||
                     skb_vlan_tag_present(original)!=offloaded)) ret=-EUCLEAN;
        if (!ret) ret=qs_frame(skb,&in);
        if (!ret) ret=q1000k_vlan_apply(&p,false,&in,&out);
        if (!ret) ret=qs_rewrite(skb,&in,&out);
        if (!ret && !offloaded && (skb->len!=len || skb_copy_bits(skb,0,bytes,len) || memcmp(bytes,wire,len))) ret=-ESTALE;
        if (!ret && offloaded && (skb->len!=len+4 || skb_copy_bits(skb,0,bytes,len+4) ||
            memcmp(bytes,wire,12) || get_unaligned_be16(bytes+12)!=0x8100 ||
            get_unaligned_be16(bytes+14)!=(5<<13) || memcmp(bytes+16,wire+12,len-12))) ret=-ESTALE;
        kfree_skb(skb); kfree_skb(original);
        if (ret) { pr_err("VLAN skb split=%u room=%u offload=%d ret=%d\n",split,room,offloaded,ret); return ret; }
    }
    return 0;
}
static int __init pon_vlan_init(void)
{
    struct omci_service_config s={.vlan_treatment_valid=true,.vlan_input_tpid=0x88a8,.vlan_output_tpid=0x88a8,
        .vlan_rule={.filter_outer_pbit=15,.filter_inner_pbit=15,.treat_outer_pbit=15,
            .treat_inner_pbit=0,.treat_inner_vid=123,.treat_inner_tpid_dei=4}};
    u8 plain[80], tagged[84], priority[84], translated[84], stacked[88];
    unsigned int i;
    int ret;
    if (!IS_ENABLED(CONFIG_UML) || !strstr(utsname()->release,"-q1000k-pon-vlan-test")) return -ENODEV;
    for (i=0;i<sizeof(plain);i++) plain[i]=i*7;
    plain[12]=8; plain[13]=6;
    memcpy(tagged,plain,12); put_unaligned_be16(0x8100,tagged+12); put_unaligned_be16(123,tagged+14);
    memcpy(tagged+16,plain+12,sizeof(plain)-12);
    ret=run_case(&s,plain,sizeof(plain),tagged,sizeof(tagged),false);
    if (ret) return ret;
    /* Captured AT&T untagged -> VID 122, mode 2: output TPID and DEI 0
     * despite an absent input DEI. Exercise real nonlinear/cloned skbs.
     */
    s.vlan_rule.treat_inner_tpid_dei=2; s.vlan_rule.treat_inner_vid=122;
    s.vlan_output_tpid=0x8100; put_unaligned_be16(122,tagged+14);
    ret=run_case(&s,plain,sizeof(plain),tagged,sizeof(tagged),false);
    if (ret) return ret;
    s.vlan_rule.treat_inner_tpid_dei=4; s.vlan_rule.treat_inner_vid=123;
    s.vlan_output_tpid=0x88a8; put_unaligned_be16(123,tagged+14);
    memcpy(priority,tagged,sizeof(priority)); put_unaligned_be16(5<<13,priority+14);
    memcpy(translated,tagged,sizeof(translated)); put_unaligned_be16((5<<13)|123,translated+14);
    s.vlan_rule.filter_inner_pbit=8; s.vlan_rule.filter_inner_vid=0;
    s.vlan_rule.tags_to_remove=1; s.vlan_rule.treat_inner_pbit=8;
    ret=run_case(&s,priority,sizeof(priority),translated,sizeof(translated),false);
    if (ret) return ret;
    ret=run_case(&s,plain,sizeof(plain),translated,sizeof(translated),true);
    if (ret) return ret;
    memcpy(stacked,priority,12); put_unaligned_be16(0x88a8,stacked+12); put_unaligned_be16((5<<13)|123,stacked+14);
    memcpy(stacked+16,priority+12,sizeof(priority)-12);
    s.vlan_rule.tags_to_remove=0; s.vlan_rule.treat_inner_tpid_dei=6;
    ret=run_case(&s,priority,sizeof(priority),stacked,sizeof(stacked),false);
    if (ret) return ret;
    pr_info("Q1000K_PON_VLAN_KERNEL_PASS\n");
    return 0;
}
static void __exit pon_vlan_exit(void) {}
module_init(pon_vlan_init);
module_exit(pon_vlan_exit);
MODULE_LICENSE("GPL");
''')
