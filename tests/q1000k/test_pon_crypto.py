#!/usr/bin/env python3
"""Run the production PON AES/CMAC helpers with OpenSSL AES and fault injection."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest
from test_pon_identity import BSP


def function(source, name):
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    match = re.search(r'^(?:static )?(?:int|void|struct crypto_lskcipher \*)\s*' + name + r'\(', source, re.M)
    start = match.start()
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos] + '\n'


class PonCryptoTests(unittest.TestCase):
    def test_vectors_fragments_lengths_and_every_crypto_failure(self):
        source = (BSP.parent / 'xpon-en757x/xpon_10g/src/gpon/gpon_security.c').read_text(errors='replace')
        production = ''.join(function(source, name) for name in [
            'gf_mulx', 'aes_128_cmac_vector', 'gpon_aes_cmac_encrypt',
            'gpon_aes_cmac_setup', 'gpon_aes_ecb_encrypt', 'gpon_aes_ecb_decrypt',
            'gpon_aes_ecb_setup', 'gpon_aes_cmac_key_free'])
        code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
typedef unsigned char u8;
#define AES_BLOCK_SIZE 16
#define AES_CMAC_KEY_LEN 16
#define AES_ECB_KEY_LEN 16
#define CMAC_TLEN 16
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define PON_MSG(...) ((void)0)
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
static int gpon_cmac_lock,gpon_ecb_lock;
static unsigned int held,calls,fail_at,alloc_fail;
#define spin_lock_irqsave(l,f) do { (void)(l); (f)=0; assert(!held); held=1; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(l); (void)(f); assert(held); held=0; } while(0)
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
struct crypto_lskcipher { u8 key[16]; };
static struct crypto_lskcipher cipher;
static struct crypto_lskcipher *crypto_alloc_lskcipher(const char *name,int type,int mask) {
    assert(!strcmp(name,"ecb(aes)") && !type && !mask);
    return alloc_fail ? (void *)(intptr_t)-ENOENT : &cipher;
}
static void crypto_free_lskcipher(struct crypto_lskcipher *tfm) { assert(tfm==&cipher); }
static int crypto_lskcipher_setkey(struct crypto_lskcipher *tfm,const u8 *key,size_t len) {
    assert(held && tfm && key && len==16);
    if(++calls==fail_at) return -EKEYREJECTED;
    memcpy(tfm->key,key,16); return 0;
}
static int crypt(struct crypto_lskcipher *tfm,const u8 *in,u8 *out,size_t n,void *iv,int encrypt) {
    EVP_CIPHER_CTX *ctx;
    int produced=0,tail=0;
    assert(held && tfm && n==16 && !iv);
    if(++calls==fail_at) { memset(out,0xee,n); return -EIO; }
    ctx=EVP_CIPHER_CTX_new(); assert(ctx);
    assert(EVP_CipherInit_ex(ctx,EVP_aes_128_ecb(),NULL,tfm->key,NULL,encrypt)==1);
    assert(EVP_CIPHER_CTX_set_padding(ctx,0)==1);
    assert(EVP_CipherUpdate(ctx,out,&produced,in,n)==1 && produced==16);
    assert(EVP_CipherFinal_ex(ctx,out+produced,&tail)==1 && !tail);
    EVP_CIPHER_CTX_free(ctx); return 0;
}
static int crypto_lskcipher_encrypt(struct crypto_lskcipher *t,const u8 *i,u8 *o,size_t n,void *v) { return crypt(t,i,o,n,v,1); }
static int crypto_lskcipher_decrypt(struct crypto_lskcipher *t,const u8 *i,u8 *o,size_t n,void *v) { return crypt(t,i,o,n,v,0); }
''' + production + r'''
static void oracle(const u8 *key,const u8 *data,size_t len,u8 *out) {
    EVP_MAC *alg=EVP_MAC_fetch(NULL,"CMAC",NULL);
    EVP_MAC_CTX *ctx;
    char name[]="AES-128-CBC";
    OSSL_PARAM params[]={OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_CIPHER,name,0),OSSL_PARAM_END};
    size_t produced=0;
    assert(alg); ctx=EVP_MAC_CTX_new(alg); assert(ctx);
    assert(EVP_MAC_init(ctx,key,16,params)==1);
    if(len) assert(EVP_MAC_update(ctx,data,len)==1);
    assert(EVP_MAC_final(ctx,out,&produced,16)==1 && produced==16);
    EVP_MAC_CTX_free(ctx); EVP_MAC_free(alg);
}
static const u8 key[]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
static const u8 message[]={
    0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
    0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
    0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10};
/* NIST SP800-38B AES-128 examples, also in Linux crypto/testmgr.h. */
static const u8 cmac[4][16]={
    {0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46},
    {0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c},
    {0xdf,0xa6,0x67,0x47,0xde,0x9a,0xe6,0x30,0x30,0xca,0x32,0x61,0x14,0x97,0xc8,0x27},
    {0x51,0xf0,0xbe,0xbf,0x7e,0x3b,0x9d,0x92,0xfc,0x49,0x74,0x17,0x79,0x36,0x3c,0xfe}};
static const u8 ecb[]={0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97};
static int vector(size_t count,const u8 **addr,const size_t *len,u8 *out) {
    unsigned long flags; int ret;
    spin_lock_irqsave(&gpon_cmac_lock,flags);
    ret=aes_128_cmac_vector(&cipher,key,count,addr,len,out);
    spin_unlock_irqrestore(&gpon_cmac_lock,flags);
    return ret;
}
int main(void) {
    const size_t lengths[]={0,16,40,64};
    u8 out[16],before[16],expected[16],data[257];
    size_t i,n,split;
    unsigned int operations,step;
    assert(gpon_aes_cmac_setup()==&cipher && gpon_aes_ecb_setup()==&cipher);
    alloc_fail=1;
    assert(!gpon_aes_cmac_setup() && !gpon_aes_ecb_setup()); alloc_fail=0;
    gpon_aes_cmac_key_free(&cipher); gpon_aes_cmac_key_free(NULL);
    for(i=0;i<4;i++) {
        calls=0;
        assert(!gpon_aes_cmac_encrypt(&cipher,key,lengths[i] ? message : NULL,lengths[i],out));
        assert(!memcmp(out,cmac[i],16) && !held);
        operations=calls;
        for(step=1;step<=operations;step++) {
            memset(out,0xa5,16); memcpy(before,out,16); calls=0; fail_at=step;
            assert(gpon_aes_cmac_encrypt(&cipher,key,message,lengths[i],out)==(step==1 ? -EKEYREJECTED : -EIO));
            assert(calls==step && !memcmp(out,before,16) && !held);
        }
        fail_at=0;
    }
    for(i=0;i<sizeof(data);i++) data[i]=(i*73)^0x5a;
    for(n=0;n<=sizeof(data);n++) {
        oracle(key,data,n,expected);
        for(split=0;split<=n;split++) {
            const u8 *addr[]={NULL,data,NULL,data+split,NULL};
            const size_t len[]={0,split,0,n-split,0};
            assert(!vector(5,addr,len,out) && !memcmp(out,expected,16));
        }
    }
    assert(!vector(0,NULL,NULL,out) && !memcmp(out,cmac[0],16));
    calls=0;
    assert(gpon_aes_cmac_encrypt(NULL,key,message,16,out)==-EINVAL);
    assert(gpon_aes_cmac_encrypt(&cipher,NULL,message,16,out)==-EINVAL);
    assert(gpon_aes_cmac_encrypt(&cipher,key,NULL,1,out)==-EINVAL);
    assert(gpon_aes_cmac_encrypt(&cipher,key,message,16,NULL)==-EINVAL);
    { const u8 *addr[]={data,data}; const size_t len[]={SIZE_MAX,1};
      assert(vector(2,addr,len,out)==-EOVERFLOW); }
    assert(!calls && !held);
    assert(!gpon_aes_ecb_encrypt(&cipher,key,message,16,out) && !memcmp(out,ecb,16));
    assert(!gpon_aes_ecb_decrypt(&cipher,key,ecb,16,out) && !memcmp(out,message,16));
    for(i=0;i<33;i++) if(i!=16) {
        calls=0;
        assert(gpon_aes_ecb_encrypt(&cipher,key,message,i,out)==-EINVAL);
        assert(gpon_aes_ecb_decrypt(&cipher,key,message,i,out)==-EINVAL);
        assert(!calls && !held);
    }
    for(step=1;step<=2;step++) {
        memset(out,0xa5,16); memcpy(before,out,16); calls=0; fail_at=step;
        assert(gpon_aes_ecb_encrypt(&cipher,key,message,16,out)==(step==1 ? -EKEYREJECTED : -EIO));
        assert(calls==step && !memcmp(out,before,16) && !held);
        calls=0;
        assert(gpon_aes_ecb_decrypt(&cipher,key,ecb,16,out)==(step==1 ? -EKEYREJECTED : -EIO));
        assert(calls==step && !memcmp(out,before,16) && !held);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-pon-crypto-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(c), '-lcrypto', '-o', str(exe)], check=True)
            # LeakSanitizer cannot run under the workspace's ptrace sandbox.
            # ASan bounds/use-after-free checks and UBSan remain enabled.
            env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
            subprocess.run([str(exe)], check=True, env=env)


    def test_security_allocation_and_timer_lifecycle(self):
        source = (BSP.parent / 'xpon-en757x/xpon_10g/src/gpon/gpon_security.c').read_text(errors='replace')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define GPON_PLOAM_IK_IDX1 1
#define GPON_OMCI_IK_IDX1 1
#define GPON_SECURITY_TK4_TIMER 10
#define GPON_SECURITY_TK5_TIMER 20
struct crypto_lskcipher { bool live; };
struct timer { bool initialized,stopped; };
struct gpon_priv {
    struct {
        struct crypto_lskcipher *tfm,*aesEcbTfm;
        struct timer TK4_timer,TK5_timer;
        int ploamIkIdx,omciIkIdx;
    } gponSecurity;
};
static struct gpon_priv priv,*gpGponPriv;
static struct crypto_lskcipher objects[2];
static bool gpon_security_ready,published;
static int allocations,fail_at,frees,timers,shutdowns,writes;
static struct crypto_lskcipher *allocate(void) {
    allocations++;
    if(allocations==fail_at) return NULL;
    assert(allocations<=2);
    objects[allocations-1].live=true;
    return &objects[allocations-1];
}
static struct crypto_lskcipher *gpon_aes_cmac_setup(void) { return allocate(); }
static struct crypto_lskcipher *gpon_aes_ecb_setup(void) { return allocate(); }
static void gpon_aes_cmac_key_free(struct crypto_lskcipher *t) {
    assert(t && t->live);
    if(published) assert(shutdowns==2);
    t->live=false; frees++;
}
static void init_timer(struct timer *t,int interval) {
    assert(objects[0].live && objects[1].live && (interval==10 || interval==20));
    t->initialized=true; timers++; published=true;
}
#define GPON_CREATE_TIMER(t,fn,ms) init_timer(t,ms)
static void timer_shutdown_sync(struct timer *t) {
    assert(t->initialized && !t->stopped);
    assert(objects[0].live && objects[1].live);
    t->stopped=true; shutdowns++;
}
static void gponDevSetPloamIkIdx(int i) { assert(i==1 && timers==2); writes++; }
static void gponDevSetOmciIkIdx(int i) { assert(i==1 && timers==2); writes++; }
''' + function(source, 'gpon_security_init') + function(source, 'gpon_security_exit') + r'''
int main(void) {
    int failure;
    assert(gpon_security_init()==-ENODEV); gpon_security_exit();
    for(failure=1;failure<=2;failure++) {
        memset(&priv,0,sizeof(priv)); memset(objects,0,sizeof(objects));
        gpGponPriv=&priv; allocations=frees=timers=shutdowns=writes=0;
        published=gpon_security_ready=false; fail_at=failure;
        assert(gpon_security_init()==-ENOMEM);
        assert(allocations==failure && frees==failure-1);
        assert(!timers && !shutdowns && !writes && !gpon_security_ready);
        assert(!priv.gponSecurity.tfm && !priv.gponSecurity.aesEcbTfm);
        assert(!objects[0].live && !objects[1].live);
        gpon_security_exit(); assert(!shutdowns && frees==failure-1);
    }
    allocations=frees=0; fail_at=0;
    assert(!gpon_security_init());
    assert(allocations==2 && timers==2 && writes==2 && gpon_security_ready);
    assert(gpon_security_init()==-EALREADY && allocations==2 && writes==2);
    gpon_security_exit();
    assert(shutdowns==2 && frees==2 && !gpon_security_ready);
    assert(!priv.gponSecurity.tfm && !priv.gponSecurity.aesEcbTfm);
    assert(!objects[0].live && !objects[1].live);
    gpon_security_exit(); assert(shutdowns==2 && frees==2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-pon-crypto-lifetime-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
