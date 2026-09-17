"""Output gate correlation: actual sampling/hold code and evidence validation."""
from argparse import Namespace
import copy
from pathlib import Path
import unittest
from pon_test_utils import run_c
from test_pon_discovery import C

ROOT = Path(__file__).resolve().parents[2]


class OutputTests(unittest.TestCase):
    def test_hardware_read_order_and_fixed_monitor_restoration(self):
        src = ROOT/'package/kernel/q1000k-pon-control/src'
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "en7573.h"
struct model { unsigned char bytes[0x3020]; unsigned int calls,fail,writes,delay,hw_reads; };
static void word(struct model *m,unsigned int reg,u32 v) { for(int i=0;i<4;i++) m->bytes[reg+i]=v>>(8*i); }
static u32 value(struct model *m,unsigned int reg) { u32 v=0; for(int i=0;i<4;i++) v|=(u32)m->bytes[reg+i]<<(8*i); return v; }
static int rd(void *ctx,u8 dev,u16 reg,u8 *data,size_t n) {
 struct model *m=ctx; assert(dev==0x51 && reg+n<=sizeof(m->bytes));
 if(++m->calls==m->fail) return -EREMOTEIO;
 if(reg==0x3a4) word(m,reg,(29500+ m->hw_reads++)<<7);
 memcpy(data,m->bytes+reg,n); return 0;
}
static int wr(void *ctx,u8 dev,u16 reg,const u8 *data,size_t n) {
 struct model *m=ctx; assert(dev==0x51 && n==4 && (reg==0x130 || reg==0x208 || reg==0x120));
 m->writes++; memcpy(m->bytes+reg,data,n); /* include a write that applies but reports failure */
 return ++m->calls==m->fail ? -EREMOTEIO : 0;
}
static void delay(void *ctx,unsigned int ms) { struct model *m=ctx; assert(ms==50 || ms==5); m->delay+=ms; }
static void fresh(struct model *m) {
 memset(m,0,sizeof(*m)); word(m,0x3018,1); word(m,0x3e0,512);
 word(m,0x130,0x80002301); word(m,0x208,0x80000021); word(m,0x120,0x400001);
 m->bytes[0xf0]=0x34; m->bytes[0xf1]=0x12; m->bytes[0xfe]=3; m->bytes[0x67]=1;
}
int main(void) {
 struct model m; struct en7573_output_sample s; struct en7573_output_hold h={0};
 struct en7573_io io={.ctx=&m,.read=rd,.write=wr,.delay_ms=delay};
 fresh(&m); assert(!en7573_output_sample(&io,&s) && !m.writes && m.hw_reads==2);
 assert(s.values[1]==(29500U<<7) && s.values[4]==(29501U<<7));
 assert(s.values[2]==0x1234 && s.values[3]==3 && s.values[7]==1 && s.valid==((1U<<30)-1));
 for(unsigned int fail=1;fail<=30;fail++) { fresh(&m); m.fail=fail; assert(en7573_output_sample(&io,&s)==-EREMOTEIO && !m.writes); assert(s.valid!=((1U<<30)-1)); }
 fresh(&m); assert(!en7573_output_hold(&io,&h,false)); unsigned int calls=m.calls;
 assert(h.saved_valid && m.delay==55 && m.writes==3);
 assert((value(&m,0x130)&0x3f00)==0x2400 && (value(&m,0x208)&0x70)==0x40 && (value(&m,0x120)&(1U<<26)));
 assert(en7573_output_hold(&io,&h,false)==-EBUSY);
 for(int i=0;i<15;i++) assert(!en7573_output_sample(&io,&s));
 assert(m.writes==3); /* no mux/gain re-selection across the off/on/off samples */
 assert(!en7573_output_hold(&io,&h,true) && !h.saved_valid);
 assert(value(&m,0x130)==0x80002301 && value(&m,0x208)==0x80000021 && value(&m,0x120)==0x400001);
 for(unsigned int fail=1;fail<=calls;fail++) {
  fresh(&m); memset(&h,0,sizeof(h)); m.fail=fail;
  assert(en7573_output_hold(&io,&h,false)==-EREMOTEIO);
  m.fail=0; assert(!en7573_output_hold(&io,&h,true));
  assert(value(&m,0x130)==0x80002301 && value(&m,0x208)==0x80000021 && value(&m,0x120)==0x400001);
 }
 fresh(&m); memset(&h,0,sizeof(h)); assert(!en7573_output_hold(&io,&h,false));
 unsigned int begin=m.calls; assert(!en7573_output_hold(&io,&h,true)); unsigned int restores=m.calls-begin;
 for(unsigned int fail=1;fail<=restores;fail++) {
  fresh(&m); memset(&h,0,sizeof(h)); assert(!en7573_output_hold(&io,&h,false));
  m.fail=m.calls+fail; assert(en7573_output_hold(&io,&h,true)==-EREMOTEIO && h.saved_valid);
 }
 puts("Output: hardware/mailbox endian, bracketed reads, fixed monitor, all setup/restore transfer failures passed");
 return 0;
}
''', flags=['-I'+str(src), str(src/'en7573.c')])

    @staticmethod
    def evidence(test_id=35):
        gates = 1 if test_id in (32,36) else 2 if test_id in (34,37) else 0 if test_id==48 else 3
        owner = dict(window_ns=2_000_000_000, disabled_ns=7_000_000_000,
                     enabled_ns=2_000_000_000 if gates&1 else 0)
        records=[]
        for phase in range(3):
            rows=[]
            for i in range(5):
                v=[0]*30
                active=gates if phase==1 else 0
                v[0]=v[5]=int(active==3)
                v[1]=v[4]=(29500+int(active==3)*100)<<7
                v[2]=v[1]>>7; v[3]=1 if active==3 else 3
                v[6]=0x3680 if active==3 else 0; v[7]=1
                v[12]=0 if active&1 else 512
                v[17]=1<<26; v[20]=0x2400; v[22]=0x40
                v[28]=34364+(46885<<16); v[29]=29491
                t=[1_000_000_000,2_000_000_000,8_000_000_000][phase]+i*100_000_000
                rows.append(dict(v=v,valid=(1<<30)-1,error=0,board_disabled=int(not active&2),begin_ns=t,end_ns=t+50_000_000))
            records.append(dict(output_version=1,id=test_id,phase=phase,gates=gates,fixed_monitor=test_id>=35,samples=rows))
        return records,owner

    def test_matrix_and_decoding(self):
        args=Namespace(suite='output',identity=None,rx_only=False,physical_only=False,skip_physical=False,cases=None)
        self.assertEqual([c['name'] for c in C.discovery_plan(args)],[f'isolated-{i}' for i in range(32,49)])
        for test_id in range(32,49):
            rows,owner=self.evidence(test_id)
            r,state=C.output_result(rows,test_id,owner)
            self.assertNotEqual(state,'containment-failure')
            self.assertTrue(r['fixed_settings_verified'])
        rows,owner=self.evidence()
        r,state=C.output_result(rows,35,owner)
        self.assertEqual(state,'gate-assertion-observed')
        self.assertEqual(r['by_phase'][1]['monitor_current_uA_oem'],[173,173])
        self.assertIsNone(r['by_phase'][0]['monitor_current_uA_oem'])
        self.assertFalse(r['conversion_ready_verified'])
        for idx in (1,2):
            bad=copy.deepcopy(rows);bad[1]['samples'][idx]['v'][20]^=0x100
            result,_=C.output_result(bad,35,owner)
            self.assertFalse(result['fixed_settings_verified'])
            self.assertIsNone(result['decoded'][1][idx]['monitor_current_uA_oem'])
        for field,value in [('board_disabled',1),('valid',0),('end_ns',99),('error',-5),('v',[0]*29)]:
            bad=copy.deepcopy(rows);bad[1]['samples'][0][field]=value
            self.assertEqual(C.output_result(bad,35,owner)[1],'containment-failure')
        for bad in ([],rows[:2],rows+[rows[0]],list(reversed(rows))):
            self.assertEqual(C.output_result(bad,35,owner)[1],'containment-failure')


if __name__ == '__main__': unittest.main()
