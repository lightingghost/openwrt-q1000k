#!/usr/bin/env python3
"""Generate a UML-only module that tests the production helpers in the kernel."""
from test_pon_crypto import BSP, function
from test_pon_auth import production as auth_production
from test_pon_key_exchange import key_source
from pathlib import Path

mac = BSP.parent / 'xpon-en757x/xpon_10g'
source = (mac / 'src/gpon/gpon_security.c').read_text(errors='replace')
production = ''.join(function(source, name) for name in [
    'gf_mulx', 'aes_128_cmac_vector', 'gpon_aes_cmac_encrypt',
    'gpon_aes_cmac_setup', 'gpon_aes_ecb_encrypt', 'gpon_aes_ecb_decrypt',
    'gpon_aes_ecb_setup', 'gpon_aes_cmac_key_free'])
print(r'''
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/random.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/err.h>
#include <crypto/skcipher.h>
#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#define AES_CMAC_KEY_LEN 16
#define AES_ECB_KEY_LEN 16
#define CMAC_TLEN 16
#define PON_MSG(level, ...) pr_err(__VA_ARGS__)
static DEFINE_SPINLOCK(gpon_cmac_lock);
static DEFINE_SPINLOCK(gpon_ecb_lock);
''')
for name in ['gpon_aes_cmac_encrypt', 'gpon_aes_cmac_setup', 'gpon_aes_ecb_encrypt',
             'gpon_aes_ecb_decrypt', 'gpon_aes_ecb_setup', 'gpon_aes_cmac_key_free']:
    print(function(source, name).split('{', 1)[0].strip() + ';')
