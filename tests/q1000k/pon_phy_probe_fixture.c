// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#include <q1000k_phy_api.h>
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define pr_info(...) ((void)0)
#define PHY_DISABLE 0
#define SCU_WAN_CONF_REG_WAN_SEL_XGSPON 10
/* REGISTERS */
static struct {
 int wan_sel,trans_tx_status,pma_init_done,first_plugin_flag;
 struct { struct { bool txPowerEnFlag; } flags; } phyCfg;
} phy, *gpPhyPriv=&phy;
static u32 registers[0x8000], original[0x8000];
static int operations, fail_at, context_error, updates, corrupt_update, delays, long_delays;
static int next(void) { return ++operations==fail_at ? -EIO : 0; }
int q1000k_phy_callback_context(void) { return context_error; }
static int an7581_pon_phy_status(void) { return next(); }
int q1000k_phy_controller_check(void) { return next(); }
static int an7581_pon_phy_read(u32 reg,u32 *value) {
 int ret=next(); if(!ret) *value=registers[(reg&0x1ffff)/4]; return ret;
}
static int an7581_pon_phy_update(u32 reg,u32 end,u32 start,u32 value) {
 int ret=next(); if(ret) return ret;
 assert(end>=start && end<32 && !(value & ~(GENMASK(end,start)>>start)));
 /* Neither upstream generator nor loopback is ever written. */
 assert(reg!=EN7581_XPON_PMA_ADD_XPON_MODE_1 && reg!=EN7581_XPON_PMA_SS_BIST_1);
 if(reg==EN7581_XPON_PMA_BISTCTL_CONTROL) assert(end==16 && start==16);
 if(reg==EN7581_XPON_PMA_SW_RST_SET) assert(end<=6);
 if(reg==EN7581_XGPON_PHY_DBG_CTRL) assert(end<16);
 u32 *word=&registers[(reg&0x1ffff)/4];
 *word=(*word & ~GENMASK(end,start)) | (value<<start);
 if(++updates==corrupt_update) *word ^= BIT(31); /* detect unrelated-bit corruption too */
 return 0;
}
static void udelay(unsigned int us) { assert(us<=500); delays++; }
static void usleep_range(unsigned int lo,unsigned int hi) { assert(lo==5000 && hi==5100); long_delays++; }
/* PRODUCTION */
static void reset(void) {
 memset(registers,0xa4,sizeof(registers));
 registers[(EN7581_XPON_PMA_BISTCTL_CONTROL&0x1ffff)/4]=5;
 registers[(EN7581_XPON_PMA_ADD_XPON_MODE_1&0x1ffff)/4]=0;
 registers[(EN7581_XPON_PMA_SS_BIST_1&0x1ffff)/4]=0;
 memcpy(original,registers,sizeof(original));
 saved_count=probe_writes=0; operations=fail_at=context_error=updates=corrupt_update=delays=long_delays=0;
 gpPhyPriv=&phy; phy.wan_sel=10; phy.pma_init_done=1; phy.first_plugin_flag=0;
 phy.trans_tx_status=0; phy.phyCfg.flags.txPowerEnFlag=false;
}
int main(void) {
 for(u32 mode=1;mode<Q1000K_RX_PROBE_COUNT;mode++) {
  reset(); assert(!q1000k_phy_rx_probe(mode) && saved_count && updates);
  int count=operations, writes=updates;
  u32 rxreg=(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL&0x1ffff)/4;
  u32 dbgreg=(EN7581_XGPON_PHY_DBG_CTRL&0x1ffff)/4;
  assert((registers[rxreg]&BIT(16))==(original[rxreg]&BIT(16)));
  assert((registers[dbgreg]&(BIT(8)|BIT(16)))==(original[dbgreg]&(BIT(8)|BIT(16))));
  if(mode==Q1000K_RX_PROBE_BIT_ORDER) assert(registers[rxreg]==(original[rxreg]^BIT(17)));
  if(mode==Q1000K_RX_PROBE_DESCRAMBLER) assert(registers[dbgreg]==(original[dbgreg]^BIT(9)));

  assert(long_delays==((mode==Q1000K_RX_PROBE_TDC_DELAY || mode==Q1000K_RX_PROBE_OEM_ORDER)?1:0));
  assert(q1000k_phy_rx_probe(mode)==-EBUSY);
  assert(!q1000k_phy_rx_probe_cleanup() && !saved_count && !memcmp(registers,original,sizeof(original)));
  assert(q1000k_phy_rx_probe(mode)==-EBUSY); /* lifetime attempt remains consumed */
  for(int n=1;n<=count;n++) {
   reset(); fail_at=n; assert(q1000k_phy_rx_probe(mode)==-EIO);
   fail_at=0; assert(!q1000k_phy_rx_probe_cleanup() && !saved_count);
   assert(!memcmp(registers,original,sizeof(original)));
  }
  for(int n=1;n<=writes;n++) {
   reset(); corrupt_update=n; assert(q1000k_phy_rx_probe(mode)==-EIO);
  }
  reset(); assert(!q1000k_phy_rx_probe(mode)); operations=0;
  assert(!q1000k_phy_rx_probe_cleanup()); count=operations;
  for(int n=1;n<=count;n++) {
   reset(); assert(!q1000k_phy_rx_probe(mode)); operations=0; fail_at=n;
   assert(q1000k_phy_rx_probe_cleanup()==-EIO && saved_count);
   fail_at=0; assert(!q1000k_phy_rx_probe_cleanup() && !memcmp(registers,original,sizeof(original)));
  }
 }
 reset(); context_error=-EPERM; assert(q1000k_phy_rx_probe(1)==-EPERM && !updates);
 reset(); phy.trans_tx_status=1; assert(q1000k_phy_rx_probe(1)==-EACCES && !updates);
 reset(); phy.phyCfg.flags.txPowerEnFlag=true; assert(q1000k_phy_rx_probe(1)==-EACCES && !updates);
 reset(); phy.pma_init_done=0; assert(q1000k_phy_rx_probe(1)==-EAGAIN && !updates);
 reset(); registers[(EN7581_XPON_PMA_BISTCTL_CONTROL&0x1ffff)/4]|=BIT(8);
 assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_CHECKER)==-EACCES && !updates);
 return 0;
}
