"""Actual OEM monitor transaction, fault restoration, and collector evidence."""
from pathlib import Path
import copy
import unittest
from argparse import Namespace
from pon_test_utils import run_c
from test_pon_discovery import C, ROOT

class MonitorTests(unittest.TestCase):
    def test_source_transaction_and_every_bus_failure(self):
        src=ROOT/'package/kernel/q1000k-pon-control/src'
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "en7573.h"
struct model { u32 regs[0x500/4]; unsigned int calls, fail, writes, slept; bool corrupt; };
static int rd(void *ctx,u8 dev,u16 reg,u8 *data,size_t n) {
 struct model *m=ctx; assert(dev==0x51 && reg<0x500); m->calls++;
 if(m->calls==m->fail) return -EREMOTEIO;
 u32 v=m->regs[reg/4];
 if(reg==0x33c) v=(m->regs[0x120/4]&(1U<<26)) ? 0x12345678 : 0x22334455;
 if(n==2) { assert(reg==0xf0 || reg==0x66 || reg==0x64 || reg==0x6a); data[0]=0x12; data[1]=0x34; }
 else { assert(n==4 && !(reg&3)); for(unsigned i=0;i<4;i++) data[i]=v>>(i*8); }
 return 0;
}
static int wr(void *ctx,u8 dev,u16 reg,const u8 *data,size_t n) {
 struct model *m=ctx; assert(dev==0x51 && n==4);
 assert(reg==0x130 || reg==0x208 || reg==0x120);
 m->calls++; if(m->calls==m->fail) return -EREMOTEIO;
 m->writes++; m->regs[reg/4]=(u32)data[0]|(u32)data[1]<<8|(u32)data[2]<<16|(u32)data[3]<<24;
 if(m->corrupt) m->regs[reg/4]=~0U;
 return 0;
}
static void delay(void *ctx,unsigned int ms) {
 struct model *m=ctx; assert(ms==50 || ms==5); m->slept+=ms;
 m->regs[0x208/4]|=0x80000000;
}
static void fresh(struct model *m) {
 memset(m,0,sizeof(*m));
 m->regs[0x130/4]=0x55112301; m->regs[0x208/4]=0x01120021; m->regs[0x120/4]=0x110001;
 m->regs[0x3e0/4]=0x1918;
}
int main(void) {
 struct model m; struct en7573_mpd s;
 struct en7573_io io={.ctx=&m,.read=rd,.write=wr,.delay_ms=delay};
 fresh(&m); assert(!en7573_measure_mpd(&io,true,&s));
 unsigned int calls=m.calls;
 assert(s.active && s.restored && !s.restore_error && m.slept==55 && m.writes==6);
 assert(s.valid[0]==511 && s.valid[1]==511 && s.valid[2]==511 && s.selected_valid==7);
 assert(s.values[0][0]==0x22334455 && s.values[1][0]==0x12345678 && s.values[2][0]==0x22334455);
 assert(s.values[1][1]==0x3412 && s.values[1][2]==0x1234);
 assert((s.selected[0]&0x3f00)==0x2400 && (s.selected[1]&0x70)==0x40 && (s.selected[2]&(1U<<26)));
 assert(m.regs[0x130/4]==0x55112301 && m.regs[0x208/4]==0x81120021 && m.regs[0x120/4]==0x110001);
 assert(m.regs[0x3e0/4]==0x1918);
 fresh(&m); assert(!en7573_measure_mpd(&io,false,&s) && !m.writes && !m.slept);
 assert(!s.active && s.restored && s.valid[0]==511 && !s.valid[1] && !s.valid[2]);
 fresh(&m); m.regs[0x120/4]|=1U<<26;
 assert(en7573_measure_mpd(&io,true,&s)==-EBUSY && !m.writes && s.restored);
 for(unsigned int fail=1;fail<=calls;fail++) {
  fresh(&m); m.fail=fail;
  assert(en7573_measure_mpd(&io,true,&s)==-EREMOTEIO);
  assert(m.regs[0x3e0/4]==0x1918);
  if(s.restored) {
   assert((m.regs[0x130/4]&0x3f00)==0x2300);
   assert((m.regs[0x208/4]&0x70)==0x20);
   assert(!(m.regs[0x120/4]&(1U<<26)));
  } else assert(s.restore_error==-EREMOTEIO);
 }
 fresh(&m); m.corrupt=true;
 assert(en7573_measure_mpd(&io,true,&s)==-EIO && !s.restored && s.restore_error);
 puts("MPD: OEM sequence, masks, endian, no calibration/current writes, every transfer failure passed");
 return 0;
}
''',flags=['-I'+str(src),str(src/'en7573.c')])

    def test_default_matrix_and_measurement_results(self):
        args=Namespace(suite='measurement',identity=None,rx_only=False,physical_only=False,skip_physical=False,cases=None)
        cases=C.discovery_plan(args)
        self.assertEqual([c['name'] for c in cases],[f'isolated-{i}' for i in range(19,32)])
        self.assertTrue(all(c['mode']=='isolated' for c in cases))
        row=dict(mpd_version=1,id=21,probes=[dict(phase=p,begin_ns=100+p*100,end_ns=155+p*100,
             error=0,restore_error=0,restored=True,active=True,selected_valid=7,
             valid=[511]*3,sample_error=[0]*3,values=[[10]*9 for _ in range(3)]) for p in range(3)])
        for p in row['probes']:
            for values in p['values']: values[7] = 0x1918 if p['phase']==1 else 0x1b18
        self.assertEqual(C.monitor_result([row],21)[1],'measured-flat')
        row['probes'][1]['values'][1][0]=20
        self.assertEqual(C.monitor_result([row],21)[1],'measured-response')
        row['probes'][1]['valid'][1]&=~1
        self.assertEqual(C.monitor_result([row],21)[1],'measurement-unavailable')
        for key,value in [('restored',False),('restore_error',-5),('active',False),('selected_valid',0)]:
            bad=copy.deepcopy(row);bad['probes'][1][key]=value
            self.assertEqual(C.monitor_result([bad],21)[1],'containment-failure')
        bad=copy.deepcopy(row);bad['probes'][1]['values'][1][7]=0x1b18
        self.assertEqual(C.monitor_result([bad],21)[1],'containment-failure')
        self.assertEqual(C.monitor_result([],21)[1],'containment-failure')
        self.assertEqual(C.monitor_result([row,row],21)[1],'containment-failure')
        args.identity=Path('/private')
        with self.assertRaises(ValueError): C.discovery_plan(args)

if __name__=='__main__': unittest.main()