print(production)
print(auth_production())
print(key_source().split("static int qkey_fifo_status")[0])
print(Path(__file__).with_name("pon_auth_vectors.h").read_text())
print(r'''
static const u8 key[]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
static const u8 message[]={
    0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
    0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
    0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10};
static const u8 cmac[4][16]={
    {0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46},
    {0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c},
    {0xdf,0xa6,0x67,0x47,0xde,0x9a,0xe6,0x30,0x30,0xca,0x32,0x61,0x14,0x97,0xc8,0x27},
    {0x51,0xf0,0xbe,0xbf,0x7e,0x3b,0x9d,0x92,0xfc,0x49,0x74,0x17,0x79,0x36,0x3c,0xfe}};
static const u8 ecb[]={0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97};
static int key_exchange_vectors(struct crypto_lskcipher *ec, struct crypto_lskcipher *cm)
{
    const u8 active[]={0,0,2,0,1,1,2}, regen[]={0,1,1,2,2,0,0};
    const u8 expected[][7]={{1,1,2,1,1,1,2},{3,3,3,3,4,4,3},
                            {0,5,5,3,4,5,6},{0,1,2,6,6,5,6}};
    struct q1000k_key_state prior={};
    struct q1000k_key_update update={};
    u8 decoded[16], name[32], digest[16];
    unsigned int context, command, i;
    int ret;

    ret=wait_for_random_bytes();
    if(ret) return ret;
    for(context=0;context<7;context++) for(command=0;command<4;command++) {
        u8 index=(command&1)+1, next=expected[command][context];
        bool confirm=command>=2;
        memset(&prior,0,sizeof(prior));
        prior.mac.tx_index=active[context]; prior.regenerating=regen[context];
        prior.mac.rx_valid=(active[context] ? BIT(active[context]-1) : 0) |
                          (regen[context] ? BIT(regen[context]-1) : 0);
        for(i=0;i<2;i++) if(prior.mac.rx_valid&BIT(i)) memcpy(prior.mac.key[i],message+16*i,16);
        ret=q1000k_key_prepare(ec,cm,key,&prior,confirm,index,&update);
        if(ret) goto out;
        if(update.next.mac.tx_index!=active[next] || update.next.regenerating!=regen[next]) {
            ret=-EINVAL; goto out;
        }
        if(confirm && !(prior.mac.rx_valid&BIT(index-1))) {
            if(update.report_index!=3-index || memchr_inv(update.report,0,32)) { ret=-EINVAL; goto out; }
            continue;
        }
        if(confirm) {
            memcpy(name,update.next.mac.key[index-1],16);
            memcpy(name+16,"3141592653589793",16);
            ret=gpon_aes_cmac_encrypt(cm,key,name,32,digest);
            if(!ret && memcmp(digest,update.report,16)) ret=-EBADMSG;
        } else {
            ret=gpon_aes_ecb_decrypt(ec,key,update.report,16,decoded);
            if(!ret && memcmp(decoded,update.next.mac.key[index-1],16)) ret=-EBADMSG;
        }
        if(!ret && memchr_inv(update.report+16,0,16)) ret=-EBADMSG;
        if(ret) goto out;
    }
    ret=0;
out:
    memzero_explicit(&prior,sizeof(prior)); memzero_explicit(&update,sizeof(update));
    memzero_explicit(decoded,sizeof(decoded)); memzero_explicit(name,sizeof(name));
    memzero_explicit(digest,sizeof(digest));
    return ret;
}
static int auth_vectors(struct crypto_lskcipher *tfm)
{
    struct q1000k_auth_keys keys;
    struct sk_buff *skb,*tail;
    u8 mic[4];
    unsigned int split;
    int ret;

    ret=q1000k_auth_derive(tfm,auth_msk,auth_serial,auth_pon_tag,&keys);
    if(ret || memcmp(&keys,&auth_expected,sizeof(keys))) return -EBADMSG;
    memzero_explicit(&keys,sizeof(keys));
    {
        u8 ploam[48]={0,0x13,0x0a,3,4,0x45,1};
        const u8 expected_mic[8]={0x46,0x39,0x87,0x56,0x28,0x08,0x14,0xe6};
        u8 report[32], expected[16], named[32];
        memcpy(ploam+40,expected_mic,8);
        ret=q1000k_auth_ploam_verify(tfm,auth_expected.ploam,ploam,48);
        if(ret) return ret;
        ploam[3]^=1;
        if(q1000k_auth_ploam_verify(tfm,auth_expected.ploam,ploam,48)!=-EBADMSG) return -EINVAL;
        ret=q1000k_auth_key_report(tfm,key,message,false,report);
        if(ret || memcmp(report,ecb,16)) return -EBADMSG;
        for(split=16;split<32;split++) if(report[split]) return -EINVAL;
        memcpy(named,message,16); memcpy(named+16,"3141592653589793",16);
        ret=gpon_aes_cmac_encrypt(tfm,key,named,32,expected);
        if(!ret) ret=q1000k_auth_key_report(tfm,key,message,true,report);
        if(ret || memcmp(report,expected,16)) return -EBADMSG;
        for(split=16;split<32;split++) if(report[split]) return -EINVAL;
        memzero_explicit(report,sizeof(report));
        memzero_explicit(named,sizeof(named));
    }
    for(split=0;split<=sizeof(auth_baseline);split++) {
        skb=alloc_skb(sizeof(auth_baseline),GFP_KERNEL);
        tail=alloc_skb(sizeof(auth_baseline),GFP_KERNEL);
        if(!skb || !tail) { kfree_skb(skb); kfree_skb(tail); return -ENOMEM; }
        skb_put_data(skb,auth_baseline,split);
        skb_put_data(tail,auth_baseline+split,sizeof(auth_baseline)-split);
        skb_shinfo(skb)->frag_list=tail;
        skb->len+=tail->len; skb->data_len+=tail->len; skb->truesize+=tail->truesize;
        ret=q1000k_auth_omci_verify(tfm,auth_expected.omci,skb);
        if(!ret) ret=q1000k_auth_omci_mic(tfm,auth_expected.omci,skb,true,2,mic);
        if(!ret && !memcmp(mic,auth_baseline+44,4)) ret=-EBADMSG;
        kfree_skb(skb);
        if(ret) return ret;
    }
    return 0;
}
static int __init pon_crypto_test_init(void)
{
    struct crypto_lskcipher *cm=NULL,*ec=NULL;
    static const size_t lengths[]={0,16,40,64};
    u8 out[16];
    unsigned int i;
    int ret=-EINVAL;
    if (!IS_ENABLED(CONFIG_UML) || !strstr(utsname()->release,"-q1000k-pon-crypto-test"))
        return -ENODEV;
    cm=gpon_aes_cmac_setup(); ec=gpon_aes_ecb_setup();
    if (!cm || !ec) { ret=-ENOENT; goto out; }
    ret=key_exchange_vectors(ec,cm);
    if(ret) goto out;
    for (i=0;i<ARRAY_SIZE(lengths);i++) {
        if(gpon_aes_cmac_encrypt(cm,key,lengths[i] ? message : NULL,lengths[i],out) ||
           memcmp(out,cmac[i],16)) goto out;
    }
    if(gpon_aes_ecb_encrypt(ec,key,message,16,out) || memcmp(out,ecb,16)) goto out;
    if(gpon_aes_ecb_decrypt(ec,key,ecb,16,out) || memcmp(out,message,16)) goto out;
    if(gpon_aes_ecb_encrypt(ec,key,message,15,out)!=-EINVAL) goto out;
    ret=auth_vectors(cm);
    if(ret) goto out;
    pr_info("Q1000K_PON_CRYPTO_KERNEL_PASS\n");
out:
    gpon_aes_cmac_key_free(cm); gpon_aes_cmac_key_free(ec);
    return ret;
}
static void __exit pon_crypto_test_exit(void) {}
module_init(pon_crypto_test_init);
module_exit(pon_crypto_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K PON cryptographic known-answer tests");
''')
