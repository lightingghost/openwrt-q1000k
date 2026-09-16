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
static int medium_delays, meter_reads, bad_meter;
static struct { u32 reg, end, start, value, delay; } trace[4096];
static unsigned int trace_count;
#define WORD(reg) registers[((reg)&0x1ffff)/4]
#define ORIGINAL(reg) original[((reg)&0x1ffff)/4]
static int next(void) { return ++operations==fail_at ? -EIO : 0; }
int q1000k_phy_callback_context(void) { return context_error; }
static int an7581_pon_phy_status(void) { return next(); }
int q1000k_phy_controller_check(void) { return next(); }
static int an7581_pon_phy_read(u32 reg,u32 *value) {
 int ret=next();
 if(!ret && reg==EN7581_XPON_PMA_RO_RX_FREQDET) {
  /* A monotonic oscillator model exercises the actual coarse/fine branch
   * decisions. The expected selected code is 0x523, with raw bit0 set.
   */
  meter_reads++;
  *value=bad_meter ? (bad_meter==1 ? 0 : 0xffff0001) :
   ((0xa49a+0x523-(WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac)&0x7ff))<<16)|1;
 } else if(!ret) *value=WORD(reg);
 return ret;
}
static int an7581_pon_phy_update(u32 reg,u32 end,u32 start,u32 value) {
 int ret=next(); if(ret) return ret;
 assert(end>=start && end<32 && !(value & ~(GENMASK(end,start)>>start)));
 /* Neither upstream generator nor loopback is ever written. */
 assert(reg!=EN7581_XPON_PMA_ADD_XPON_MODE_1 && reg!=EN7581_XPON_PMA_SS_BIST_1);
 if(reg==EN7581_XPON_PMA_BISTCTL_CONTROL) assert(end==16 && start==16);
 if(reg==EN7581_XPON_PMA_SW_RST_SET) assert(end<=11 && end==start);
 if(reg==EN7581_XGPON_PHY_DBG_CTRL) assert(end<16);
 assert(trace_count<ARRAY_SIZE(trace));
 trace[trace_count++]=(typeof(trace[0])){reg,end,start,value,0};
 u32 *word=&registers[(reg&0x1ffff)/4];
 *word=(*word & ~GENMASK(end,start)) | (value<<start);
 if(++updates==corrupt_update) *word ^= BIT(31); /* detect unrelated-bit corruption too */
 return 0;
}
static void udelay(unsigned int us) {
 assert(us<=500); delays++;
 assert(trace_count<ARRAY_SIZE(trace));
 trace[trace_count++]=(typeof(trace[0])){0,0,0,0,us};
}
static void usleep_range(unsigned int lo,unsigned int hi) {
 assert((lo==1000 || lo==5000 || lo==5500) && hi==lo+100);
 if(lo==5000) long_delays++; else if(lo==1000) medium_delays++;
 assert(trace_count<ARRAY_SIZE(trace));
 trace[trace_count++]=(typeof(trace[0])){0,0,0,0,lo};
}
static bool post_saved, post_bit;
int q1000k_phy_controller_oem_post(bool restore) {
 int ret=next(); if(ret) return ret;
 if(restore) { post_saved=false; post_bit=false; }
 else { assert(!post_saved); post_saved=true; post_bit=true; }
 return 0;
}
/* PRODUCTION */
static void reset(void) {
 memset(registers,0xa4,sizeof(registers));
 registers[(EN7581_XPON_PMA_BISTCTL_CONTROL&0x1ffff)/4]=5;
 registers[(EN7581_XPON_PMA_ADD_XPON_MODE_1&0x1ffff)/4]=0;
 registers[(EN7581_XPON_PMA_SS_BIST_1&0x1ffff)/4]=0;
 WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data)|=0x01010101;
 WORD(EN7581_XPON_PMA_SS_RX_FLL_3)|=0x101;
 WORD(EN7581_XPON_ANA_RG_PXP_CDR_PR_INJ_MODE)|=BIT(24);
 WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en)|=0x01010101;
 WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac)=0xa4010523;
 WORD(EN7581_XPON_PMA_RX_DISB_MODE_3)|=1;
 WORD(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan)=0xa40c1234;
 memcpy(original,registers,sizeof(original));
 repeat_calls=0; repeat_failed=probe_closed=false;
 post_saved=post_bit=post_pending=false; saved_count=probe_writes=0; operations=fail_at=context_error=updates=corrupt_update=delays=long_delays=0;
 medium_delays=meter_reads=bad_meter=0; trace_count=0;
 gpPhyPriv=&phy; phy.wan_sel=10; phy.pma_init_done=1; phy.first_plugin_flag=0;
 phy.trans_tx_status=0; phy.phyCfg.flags.txPowerEnFlag=false;
}
int main(void) {
 for(u32 mode=1;mode<Q1000K_RX_PROBE_COUNT;mode++) {
  reset(); assert(!q1000k_phy_rx_probe(mode) && (saved_count || post_saved) && (updates || post_bit));
  int count=operations, writes=updates;
  u32 rxreg=(EN7581_XGPON_PHY_XG_PON_RX_SYNC_CTRL&0x1ffff)/4;
  u32 dbgreg=(EN7581_XGPON_PHY_DBG_CTRL&0x1ffff)/4;
  assert((registers[rxreg]&BIT(16))==(original[rxreg]&BIT(16)));
  assert((registers[dbgreg]&(BIT(8)|BIT(16)))==(original[dbgreg]&(BIT(8)|BIT(16))));
  if(mode==Q1000K_RX_PROBE_BIT_ORDER) assert(registers[rxreg]==(original[rxreg]^BIT(17)));
  if(mode==Q1000K_RX_PROBE_DESCRAMBLER) assert(registers[dbgreg]==(original[dbgreg]^BIT(9)));

  int clock_cycle=mode==Q1000K_RX_PROBE_OEM_CLOCK_CYCLE || mode==Q1000K_RX_PROBE_OEM_RX_ACQUIRE ||
   mode==Q1000K_RX_PROBE_COMBINED_AUTO || mode==Q1000K_RX_PROBE_PRCAL_RERUN;
  if(mode<=Q1000K_RX_PROBE_PRCAL_RERUN) assert(long_delays==(mode==Q1000K_RX_PROBE_PRCAL_RERUN ? 16 :
   (mode==Q1000K_RX_PROBE_TDC_DELAY || mode==Q1000K_RX_PROBE_OEM_ORDER || clock_cycle)?1:0));
  if(mode<=Q1000K_RX_PROBE_PRCAL_RERUN) assert(medium_delays==(mode==Q1000K_RX_PROBE_PRCAL_RERUN ? 2 : clock_cycle?1:0));
  if(mode==Q1000K_RX_PROBE_CDR_AUTO_RELEASE) {
   assert((WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data)&0x01010101)==0x101);
   assert(updates==6 && trace_count==8);
   assert(trace[0].start==16 && trace[0].value==0);
   assert(trace[1].start==24 && trace[1].value==0);
   assert(trace[2].start==0 && trace[2].value==0);
   assert(trace[3].start==8 && trace[3].value==0 && trace[4].delay==100);
   assert(trace[5].start==0 && trace[5].value==1);
   assert(trace[6].start==8 && trace[6].value==1 && trace[7].delay==200);
  }
  if(clock_cycle && mode!=Q1000K_RX_PROBE_COMBINED_AUTO) {
   assert((WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data)&0x01010101)==0x101);
   assert((WORD(EN7581_XPON_PMA_SW_RST_SET)&0x7f)==0x7f);
   assert((WORD(EN7581_XPON_PMA_SW_RST_SET)&~0x7f)==(ORIGINAL(EN7581_XPON_PMA_SW_RST_SET)&~0x7f));
   assert(WORD(EN7581_XPON_PMA_SS_RX_FLL_3)==ORIGINAL(EN7581_XPON_PMA_SS_RX_FLL_3));
  }
  if(mode==Q1000K_RX_PROBE_OEM_CLOCK_CYCLE) {
   /* OEM ordering differs materially from the old probe. It turns TDC
    * off without PCW pulses, settles, holds/releases known reset bits in
    * descending order, then enters L2D and waits before TDC acquisition.
    */
   assert(trace_count==65 && trace[3].delay==1000 && trace[8].delay==100);
   assert(trace[9].reg==EN7581_XPON_PMA_SS_LCPLL_PWCTL_SETTING_0);
   assert(trace[9].start==24 && trace[9].value==1 && trace[10].delay==100);
   for(int bit=6;bit>=0;bit--) {
    assert(trace[17-bit].reg==EN7581_XPON_PMA_SW_RST_SET);
    assert(trace[17-bit].start==(u32)bit && trace[17-bit].value==0);
    assert(trace[25-bit].reg==EN7581_XPON_PMA_SW_RST_SET);
    assert(trace[25-bit].start==(u32)bit && trace[25-bit].value==1);
   }
   assert(trace[18].delay==10 && trace[28].delay==200 && trace[40].delay==5000);
   assert(trace[44].delay==6 && trace[55].delay==500);
   assert(trace[58].delay==10 && trace[61].delay==100 && trace[64].delay==1);
  }
  if(mode==Q1000K_RX_PROBE_CDR_INTERNAL_AUTO || mode==Q1000K_RX_PROBE_COMBINED_AUTO) {
   assert((WORD(EN7581_XPON_PMA_RX_DISB_MODE_0)&0x01010000)==0x01010000);
   assert(!(WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_lpf_lck2data)&0x01000100));
  }
  if(mode==Q1000K_RX_PROBE_PRCAL_FINALIZE || mode==Q1000K_RX_PROBE_OEM_RX_ACQUIRE ||
     mode==Q1000K_RX_PROBE_COMBINED_AUTO || mode==Q1000K_RX_PROBE_PRCAL_RERUN) {
   assert(!(WORD(EN7581_XPON_ANA_RG_PXP_CDR_PR_INJ_MODE)&BIT(24)));
   assert((WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_lpf_c_en)&0x01010101)==0x01010000);
   assert(!(WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac)&BIT(16)));
   assert(WORD(EN7581_XPON_PMA_SS_RX_FLL_b)&1);
   u32 expected=mode==Q1000K_RX_PROBE_PRCAL_RERUN ? 0x523 :
    ORIGINAL(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac)&0x7ff;
   assert((WORD(EN7581_XPON_PMA_SS_RX_FLL_1)&0x7ff)==expected);
   assert((WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_pieye_pwdb)&0x01010000)==0x01010000);
  }
  if(mode==Q1000K_RX_PROBE_FLL_AUTO || mode==Q1000K_RX_PROBE_COMBINED_AUTO)
   assert(WORD(EN7581_XPON_PMA_SS_RX_FLL_3)==(ORIGINAL(EN7581_XPON_PMA_SS_RX_FLL_3)&~1U));
  if(mode==Q1000K_RX_PROBE_RX_SEQUENCE_AUTO || mode==Q1000K_RX_PROBE_COMBINED_AUTO)
   assert((WORD(EN7581_XPON_PMA_RX_CTRL_SEQUENCE_DISB_CTRL_1)&0x01010101)==0x01010101);
  if(mode==Q1000K_RX_PROBE_POST_EYE_READY || mode==Q1000K_RX_PROBE_OEM_RX_ACQUIRE || mode==Q1000K_RX_PROBE_COMBINED_AUTO) {
   assert(!(WORD(EN7581_XPON_PMA_RX_DISB_MODE_3)&1));
   assert(WORD(EN7581_XPON_PMA_RX_FORCE_MODE_3)&1);
  }
  if(mode==Q1000K_RX_PROBE_OEM_PEAKING || mode==Q1000K_RX_PROBE_OEM_RX_ACQUIRE || mode==Q1000K_RX_PROBE_COMBINED_AUTO) {
   assert(((WORD(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan)>>16)&15)==
    ((ORIGINAL(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan)>>17)&7));
   assert(WORD(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan)&BIT(24));
  }
  if(mode==Q1000K_RX_PROBE_OEM_RX_ACQUIRE || mode==Q1000K_RX_PROBE_COMBINED_AUTO)
   assert((WORD(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl)&0x103)==0x101);
  if(mode==Q1000K_RX_PROBE_PRCAL_RERUN) assert(meter_reads==16);
  if(mode>=Q1000K_RX_PROBE_OEM_ANALOG && mode<=Q1000K_RX_PROBE_OEM_EYE_7 && mode!=Q1000K_RX_PROBE_OEM_FULL_RESET) {
   assert((WORD(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl)&0x103)==0x101);
   unsigned peak=mode>=Q1000K_RX_PROBE_OEM_EYE_0 ? mode-Q1000K_RX_PROBE_OEM_EYE_0 : 4;
   assert(((WORD(EN7581_XPON_PMA_rg_da_pxp_jcpll_sdm_scan)>>16)&15)==peak);
  }
  if(mode==Q1000K_RX_PROBE_OEM_FULL_RESET || mode==Q1000K_RX_PROBE_OEM_CAL_RESET ||
     mode==Q1000K_RX_PROBE_OEM_CAL_AUTO || (mode>=Q1000K_RX_PROBE_OEM_EYE_0 && mode<=Q1000K_RX_PROBE_OEM_EYE_7)) {
   assert((WORD(EN7581_XPON_PMA_SW_RST_SET)&0xfff)==0xfff);
   assert((WORD(EN7581_XPON_PMA_SW_RST_SET)&~0xfffU)==(ORIGINAL(EN7581_XPON_PMA_SW_RST_SET)&~0xfffU));
  }
  if(mode==Q1000K_RX_PROBE_OEM_POST_INIT || mode==Q1000K_RX_PROBE_OEM_POST_CAL) assert(post_saved && post_bit);
  if(mode==Q1000K_RX_PROBE_OEM_RESET_REPEAT) {
   for(unsigned n=1;n<Q1000K_RX_REPEAT_LIMIT;n++) assert(!q1000k_phy_rx_probe(mode));
   assert(repeat_calls==6);
  }
  assert(q1000k_phy_rx_probe(mode)==-EBUSY);
  assert(!q1000k_phy_rx_probe_cleanup() && !saved_count && !post_saved && !post_bit && !memcmp(registers,original,sizeof(original)));
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
   assert(q1000k_phy_rx_probe_cleanup()==-EIO && (saved_count || post_pending));
   fail_at=0; assert(!q1000k_phy_rx_probe_cleanup() && !memcmp(registers,original,sizeof(original)));
  }
 }
 /* Every failure in a later pass stops the series and preserves originals. */
 reset(); assert(!q1000k_phy_rx_probe(Q1000K_RX_PROBE_OEM_RESET_REPEAT));
 int first_ops=operations;
 for(int n=1;n<=first_ops;n++) {
  reset(); assert(!q1000k_phy_rx_probe(Q1000K_RX_PROBE_OEM_RESET_REPEAT));
  operations=0; fail_at=n;
  assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_OEM_RESET_REPEAT)==-EIO);
  fail_at=0;
  if(repeat_failed) assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_OEM_RESET_REPEAT)==-EBUSY);
  assert(!q1000k_phy_rx_probe_cleanup() && !memcmp(registers,original,sizeof(original)));
  assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_OEM_RESET_REPEAT)==-EBUSY);
 }
 reset(); context_error=-EPERM; assert(q1000k_phy_rx_probe(1)==-EPERM && !updates);
 reset(); phy.trans_tx_status=1; assert(q1000k_phy_rx_probe(1)==-EACCES && !updates);
 reset(); phy.phyCfg.flags.txPowerEnFlag=true; assert(q1000k_phy_rx_probe(1)==-EACCES && !updates);
 reset(); phy.pma_init_done=0; assert(q1000k_phy_rx_probe(1)==-EAGAIN && !updates);
 reset(); registers[(EN7581_XPON_PMA_BISTCTL_CONTROL&0x1ffff)/4]|=BIT(8);
 assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_CHECKER)==-EACCES && !updates);
 reset(); WORD(EN7581_XPON_PMA_BISTCTL_CONTROL)|=BIT(8);
 assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_CHECKER_DARK)==-EACCES && !updates);
 reset(); WORD(EN7581_XPON_PMA_rg_force_da_pxp_cdr_pr_idac)&=~0x7ffU;
 assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_PRCAL_FINALIZE)==-ERANGE && !updates);
 for(int invalid=1;invalid<=2;invalid++) {
  reset(); bad_meter=invalid;
  assert(q1000k_phy_rx_probe(Q1000K_RX_PROBE_PRCAL_RERUN)==-ERANGE && meter_reads==1);
  assert(!q1000k_phy_rx_probe_cleanup() && !memcmp(registers,original,sizeof(original)));
 }
 return 0;
}
