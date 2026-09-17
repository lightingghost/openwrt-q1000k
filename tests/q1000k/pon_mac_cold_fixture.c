// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define BIT(n) (1UL<<(n))
#define Q1000K_TABLE_CLEAR 1
#define Q1000K_TABLE_INSTALL 2
#define Q1000K_TABLE_ACTIVATE 3
static u32 regs[0x6000/4], written[0x6000/4];
static int phase, writes, fail_write, provider_error, delays;
static bool owned, select_stuck, self_clear;
#define AN7581_XPON_MBI_TX_STOP BIT(8)
#define AN7581_XPON_MPI_TX_STOP BIT(24)
static int stop_calls, stop_fail, drain_error;
static void q1000k_activation_snapshot(u32 stage,u32 sequence) {}
static int an7581_xpon_mac_stop(u32 mask,bool hold) {
    assert(mask==BIT(8)||mask==BIT(24));
    if(++stop_calls==stop_fail) return -ETIMEDOUT;
    if(hold) regs[0x5004/4]|=mask; else regs[0x5004/4]&=~mask;
    return 0;
}
static int an7581_xpon_mac_wait_tx_empty(void) {
    assert((regs[0x5004/4]&BIT(8)) && !(regs[0x5004/4]&BIT(24)));
    return drain_error;
}
static int q1000k_pipeline_table_context(int wanted) { return phase==wanted ? 0 : -EPERM; }
static bool q1000k_protocol_owned(void) { return owned; }
static int an7581_xpon_status(void) { return provider_error; }
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static void udelay(int n) { assert(n==1); delays++; }
static u32 get_xpon_data(u32 reg) { assert(reg<0x6000 && !(reg&3)); return regs[reg/4]; }
static void set_xpon_data(u32 reg,u32 value) {
    assert(reg<0x6000 && !(reg&3)); writes++;
    u32 flip=reg==0x5868 ? 1 : reg==0x5800 ? BIT(3) : reg==0x582c ? BIT(12) : reg==0x5500 ? BIT(8) : 1;
    written[reg/4]=value; regs[reg/4]=value^(writes==fail_write ? flip : 0);
    if(reg==0x53e8 && !select_stuck) regs[0x5318/4]=0x10001;
    if(reg==0x53e8 && self_clear) regs[reg/4]&=~0x01010000;
    if(reg==0x5284) regs[reg/4]|=BIT(8); /* Independent RO dying-gasp status. */
    if(reg==0x5868) regs[reg/4]|=BIT(2); /* Independent OC FEC indication. */
    if(reg==0x582c) regs[reg/4]|=BIT(31); /* Independent TX-sync status. */
}
static bool rx_bench;
static bool q1000k_rx_bench_enabled(void) { return rx_bench; }
/* PRODUCTION */
static void reset(void) {
    memset(regs,0,sizeof(regs)); memset(written,0,sizeof(written));
    writes=delays=fail_write=provider_error=0; phase=0; owned=select_stuck=self_clear=false;
    stop_calls=stop_fail=drain_error=0;
}
int main(void) {
    u8 sn[8]={1,2,3,4,5,6,7,8},reg[36];
    for(unsigned int mode=1;mode<=3;mode++) {
        reset(); assert(q1000k_mac_ranging_bench(573115,mode)==-EPERM && !writes);
        owned=true; regs[0x5104/4]=4; regs[0x511c/4]=0x11011101;
        regs[0x5200/4]=0xabc; regs[0x5108/4]=5632;
        assert(!q1000k_mac_ranging_bench(573115,mode));
        assert(regs[0x5114/4]==2292460 && regs[0x511c/4]==0x11011101);
        assert(!regs[0x5004/4] && regs[0x5200/4]==0xabc && regs[0x5108/4]==5632);
        assert(stop_calls==(mode==3 ? 0 : 4));
        assert((regs[0x582c/4]&0x101)==(mode==2 ? 0x101 : 0));
        int count=writes;
        for(int fail=1;fail<=count;fail++) {
            reset(); owned=true; regs[0x5104/4]=4; regs[0x511c/4]=0x11011101;
            fail_write=fail; assert(q1000k_mac_ranging_bench(573115,mode)==-EIO);
            assert(writes==fail);
        }
    }
    for(int fail=1;fail<=4;fail++) {
        reset(); owned=true; regs[0x5104/4]=4; regs[0x511c/4]=1; stop_fail=fail;
        assert(q1000k_mac_ranging_bench(1,1)==-ETIMEDOUT && stop_calls==fail);
    }
    reset(); owned=true; regs[0x5104/4]=5; regs[0x511c/4]=1;
    assert(q1000k_mac_ranging_bench(1,3)==-EAGAIN && !writes);
    regs[0x5104/4]=4; regs[0x5004/4]=BIT(16);
    assert(q1000k_mac_ranging_bench(1,1)==-EAGAIN && !writes);
    regs[0x5004/4]=0; drain_error=-ETIMEDOUT;
    assert(q1000k_mac_ranging_bench(1,1)==-ETIMEDOUT && !(regs[0x511c/4]&1));
    reset();
    assert(q1000k_mac_ranging_install(123)==-EPERM && !writes);
    phase=2;
    assert(q1000k_mac_ranging_install(0x40000000)==-ERANGE && !writes);
    assert(!q1000k_mac_ranging_install(0x3fffffff) && regs[0x5114/4]==0xfffffffc);
    assert(written[0x582c/4]==0x101);
    assert(q1000k_mac_ranging_ready()==-EPERM);
    phase=3; assert(!q1000k_mac_ranging_ready() && !(regs[0x582c/4]&0x101));
    reset(); phase=2; fail_write=1; assert(q1000k_mac_ranging_install(123)==-EIO);
    reset(); phase=2; fail_write=2; assert(q1000k_mac_ranging_install(123)==-EIO);
    reset(); phase=3; regs[0x582c/4]=BIT(31)|0x101; fail_write=1;
    assert(q1000k_mac_ranging_ready()==-EIO && writes==1);
    reset(); phase=3; assert(q1000k_mac_ranging_ready()==-ETIMEDOUT && delays==3000);
    reset(); phase=3; regs[0x582c/4]=~0U; assert(q1000k_mac_ranging_ready()==-EIO && !writes);
    reset(); phase=3; provider_error=-ENODEV; assert(q1000k_mac_ranging_ready()==-ENODEV && !writes);
    reset();
    for(int i=0;i<36;i++) reg[i]=i;
    assert(q1000k_mac_cold_release()==-EPERM);
    assert(q1000k_mac_cold_install(sn,reg,false)==-EPERM);
    assert(q1000k_mac_cold_select_keys()==-EPERM && !writes);
    phase=1; regs[0x5000/4]=0x12340000;
    assert(!q1000k_mac_cold_release() && regs[0x5000/4]==0x12340001);
    phase=2; writes=0;
    assert(q1000k_mac_cold_install(NULL,reg,false)==-EINVAL && !writes);
    assert(q1000k_mac_cold_install(sn,NULL,false)==-EINVAL && !writes);
    regs[0x5108/4]=0x55000000; regs[0x511c/4]=0xf1f1f1f1;
    regs[0x5200/4]=0x80000001; regs[0x5204/4]=15;
    regs[0x5800/4]=0x5500001b; regs[0x5014/4]=0x8005;
    assert(!q1000k_mac_cold_install(sn,reg,false));
    int total=writes;
    assert(regs[0x500c/4]==0x01020304 && regs[0x5010/4]==0x05060708);
    for(int i=0;i<9;i++) assert(regs[0x5018/4+i]==get_unaligned_be32(reg+32-4*i));
    assert(regs[0x5108/4]==0x55001600 && regs[0x511c/4]==0xe0e0e0e0);
    assert(!(regs[0x5200/4]&0x80000001) && !(regs[0x5204/4]&15));
    assert(regs[0x5800/4]==0x55000180 && regs[0x5014/4]==0x3ff);
    assert(regs[0x5104/4]==1 && !regs[0x5114/4]);
    assert((regs[0x5284/4]&0xfffff011)==0xff001 && !(written[0x5284/4]&BIT(8)));
    assert(!(written[0x5868/4]&BIT(2)) && !(written[0x582c/4]&BIT(31)));
    for(int n=1;n<=total;n++) {
        reset(); phase=2; fail_write=n;
        assert(q1000k_mac_cold_install(sn,reg,false)==-EIO && writes==n);
    }
    reset(); phase=2; provider_error=-ENODEV;
    assert(q1000k_mac_cold_install(sn,reg,false)==-ENODEV && !writes);
    provider_error=0;
    assert(!q1000k_mac_cold_install(sn,reg,true) && regs[0x5104/4]==7);
    assert(!q1000k_mac_cold_select_keys() && regs[0x53e8/4]==0x1010101 && !delays);
    self_clear=true;
    assert(!q1000k_mac_cold_select_keys() && regs[0x53e8/4]==0x101);
    select_stuck=true; regs[0x5318/4]=0;
    assert(q1000k_mac_cold_select_keys()==-ETIMEDOUT && delays==3000);
    regs[0x5318/4]=~0U;
    assert(q1000k_mac_cold_select_keys()==-EIO);
    writes=0; assert(q1000k_mac_activation_set(5)==-EPERM && !writes);
    owned=true;
    for(int n=0;n<16;n++) {
        if(n==1||n==2||n==4||n==5||n==7)
            assert(!q1000k_mac_activation_set(n) && (regs[0x5104/4]&15)==(u32)n);
        else assert(q1000k_mac_activation_set(n)==-EOPNOTSUPP);
    }
    reset(); assert(q1000k_mac_cold_interrupts(0x1234)==-EPERM && !writes);
    phase=2; assert(!q1000k_mac_cold_interrupts(0x1234) && regs[0x5040/4]==0x1234);
    rx_bench=true; assert(!q1000k_mac_cold_interrupts(~0U) && !regs[0x5040/4]);
    rx_bench=false;
    writes=0; fail_write=2;
    assert(q1000k_mac_cold_interrupts(0x1234)==-EIO);
    reset(); assert(q1000k_mac_profiles_invalidate()==-EPERM);
    assert(q1000k_mac_profile_install(0,1,100)==-EPERM && !writes);
    phase=2; regs[0x511c/4]=0xf1f1f1f1;
    assert(!q1000k_mac_profiles_invalidate() && regs[0x511c/4]==0xf0f0f0f0);
    for(unsigned int i=0;i<4;i++) {
        assert(!q1000k_mac_profile_install(i,i+1,100+i));
        owned=true;
        int before=writes;
        assert(q1000k_mac_profile_matches(i,i+1,100+i)==1 && writes==before);
        assert(q1000k_mac_profile_matches(i,i+1,101+i)==0);
        regs[0x511c/4]^=1U<<(8*i);
        assert(q1000k_mac_profile_matches(i,i+1,100+i)==0);
        regs[0x511c/4]^=1U<<(8*i);
        assert(((regs[0x511c/4]>>(8*i))&0xf1)==((i+1)*16+1));
        assert(((regs[(0x5120+4*(i/2))/4]>>(16*(i&1)))&0xffff)==100+i);
    }
    for(int n=1;n<=2;n++) {
        reset(); phase=2; fail_write=n;
        assert(q1000k_mac_profile_install(0,1,100)==-EIO && writes==n);
        if(n==1) assert(!(regs[0x511c/4]&1));
    }
    reset(); phase=2;
    assert(q1000k_mac_profile_install(4,1,100)==-EINVAL);
    assert(q1000k_mac_profile_install(0,16,100)==-EINVAL);
    assert(q1000k_mac_profile_install(0,1,0)==-EINVAL && !writes);
    return 0;
}
