// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (UINT32_C(1)<<(n))
struct crypto_lskcipher { int kind; };
static struct crypto_lskcipher ecb={0},cmac={1};
static bool random_ready=true,owned=true;
static int random_calls,crypto_error,crypto_calls,provider_error,protocol_error;
static int reads,writes,delay,fail_read,fail_write,overflow_after;
static u32 fifo[11];
static unsigned int avail=32;
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static bool rng_is_initialized(void) { return random_ready; }
static void get_random_bytes(void *p,size_t n) { assert(random_ready && n==16); memset(p,++random_calls,n); }
static u32 get_unaligned_be32(const void *p) { const u8 *b=p; return (u32)b[0]<<24 | (u32)b[1]<<16 | b[2]<<8 | b[3]; }
static bool q1000k_protocol_owned(void) { return owned; }
static int q1000k_protocol_status(void) { return protocol_error; }
static int an7581_xpon_status(void) { return provider_error; }
static void udelay(unsigned int n) { assert(n==1); delay++; }
static u32 get_xpon_data(u32 reg) {
    assert(reg==0x5300); reads++;
    if(reads==fail_read) { provider_error=-ENODEV; return ~0U; }
    return overflow_after && writes>=overflow_after ? BIT(31) : avail;
}
static void set_xpon_data(u32 reg,u32 value) {
    assert(reg==0x5304 && writes<11 && avail);
    fifo[writes++]=value; avail--;
    if(writes==fail_write) provider_error=-EIO;
}
/* PRODUCTION */
int q1000k_auth_key_report(struct crypto_lskcipher *tfm,const u8 kek[16],const u8 key[16],bool confirm,u8 report[32]) {
    assert(tfm==(confirm ? &cmac : &ecb)); crypto_calls++;
    if(crypto_error) return crypto_error;
    memset(report,0,32); for(int i=0;i<16;i++) report[i]=key[i]^kek[i]^(confirm ? 0xab : 0xef); return 0;
}
int main(void)
{
    /* Explicit oracle from G Suppl.81 Fig.7-8: II,RI,RV,IR,VR,VI,IV. */
    const u8 active[]={0,0,2,0,1,1,2},regen[]={0,1,1,2,2,0,0};
    const u8 next[][7]={{1,1,2,1,1,1,2},{3,3,3,3,4,4,3},
                       {0,5,5,3,4,5,6},{0,1,2,6,6,5,6}};
    struct q1000k_key_state current={}; struct q1000k_key_update update,unchanged;
    u8 kek[16]={0x47};
    for(int c=0;c<7;c++) for(int command=0;command<4;command++) {
        u8 index=(command&1)+1; bool confirm=command>=2;
        memset(&current,0,sizeof(current)); current.mac.tx_index=active[c]; current.regenerating=regen[c];
        current.mac.rx_valid=(active[c] ? BIT(active[c]-1) : 0)|(regen[c] ? BIT(regen[c]-1) : 0);
        for(int i=0;i<2;i++) if(current.mac.rx_valid&BIT(i)) memset(current.mac.key[i],0x70+i,16);
        struct q1000k_key_state before=current;
        int old_random=random_calls,old_crypto=crypto_calls;
        assert(!q1000k_key_prepare(&ecb,&cmac,kek,&current,confirm,index,&update));
        assert(!memcmp(&before,&current,sizeof(current)));
        int expected=next[command][c];
        assert(update.next.mac.tx_index==active[expected] && update.next.regenerating==regen[expected]);
        assert(update.next.mac.rx_valid==((active[expected] ? BIT(active[expected]-1) : 0)|(regen[expected] ? BIT(regen[expected]-1) : 0)));
        bool swapped=confirm && !(before.mac.rx_valid&BIT(index-1));
        assert(update.report_index==(swapped ? 3-index : index));
        assert(random_calls==old_random+(!confirm && regen[c]!=index));
        assert(crypto_calls==old_crypto+!swapped);
        for(int i=0;i<2;i++) for(int j=0;j<16;j++) {
            u8 value=0;
            if(update.next.mac.rx_valid&BIT(i)) value=(!confirm && regen[c]!=index && i==index-1) ? random_calls : before.mac.key[i][j];
            assert(update.next.mac.key[i][j]==value);
        }
        for(int i=16;i<32;i++) assert(!update.report[i]);
        if(swapped) for(int i=0;i<16;i++) assert(!update.report[i]);
        assert(update.changed==(!confirm ? regen[c]!=index : regen[c]==index));
    }
    memset(&current,0,sizeof(current)); memset(&update,0xa5,sizeof(update)); unchanged=update;
    random_ready=false;
    assert(q1000k_key_prepare(&ecb,&cmac,kek,&current,false,1,&update)==-EAGAIN);
    assert(!memcmp(&update,&unchanged,sizeof(update))); random_ready=true;
    crypto_error=-EIO;
    assert(q1000k_key_prepare(&ecb,&cmac,kek,&current,false,1,&update)==-EIO);
    assert(!memcmp(&update,&unchanged,sizeof(update))); crypto_error=0;
    for(int i=0;i<256;i++) if(i!=1 && i!=2) assert(q1000k_key_prepare(&ecb,&cmac,kek,&current,false,i,&update)==-EINVAL);
    assert(q1000k_key_prepare(&ecb,&ecb,kek,&current,false,1,&update)==-EINVAL);
    current.mac.rx_valid=3;
    assert(q1000k_key_prepare(&ecb,&cmac,kek,&current,false,1,&update)==-EUCLEAN);
    current.mac.tx_index=current.regenerating=1;
    assert(q1000k_key_prepare(&ecb,&cmac,kek,&current,false,1,&update)==-EUCLEAN);

    u8 message[44]={}; for(int i=4;i<44;i++) message[i]=i;
    owned=false; assert(q1000k_ploam_send(message)==-EPERM && !reads && !writes); owned=true;
    assert(q1000k_ploam_send(NULL)==-EINVAL);
    for(int i=0;i<4;i++) { message[i]=2; assert(q1000k_ploam_send(message)==-EINVAL && !writes); message[i]=0; }
    assert(!q1000k_ploam_send(message) && writes==11 && !delay);
    for(int i=0;i<11;i++) assert(fifo[i]==get_unaligned_be32(message+4*i));
    for(int failure=1;failure<=11;failure++) {
        reads=writes=delay=0; avail=11; provider_error=0; fail_write=failure;
        assert(q1000k_ploam_send(message)==-EIO && writes==failure);
    }
    fail_write=0;
    for(int failure=1;failure<=2;failure++) {
        reads=writes=delay=0; avail=11; provider_error=0; fail_read=failure;
        assert(q1000k_ploam_send(message)==-ENODEV && writes==(failure==1 ? 0 : 11));
    }
    fail_read=0; reads=writes=delay=provider_error=0; avail=10;
    assert(q1000k_ploam_send(message)==-ETIMEDOUT && !writes && reads==3000 && delay==3000);
    reads=writes=delay=0; avail=11; overflow_after=11;
    assert(q1000k_ploam_send(message)==-EIO && writes==11);
    protocol_error=-EUCLEAN; reads=writes=0;
    assert(q1000k_ploam_send(message)==-EUCLEAN && !reads && !writes);
    return 0;
}
