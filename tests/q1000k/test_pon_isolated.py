"""Disconnected transmitter: bounded ownership and every fixed recipe."""
from pathlib import Path
import unittest
import re
from pon_test_utils import run_c
from test_pon_phy_lifecycle import production_source, PHY, BSP
from test_pon_discovery import C
from argparse import Namespace

class IsolatedTests(unittest.TestCase):
    def test_no_identity_no_physical_controls(self):
        args=Namespace(suite='isolated', identity=None, rx_only=False, physical_only=False, skip_physical=False, cases=None)
        cases=C.discovery_plan(args)
        self.assertEqual(len(cases),18)
        self.assertTrue(all(c['mode']=='isolated' and c['name'] not in C.PHYSICAL for c in cases))
        self.assertEqual(len({c['ids'][0] for c in cases}),18)
        args.identity=Path('/private')
        with self.assertRaises(ValueError): C.discovery_plan(args)

    def test_actual_finite_phy_owner(self):
        isolated=(PHY/'src/q1000k_phy_isolated.h').read_text()
        isolated=isolated.replace('#include "q1000k_phy_output.h"', (PHY/'src/q1000k_phy_output.h').read_text())
        source, regs=production_source(extra=isolated)
        source+='\n'+isolated
        fixture=Path(__file__).with_name('pon_phy_lifecycle_fixture.c').read_text()
        # This suite supplies a reset mock that accepts both FIRST_PLUG_IN
        # and PLUG_OUT. Replace the normal-service-only mock in the shared
        # lifecycle fixture while retaining its forward declaration.
        fixture=fixture.replace('#define PLUG_OUT 0', '#define PLUG_OUT 2')
        fixture, removed=re.subn(
            r'\nstatic void fiber_plug_reset\(int operation, int mode\)\n\{.*?\n\}\n',
            '\n', fixture, count=1, flags=re.S)
        self.assertEqual(removed, 1)
        extra=r"""
#include "en7573_mpd.h"
#include "en7573_output.h"
#define PAGE_SIZE 4096
#define scnprintf snprintf
#define module_param(a,b,c)
#define MODULE_PARM_DESC(a,b)
#define module_param_cb(a,b,c,d)
#define FIRST_PLUG_IN 0
struct kernel_param { int unused; };
struct kernel_param_ops { int (*get)(char *, const struct kernel_param *); int (*set)(const char *, const struct kernel_param *); };
static int kstrtouint(const char *s, int base, unsigned int *v) { char *end; *v=strtoul(s,&end,base); return *end ? -EINVAL : 0; }
static u64 ktime_get_boottime_ns(void) { return (u64)jiffies*1000000; }
static unsigned int slept, light_after, clock_calls;
static void msleep(unsigned int ms) { slept+=ms; jiffies+=ms; if(light_after && slept>=light_after) controller_los=false; }
static void fiber_plug_reset(int what, int mode) { assert(!controller.tx); assert(what==FIRST_PLUG_IN || what==PLUG_OUT); clock_calls++; }
struct en7573_tx_recipe { u32 words[8]; unsigned int count; };
static int mpd_calls, mpd_fail;
static int q1000k_pon_measure_mpd(struct q1000k_pon *p, bool active, struct en7573_mpd *s) {
 assert(p->held && !controller_inhibit && controller_los); mpd_calls++;
 memset(s,0,sizeof(*s)); s->active=active; s->restored=true;
 if(active) { msleep(50); msleep(5); } else msleep(1);
 return mpd_calls==mpd_fail ? -EIO : 0;
}
static int output_calls, output_fail, output_hold_calls, board_disabled=1;
static int q1000k_pon_output_gates(struct q1000k_pon *p,unsigned int gates) {
 assert(p->held && !controller_inhibit); p->tx=!!(gates&1); board_disabled=!(gates&2); return 0;
}
static int q1000k_pon_output_sample(struct q1000k_pon *p,struct en7573_output_sample *s) {
 assert(p->held && controller_los); output_calls++; memset(s,0,sizeof(*s)); s->valid=(1U<<30)-1;
 s->board_disabled=board_disabled; s->values[12]=p->tx ? 0 : 512; msleep(30);
 return output_calls==output_fail ? -EIO : 0;
}
static int q1000k_pon_output_hold(struct q1000k_pon *p,struct en7573_output_hold *s,bool restore) {
 assert(p->held && !p->tx); output_hold_calls++; s->saved_valid=!restore; msleep(55); return 0;
}
static int recipe_error, recipe_calls, last_recipe;
static int q1000k_pon_tx_recipe(struct q1000k_pon *p, unsigned int id, struct en7573_tx_recipe *s, bool restore)
{ assert(p->held && !p->tx); if(controller_inhibit) return -EACCES;
  if(restore) { s->count=0; return 0; } recipe_calls++; last_recipe=id;
  if(recipe_error) return recipe_error; s->count=8; return 0; }
"""
        fixture=fixture.replace('/* PRODUCTION */',extra+'\n'+source)
        fixture=fixture.replace('/* REGISTERS */',regs)
        fixture=fixture.replace('int main(void)', 'int original_main(void)',1)
        fixture+=r"""
static void fresh_isolated(void) {
 reset(); isolated_tx_bench=true; memset(&qiso,0,sizeof(qiso)); memset(&qiso_mpd,0,sizeof(qiso_mpd)); mpd_calls=mpd_fail=0;
 controller_inhibit=false; controller_los=true; slept=light_after=clock_calls=0;
 recipe_error=recipe_calls=last_recipe=0; memset(&qout,0,sizeof(qout));
 output_calls=output_fail=output_hold_calls=0; board_disabled=1;
 assert(!q1000k_phy_init());
 regs[(EN7581_XGPON_PHY_SFP_STA&0x1ffff)/4]=EN7581_XGPON_PHY_SFP_RX_LOS_ST;
}
int main(void) {
 char buffer[PAGE_SIZE];
 for(unsigned int id=1;id<=18;id++) {
  fresh_isolated();
  assert(q1000k_phy_start()==-EPERM && !allocated_irq);
  assert(!qiso_run(id));
  assert(qiso.id==id && qiso.used && qiso.valid==7 && qiso.restored && qiso.tx_off && !controller.tx);
  assert(slept==(id>=3 ? 5350U : 5000U));
  assert(clock_calls==(id>=3 ? 2U : 0U));
  assert(qiso.dark_checks==22);
  assert(qiso.enabled_ns || id==17);
  if(id!=17) assert(qiso.disabled_ns-qiso.enabled_ns==5000000000ULL);
  assert(!qphy_active && !allocated_irq && !gpPhyPriv->event_poll_timer.armed);
  assert(qiso_run(id)==-EALREADY);
  int n=qiso_get(buffer,NULL); assert(n>0 && n<PAGE_SIZE);
  assert(strstr(buffer,"\"restored\":true") && strstr(buffer,"\"tx_off\":true"));
 }
 fresh_isolated(); isolated_tx_bench=false; assert(qiso_run(1)==-EPERM && !writes);
 fresh_isolated(); controller_los=false; assert(qiso_run(3)==-ENOLINK && !qiso.used && !controller.tx);
 fresh_isolated(); controller_inhibit=true; assert(qiso_run(3)==-EACCES && !controller.tx);
 fresh_isolated(); light_after=1000; assert(qiso_run(3)==-ENOLINK && !controller.tx && qiso.tx_off && qiso.restored);
 assert(slept==1100); /* 350ms setup + 3 x 250ms checks */
 fresh_isolated(); recipe_error=-ENODATA; assert(qiso_run(10)==-ENODATA && qiso.valid==5 && qiso.restored && !controller.tx);
 /* Inject each MMIO read/write failure across preparation, emission and
  * restoration. No failing operation can return success or leave TX on. */
 fresh_isolated(); assert(!qiso_run(9)); unsigned int nr=reads,nw=writes;
 for(unsigned int i=1;i<=nr;i++) {
  fresh_isolated(); fail_read=i; assert(qiso_run(9)<0 && !controller.tx);
 }
 for(unsigned int i=1;i<=nw;i++) {
  fresh_isolated(); fail_write=i; assert(qiso_run(9)<0 && !controller.tx);
 }
 for(unsigned int id=19;id<=31;id++) {
  fresh_isolated(); assert(!qiso_run(id));
  assert(qiso.restored && qiso.tx_off && !controller.tx && mpd_calls==3);
  assert((id==20)==(qiso.enabled_ns==0));
  assert(qiso_mpd.count==3 && qiso_mpd.begin[0]<qiso.window_ns && qiso_mpd.begin[1]>=qiso.window_ns);
  assert(qiso_mpd.end[1]<=qiso.disabled_ns && qiso_mpd.begin[2]>=qiso.disabled_ns);
  if(id!=20) assert(qiso.disabled_ns-qiso.enabled_ns==5000000000ULL);
  /* Largest u32 values must still fit the sysfs page. */
  for(int p=0;p<3;p++) { memset(qiso_mpd.sample[p].values,0xff,sizeof(qiso_mpd.sample[p].values));
    for(int i=0;i<3;i++) qiso_mpd.sample[p].saved[i]=qiso_mpd.sample[p].selected[i]=~0U; }
  int n=qiso_mpd_get(buffer,NULL); assert(n>0 && n<PAGE_SIZE && strstr(buffer,"\"mpd_version\":1"));
 }
 for(int i=1;i<=3;i++) { fresh_isolated(); mpd_fail=i; assert(qiso_run(21)==-EIO && !controller.tx && qiso.tx_off); }
 for(unsigned int id=32;id<=48;id++) {
  fresh_isolated(); assert(!qiso_run(id));
  assert(qiso.restored && qiso.tx_off && !controller.tx && board_disabled && output_calls==15);
  assert(qiso.valid==7 && qiso.used && !qphy_active && !allocated_irq);
  assert(output_hold_calls==(id>=35 ? 2 : 0));
  assert(qiso.disabled_ns-qiso.window_ns==5000000000ULL);
  for(unsigned int p=0;p<3;p++) {
   assert(qout.count[p]==5);
   for(unsigned int r=0;r<5;r++) {
    assert(qout.begin[p][r]<qout.end[p][r]);
    if(p==0) assert(qout.end[p][r]<=qiso.window_ns);
    if(p==1) assert(qout.begin[p][r]>=qiso.window_ns && qout.end[p][r]<=qiso.disabled_ns);
    if(p==2) assert(qout.begin[p][r]>=qiso.disabled_ns);
    memset(qout.sample[p][r].values,0xff,sizeof(qout.sample[p][r].values));
    qout.begin[p][r]=qout.end[p][r]=~(u64)0;
   }
   int n=qout_get(buffer,p); assert(n>0 && n<PAGE_SIZE-1 && buffer[n-2]=='}');
  }
 }
 for(int i=1;i<=15;i++) {
  fresh_isolated(); output_fail=i; assert(qiso_run(35)==-EIO && !controller.tx && board_disabled);
 }
 fresh_isolated(); light_after=2200; assert(qiso_run(35)==-ENOLINK && !controller.tx && board_disabled);
 fresh_isolated(); assert(!qiso_run(41)); nr=reads; nw=writes;
 for(unsigned int i=1;i<=nr;i++) { fresh_isolated(); fail_read=i; assert(qiso_run(41)<0 && !controller.tx); }
 for(unsigned int i=1;i<=nw;i++) { fresh_isolated(); fail_write=i; assert(qiso_run(41)<0 && !controller.tx); }
 reset(); isolated_tx_bench=false;
 puts("Isolated TX: 18 recipes, finite window, LOS guards, disabled callbacks and all MMIO failures passed");
 return 0;
}
"""
        run_c(fixture, flags=['-Wno-sign-compare', '-Wno-misleading-indentation','-I',str(BSP/'include'), '-I',str(Path(__file__).resolve().parents[2]/'package/kernel/q1000k-pon-control/src')])

if __name__=='__main__': unittest.main()
