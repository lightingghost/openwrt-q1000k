/* No hardware: execute the imported MIC caller with allocation/CMAC faults. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned char unchar;
typedef unsigned long ulong;
typedef uintptr_t dma_addr_t;
#define Q1000K_PON_IDENTITY
#define TCSUPPORT_CPU_ARMV8_64
#define CHECKSUM_NONE 0
#define CHECKSUM_PARTIAL 1
#define GFP_ATOMIC 0
#define XPON_FAIL -1
#define XPON_SUCCESS 0
#define OMCI_CRC_LEN 4
#define OMCI_BASIC_MSG_FIX_LEN 48
#define OMCI_BASIC_MSG_DEV_ID 0x0a
#define OMCI_EXTENDED_MSG_DEV_ID 0x0b
#define OMCI_MIC_RESULT_LEN 5
#define DMA_ALLOC_MAX_NUM 2
#define GPON_OMCI_IK_IDX0 0
#define GPON_CMAC_OMCI_IDX0 0
#define GPON_CMAC_OMCI_IDX1 1
#define GPON_CMAC_UPSTREAM 0
#define PON_MSG(...) ((void)0)
#define XPON_ARR_PRINT(...) ((void)0)
#define printk(...) ((void)0)
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define spin_lock_irqsave(lock,flags) do { assert(!*(lock)); *(lock)=true; (flags)=0; } while(0)
#define spin_unlock_irqrestore(lock,flags) do { assert(*(lock) && (flags)==0); *(lock)=false; } while(0)
struct sk_buff {
    unsigned len,tail,end; unsigned char data[16132];
    bool shared,cloned,gso,vlan,nonlinear,linearize_fail,expand_fail;
    int ip_summed;
};
static bool skb_shared(struct sk_buff *s) { return s->shared; }
static bool skb_cloned(struct sk_buff *s) { return s->cloned; }
static bool skb_is_gso(struct sk_buff *s) { return s->gso; }
static bool skb_vlan_tag_present(struct sk_buff *s) { return s->vlan; }
static int skb_tailroom(struct sk_buff *s) { assert(s->end>=s->tail); return s->end-s->tail; }
static int skb_linearize(struct sk_buff *s) {
    if(s->linearize_fail) return -ENOMEM;
    s->nonlinear=false; s->tail=s->len; return 0;
}
static int pskb_expand_head(struct sk_buff *s,int head,int extra,int gfp) {
    assert(!s->shared && !s->nonlinear && extra>=0);
    if(s->expand_fail) return -ENOMEM;
    s->end+=extra; assert(s->end<=sizeof(s->data)); s->cloned=false; return 0;
}
static void skb_trim(struct sk_buff *s,unsigned len) {
    assert(!s->nonlinear && len<=s->len); s->len=s->tail=len;
}
static void skb_put_data(struct sk_buff *s,const void *data,unsigned len) {
    assert(!s->nonlinear && !s->cloned && s->len==s->tail && s->tail+len<=s->end);
    memcpy(s->data+s->tail,data,len); s->len+=len; s->tail+=len;
}
static struct { bool cmac_test; struct { int omciIkIdx; } gponSecurity; } gpon,*gpGponPriv=&gpon;
struct device { int dummy; };
static struct device device;
static bool no_device;
static int allocs,frees,outstanding,cmac_calls,fail_allocation,cmac_error;
static unsigned expected_len;
static struct device *get_xpon_dev(void) { return no_device?NULL:&device; }
static void *dma_alloc_coherent(struct device *dev,unsigned len,dma_addr_t *addr,int flags) {
    allocs++;
    if((fail_allocation==1 && !outstanding) || (fail_allocation==2 && outstanding)) return NULL;
    void *p=calloc(1,len); assert(p); *addr=(uintptr_t)p; outstanding++; return p;
}
static void dma_free_coherent(struct device *dev,unsigned len,void *ptr,dma_addr_t addr) {
    assert(ptr && (uintptr_t)ptr==addr && outstanding>0); frees++; outstanding--; free(ptr);
}
static int gponDevSetCmac0Start(int key,int direction,void *iv,unchar *msg,unsigned len,
                               unchar *result_addr,unchar *result,unsigned result_len) {
    assert(gpon.cmac_test && direction==GPON_CMAC_UPSTREAM && key==gpon.gponSecurity.omciIkIdx);
    assert(len==expected_len && result_addr==result && result_len==5);
    assert(msg[3]==0x0a || msg[3]==0x0b); cmac_calls++;
    for(unsigned i=0;i<5;i++) result[i]=0xa0+i;
    return cmac_error;
}
/* PRODUCTION */
_Static_assert(sizeof(Omci_Header_T)==10,"OMCI header ABI");
static void setup(struct sk_buff *s,unsigned len,unsigned body,bool extended) {
    memset(s,0,sizeof(*s)); s->len=s->tail=s->end=len;
    s->data[3]=extended?0x0b:0x0a;
    s->data[8]=(body-10)>>8; s->data[9]=body-10;
    allocs=frees=outstanding=cmac_calls=fail_allocation=cmac_error=0; no_device=false;
    expected_len=body;
}
int main(void) {
    struct sk_buff skb;
    assert(q1000k_omci_tx_prepare(NULL)==-EINVAL);
    for(unsigned n=0;n<=65535;n++) {
        for(unsigned trailer=0;trailer<=4;trailer+=4) {
            unsigned body=n+10,len=body+trailer;
            setup(&skb,len,body,true);
            int ret=remove_omci_crc_if_exist(&skb);
            if(body<=16124) {
                assert(ret==0 && skb.len==body && skb.tail==body && skb_tailroom(&skb)>=4);
            } else assert(ret<0);
            setup(&skb,10,body,true);
            ret=remove_omci_crc_if_exist(&skb);
            assert(n==0 ? ret==0 : ret<0);
        }
    }
    for(unsigned len=0;len<55;len++) {
        setup(&skb,len,44,false);
        int ret=remove_omci_crc_if_exist(&skb);
        assert((len==44 || len==48) ? ret==0 : ret<0);
    }
    for(int fault=0;fault<10;fault++) {
        setup(&skb,44,44,false);
        if(fault==0) skb.shared=true;
        if(fault==1) skb.gso=true;
        if(fault==2) skb.vlan=true;
        if(fault==3) skb.ip_summed=CHECKSUM_PARTIAL;
        if(fault==4) skb.linearize_fail=true;
        if(fault==5) skb.expand_fail=true;
        if(fault==6) skb.data[3]=0xff;
        if(fault==7) no_device=true;
        if(fault==8) fail_allocation=1;
        if(fault==9) fail_allocation=2;
        assert(gwan_add_us_omci_mic(&skb)==XPON_FAIL);
        assert(!cmac_calls && !outstanding && skb.len==44 && skb.tail==44);
    }
    for(int extended=0;extended<2;extended++) for(int clone=0;clone<2;clone++) {
        unsigned body=extended?17:44;
        for(int error=0;error<2;error++) {
            setup(&skb,body+4,body,extended); skb.cloned=clone; skb.nonlinear=true;
            cmac_error=error?-EIO:0; gpon.gponSecurity.omciIkIdx=clone;
            assert(gwan_add_us_omci_mic(&skb)==(error?XPON_FAIL:XPON_SUCCESS));
            assert(cmac_calls==1 && allocs==2 && frees==2 && !outstanding && !gpon.cmac_test);
            assert(skb.len==body+(error?0:4) && skb.tail==skb.len && !skb.cloned && !skb.nonlinear);
            if(!error) for(unsigned i=0;i<4;i++) assert(skb.data[body+i]==0xa0+i);
        }
    }
    return 0;
}
