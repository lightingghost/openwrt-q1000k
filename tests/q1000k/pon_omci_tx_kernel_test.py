#!/usr/bin/env python3
"""Generate a UML-only skb/MIC-caller test; all hardware operations are fixtures."""
from test_pon_omci_tx import production

print(r'''
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/utsname.h>
#ifndef CONFIG_UML
#error This test is for a disposable UML guest only.
#endif
typedef unsigned char unchar;
typedef unsigned long ulong;
#define Q1000K_PON_IDENTITY
#define TCSUPPORT_CPU_ARMV8_64
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
int q1000k_omci_tx_prepare(struct sk_buff *skb);
int remove_omci_crc_if_exist(struct sk_buff *skb);
int gwan_add_us_omci_mic(struct sk_buff *skb);
static struct { spinlock_t cmac_test; struct { int omciIkIdx; } gponSecurity; } gpon,*gpGponPriv=&gpon;
static struct device test_device;
static int outstanding,allocs,frees,cmac_calls,cmac_error,fail_allocation;
static bool no_device;
static struct device *get_xpon_dev(void) { return no_device?NULL:&test_device; }
static void *test_alloc(struct device *dev,size_t len,dma_addr_t *addr,gfp_t flags)
{
    void *ptr;
    allocs++;
    if((fail_allocation==1 && !outstanding) || (fail_allocation==2 && outstanding)) return NULL;
    ptr=kmalloc(len,flags);
    if(ptr) { outstanding++; *addr=(dma_addr_t)(unsigned long)ptr; }
    return ptr;
}
static void test_free(struct device *dev,size_t len,void *ptr,dma_addr_t addr)
{
    outstanding--; frees++; kfree(ptr);
}
/* No DMA or CMAC hardware is used, even inside the disposable guest. */
#define dma_alloc_coherent test_alloc
#define dma_free_coherent test_free
static int gponDevSetCmac0Start(int key,int dir,void *iv,unchar *msg,unsigned int len,
                               unchar *result_addr,unchar *result,unsigned int result_len)
{
    cmac_calls++;
    if(len<10 || result_len!=5 || key!=gpon.gponSecurity.omciIkIdx || dir!=GPON_CMAC_UPSTREAM) return -EINVAL;
    memset(result,0xa5,result_len);
    return cmac_error;
}
''')
print(production())
print(r'''
static int packet_case(bool extended,bool fragmented,bool cloned,bool trailer,int error)
{
    unsigned char bytes[64],original[64];
    unsigned int body=extended?17:44,len=body+(trailer?4:0),head=fragmented?4:len;
    struct sk_buff *skb,*parent=NULL;
    int ret;
    memset(bytes,0x31,sizeof(bytes)); bytes[3]=extended?0x0b:0x0a;
    bytes[8]=0; bytes[9]=body-10;
    allocs=frees=outstanding=cmac_calls=0;
    cmac_error=error==1?-ETIMEDOUT:0;
    no_device=error==2; fail_allocation=error>=3?error-2:0;
    skb=alloc_skb(64,GFP_KERNEL);
    if(!skb) return -ENOMEM;
    /* No spare tailroom: trim/reuse a trailer or expand to append one. */
    skb_reserve(skb,skb_tailroom(skb)-head);
    skb_put_data(skb,bytes,head);
    if(fragmented) {
        struct page *page=alloc_page(GFP_KERNEL);
        if(!page) { kfree_skb(skb); return -ENOMEM; }
        memcpy(page_address(page),bytes+head,len-head);
        skb_add_rx_frag(skb,0,page,0,len-head,PAGE_SIZE);
    }
    if(cloned) {
        parent=skb; skb=skb_clone(parent,GFP_KERNEL);
        if(!skb) { kfree_skb(parent); return -ENOMEM; }
    }
    ret=gwan_add_us_omci_mic(skb);
    if(ret!=(error?XPON_FAIL:XPON_SUCCESS) || outstanding ||
       skb->len!=body+(error?0:4) || skb_is_nonlinear(skb) ||
       skb_tail_pointer(skb)!=skb->data+skb->len || memcmp(skb->data,bytes,body)) {
        ret=-EINVAL; goto out;
    }
    if((error<2 && cmac_calls!=1) || (error>=2 && cmac_calls)) { ret=-EINVAL; goto out; }
    if(!error) {
        unsigned int i;
        for(i=0;i<4;i++) if(skb->data[body+i]!=0xa5) { ret=-EBADMSG; goto out; }
    }
    if(parent && (parent->len!=len || skb_copy_bits(parent,0,original,len) ||
                  memcmp(original,bytes,len))) { ret=-EBADMSG; goto out; }
    ret=0;
out:
    kfree_skb(skb); kfree_skb(parent); return ret;
}
static int __init omci_tx_test_init(void)
{
    unsigned int ext,frag,clone,trailer,error,cases=0;
    int ret;
    if(!strstr(init_utsname()->release,"-q1000k-pon-omci-tx-test")) return -EPERM;
    spin_lock_init(&gpon.cmac_test);
    for(ext=0;ext<2;ext++) for(frag=0;frag<2;frag++) for(clone=0;clone<2;clone++) {
        gpon.gponSecurity.omciIkIdx=clone;
        for(trailer=0;trailer<2;trailer++) for(error=0;error<5;error++) {
            ret=packet_case(ext,frag,clone,trailer,error);
            if(ret) {
                pr_err("Q1000K_PON_OMCI_TX_KERNEL_FAIL case=%u err=%d ext=%u frag=%u clone=%u fault=%u\n",cases,ret,ext,frag,clone,error);
                return ret;
            }
            cases++;
        }
    }
    pr_info("Q1000K_PON_OMCI_TX_KERNEL_PASS cases=%u\n",cases);
    return 0;
}
static void __exit omci_tx_test_exit(void) {}
module_init(omci_tx_test_init);
module_exit(omci_tx_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UML-only Q1000K OMCI TX buffer and failure test");
''')
