// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
typedef unsigned char u8;
#define GFP_ATOMIC 0
struct crypto_lskcipher { int unused; };
struct sk_buff { unsigned int len, split; const u8 *head, *tail; };
static int crypto_calls, crypto_fail, alloc_fail, live_alloc;
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static void *kmalloc(size_t n,int flags) { if(alloc_fail) return NULL; live_alloc++; return malloc(n); }
static void kfree_sensitive(void *p) { assert(p && live_alloc); live_alloc--; free(p); }
static int skb_copy_bits(const struct sk_buff *skb,int off,void *out,int len)
{
    if(off<0 || len<0 || (unsigned int)off>skb->len || (unsigned int)len>skb->len-off) return -EFAULT;
    u8 *dest=out;
    for(int i=0;i<len;i++) { unsigned int pos=off+i; dest[i]=pos<skb->split ? skb->head[pos] : skb->tail[pos-skb->split]; }
    return 0;
}
static int crypto_memneq(const void *a,const void *b,size_t n) { return CRYPTO_memcmp(a,b,n); }
int gpon_aes_cmac_encrypt(struct crypto_lskcipher *tfm,const u8 *key,const u8 *data,size_t len,u8 *out)
{
    EVP_MAC *alg; EVP_MAC_CTX *ctx; size_t produced=0;
    char name[]="AES-128-CBC";
    OSSL_PARAM params[]={OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_CIPHER,name,0),OSSL_PARAM_END};
    assert(tfm && key && data);
    if(++crypto_calls==crypto_fail) { memset(out,0xee,16); return -EIO; }
    alg=EVP_MAC_fetch(NULL,"CMAC",NULL); assert(alg);
    ctx=EVP_MAC_CTX_new(alg); assert(ctx);
    assert(EVP_MAC_init(ctx,key,16,params)==1);
    assert(EVP_MAC_update(ctx,data,len)==1);
    assert(EVP_MAC_final(ctx,out,&produced,16)==1 && produced==16);
    EVP_MAC_CTX_free(ctx); EVP_MAC_free(alg); return 0;
}
/* PRODUCTION */
/* VECTORS */
int main(void)
{
    struct crypto_lskcipher cipher;
    struct q1000k_auth_keys keys,before;
    u8 data[1981],tail[1981],out[4],old[4],registration[36]={0};
    struct sk_buff skb={.len=48,.split=48,.head=data,.tail=tail};
    assert(!q1000k_auth_derive(&cipher,auth_msk,auth_serial,auth_pon_tag,&keys));
    assert(!memcmp(&keys,&auth_expected,sizeof(keys)));
    /* Independent OpenSSL CMAC supplies a downstream PLOAM vector. Mutate
     * every bit of its body/MIC; no altered message may authenticate.
     */
    {
        /* G.9807.1 C.IV.8 published Assign_Alloc-ID vector. */
        u8 message[48]={0,0x13,0x0a,3,4,0x45,1}, digest[16], directed[41]={1};
        const u8 mic[]={0x46,0x39,0x87,0x56,0x28,0x08,0x14,0xe6};
        memcpy(message+40,mic,8);
        assert(!q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,48));
        for(unsigned int i=0;i<40;i++) message[i]=(u8)(i*13+7);
        memcpy(directed+1,message,40);
        assert(!gpon_aes_cmac_encrypt(&cipher,auth_expected.ploam,directed,41,digest));
        memcpy(message+40,digest,8);
        assert(!q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,48));
        for(unsigned int i=0;i<48;i++) for(unsigned int bit=0;bit<8;bit++) {
            message[i]^=1U<<bit;
            assert(q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,48)==-EBADMSG);
            message[i]^=1U<<bit;
        }
        directed[0]=2;
        assert(!gpon_aes_cmac_encrypt(&cipher,auth_expected.ploam,directed,41,digest));
        memcpy(message+40,digest,8);
        assert(q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,48)==-EBADMSG);
        assert(!gpon_aes_cmac_encrypt(&cipher,auth_expected.ploam,message,40,digest));
        memcpy(message+40,digest,8);
        assert(q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,48)==-EBADMSG);
        for(unsigned int n=0;n<=52;n++) if(n!=48)
            assert(q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,n)==-EMSGSIZE);
        assert(q1000k_auth_ploam_verify(NULL,auth_expected.ploam,message,48)==-EINVAL);
        assert(q1000k_auth_ploam_verify(&cipher,NULL,message,48)==-EINVAL);
        assert(q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,NULL,48)==-EINVAL);
        crypto_fail=crypto_calls+1;
        assert(q1000k_auth_ploam_verify(&cipher,auth_expected.ploam,message,48)==-EIO);
        crypto_fail=0;
    }
    for(int failure=1;failure<=5;failure++) {
        crypto_calls=0; crypto_fail=failure; memset(&keys,0xa5,sizeof(keys)); before=keys;
        assert(q1000k_auth_registration(&cipher,registration,auth_serial,auth_pon_tag,&keys)==-EIO);
        assert(!memcmp(&keys,&before,sizeof(keys)));
    }
    crypto_fail=0;
    assert(!q1000k_auth_registration(&cipher,registration,auth_serial,auth_pon_tag,&keys));
    before=keys; registration[35]=1; /* Do not truncate to the GPON password's 10 bytes. */
    assert(!q1000k_auth_registration(&cipher,registration,auth_serial,auth_pon_tag,&keys));
    assert(memcmp(&keys,&before,sizeof(keys)));
    memcpy(data,auth_baseline,48);
    for(unsigned int split=0;split<=48;split++) {
        skb.split=split; memcpy(tail,data+split,48-split);
        assert(!q1000k_auth_omci_verify(&cipher,auth_expected.omci,&skb));
        assert(!q1000k_auth_omci_mic(&cipher,auth_expected.omci,&skb,true,2,out));
        assert(memcmp(out,auth_baseline+44,4));
    }
    skb.split=48;
    for(unsigned int offset=0;offset<48;offset++) for(unsigned int bit=0;bit<8;bit++) {
        data[offset]^=1U<<bit;
        assert(q1000k_auth_omci_verify(&cipher,auth_expected.omci,&skb)<0);
        data[offset]^=1U<<bit;
    }
    memset(out,0xa5,4); memcpy(old,out,4); alloc_fail=1;
    assert(q1000k_auth_omci_mic(&cipher,auth_expected.omci,&skb,true,1,out)==-ENOMEM);
    assert(!memcmp(old,out,4) && !live_alloc); alloc_fail=0;
    crypto_calls=0; crypto_fail=1;
    assert(q1000k_auth_omci_mic(&cipher,auth_expected.omci,&skb,true,1,out)==-EIO);
    assert(!memcmp(old,out,4) && !live_alloc); crypto_fail=0;
    for(unsigned int n=0;n<=1981;n++) {
        skb.len=skb.split=n;
        if(n!=48) assert(q1000k_auth_omci_mic(&cipher,auth_expected.omci,&skb,true,1,out)<0);
    }
    /* Extended lengths include zero content and the maximum complete PDU. */
    for(unsigned int n=0;n<=1966;n+=1) {
        memset(data,0,sizeof(data)); data[3]=0x0b; data[8]=n>>8; data[9]=n;
        skb.len=skb.split=n+10;
        assert(!q1000k_auth_omci_mic(&cipher,auth_expected.omci,&skb,false,1,out));
        memcpy(data+skb.len,out,4); skb.len+=4;
        unsigned int split=n%skb.len; skb.split=split; memcpy(tail,data+split,skb.len-split);
        assert(!q1000k_auth_omci_verify(&cipher,auth_expected.omci,&skb));
        skb.len--; assert(q1000k_auth_omci_verify(&cipher,auth_expected.omci,&skb)==-EMSGSIZE);
    }
    assert(!live_alloc);
    return 0;
}
