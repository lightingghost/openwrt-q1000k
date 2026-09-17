"""New bench evidence: actual ring, wire layout and runner failure boundaries."""
import argparse
import importlib.util
from pathlib import Path
import re
import unittest
from unittest import mock
import tempfile
import json
import contextlib
import io
import tarfile
from pon_test_utils import run_c
ROOT=Path(__file__).resolve().parents[2]
B=ROOT/'package/kernel/airoha-pon'
spec=importlib.util.spec_from_file_location('discovery_collector',ROOT/'scripts/q1000k/activation-collect.py')
C=importlib.util.module_from_spec(spec);spec.loader.exec_module(C)

class DiscoveryTests(unittest.TestCase):
    def test_bounded_ring_retains_first_fault_and_exact_counts(self):
        header=(B/'src/bsp/include/q1000k_trace.h').read_text()
        source=(B/'src/bsp/core/q1000k_trace.c').read_text()
        body=re.sub(r'^#include.*\n','',header+'\n'+source,flags=re.M)
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
typedef unsigned long long u64;
typedef unsigned int u32;
typedef int s32;
typedef long long s64;
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define DEFINE_SPINLOCK(n) int n
#define spin_lock_irqsave(lock,flags) do { assert(!*(lock)); *(lock)=1; flags=0; } while(0)
#define spin_unlock_irqrestore(lock,flags) do { assert(*(lock)); *(lock)=0; (void)flags; } while(0)
#define EXPORT_SYMBOL(n)
static u64 ktime_get_boottime_ns(void) { static u64 n; return ++n; }
struct proc_dir_entry { int unused; };
struct seq_file { char out[2000000]; };
static struct seq_file seq;
static struct proc_dir_entry proc;
static int seq_printf(struct seq_file *s,const char *fmt, ...) {
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(s->out+strlen(s->out),sizeof(s->out)-strlen(s->out),fmt,ap);
    va_end(ap); assert(n>0); return n;
}
static struct proc_dir_entry *proc_create_single(const char *n,int mode,void *p,int (*show)(struct seq_file *,void *))
{ assert(!strcmp(n,"q1000k-pon-events") && mode==0400 && show); return &proc; }
static void proc_remove(struct proc_dir_entry *p) { assert(p==&proc); }
'''+body+r'''
int main(void) {
 assert(!q1000k_trace_init());
 q1000k_trace_generation(1);
 q1000k_trace(QT_FAULT,1,-5,123,0,0,0);
 for(int i=0;i<5000;i++) q1000k_trace(QT_PLOAM_DISPATCH,1,i%2?-95:0,0,0,0,0);
 assert(sizeof(struct qt_record)<=96);
 assert(qt_seq==5002 && qt_first[QT_FAULT].seq==2 && qt_first[QT_FAULT].a==123);
 assert(qt_count[QT_PLOAM_DISPATCH][1]==5000 && qt_errors[QT_PLOAM_DISPATCH][1]==2500);
 assert(qt_ring[(qt_seq-1)%QT_CAPACITY].seq==5002);
 assert(!qt_show(&seq,NULL));
 assert(strstr(seq.out,"\"wrapped\":906") && strstr(seq.out,"\"first\":true,\"seq\":2"));
 q1000k_trace_exit(); return 0;
}
''')

    def test_shadow_decoder_uses_actual_production_wire_layout(self):
        header=(B/'src/xpon-en757x/xpon_10g/inc/gpon/gpon_ploam_raw.h').read_text(encoding='latin1')
        header=header[header.index('/* Profile message */'):header.index('/* Assign_ONU_ID message */')]
        patch=(B/'patches/063-q1000k-discovery-event-history.patch').read_text()
        added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        expression=added[added.index('bool match ='):added.index('\n\t\tq1000k_trace(QT_PROFILE,')]
        run_c('''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef unsigned char unchar;
typedef unsigned char u8;
typedef unsigned int uint;
#define PLOAM_DELIMITER_PATTERN_LENS 8
#define PLOAM_PREAMBLE_PATTERN_LENS 8
#define GPON_TAG_LENS 8
#define PLOAM_MSG_MIC_LEN 8
#undef __BIG_ENDIAN
'''+header+'''
int main(void) {
    PLOAM_RAW_Profile_T p, *pRecvProfMsg=&p;
    assert(sizeof(p)==52);
    for(unsigned int a=0;a<256;a++) for(unsigned int b=0;b<256;b++) {
        memset(&p,0,sizeof(p)); u8 *w=(u8 *)&p;
        w[4]=a; w[5]=b; w[6]=b; w[15]=a; w[16]=b;
'''+expression+'''
        assert(match);
    }
    return 0;
}
''')

    def test_negative_continues_but_containment_never_does(self):
        base=dict(stages={'cleanup':'passed','outcome':'functional-negative'},returncode=2,
                  diagnostics_pairs_valid=True,trace={'events':20})
        self.assertEqual(C.case_outcome(base),'functional-negative')
        self.assertEqual(C.case_outcome(dict(base,trace={'events':20,'internal_sequence_gaps':1})),'inconclusive')
        self.assertEqual(C.case_outcome(dict(base,stages={'cleanup':'failed'})),'containment-failure')
        self.assertEqual(C.case_outcome(dict(base,stages={'cleanup':'passed','failure':'containment'})),'containment-failure')
        self.assertEqual(C.case_outcome(dict(base,physical_control={'confirmed':False})),'inconclusive')
        self.assertEqual(C.case_outcome(dict(base,timing_errors=1)),'inconclusive')
        self.assertEqual(C.summarize("sleep: invalid number '0.2'\n")['timing_errors'],1)

    def test_runner_continues_after_o5_timeout_and_stops_on_containment(self):
        for failed, expected in [('functional-negative',2),('containment-failure',1)]:
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp); inputs=root/'inputs'; inputs.write_bytes(b'fixture')
                serial=root/'serial'; serial.write_text('')
                args=argparse.Namespace(inputs=inputs,output=root/'capture',identity=None,rx_only=True,
                    physical_only=False,skip_physical=True,soak=180,serial_log=serial,iperf_server=None,resume=None)
                cases=[dict(name='activation',mode='activate',samples=240),dict(name='rx-startup',mode='rx',samples=30)]
                results=[dict(name='activation',status=failed,stages={'activation':'timeout'}),
                         dict(name='rx-startup',status='observed',stages={})]
                with mock.patch.object(C.tempfile,'gettempdir',return_value=str(root)), \
                     mock.patch.object(C,'discovery_plan',return_value=cases), mock.patch.object(C,'private_inputs',return_value=b'fixture'), \
                     mock.patch.object(C,'guards',return_value='fixture'), mock.patch.object(C,'ssh',return_value='fixture-boot'), \
                     mock.patch.object(C,'capture',side_effect=results) as capture, contextlib.redirect_stdout(io.StringIO()):
                    C.execute(args,dict(runtime='fixture',revision='a'*40))
                self.assertEqual(capture.call_count,expected)
                data=json.loads((root/'capture/collection.json').read_text())
                self.assertTrue(data['cleanup_verified'])
                self.assertEqual(data['status'],'collection-complete' if expected==2 else 'stopped')

    def test_full_plan_and_independent_recipe_selection(self):
        args=argparse.Namespace(rx_only=False,physical_only=False,identity=Path('private.json'),
                                skip_physical=False,soak=180,cases=None,recovery_action=None)
        cases=C.discovery_plan(args)
        names=[c['name'] for c in cases]
        self.assertLess(names.index('activation'),names.index('rx-reconnect'))
        self.assertTrue(next(c for c in cases if c['name']=='rx-confirm')['requires_winner'])
        self.assertTrue(next(c for c in cases if c['name']=='activation-long-sn')['requires_sn_reset'])
        args.cases='rx-confirm,rx-dark-start';args.recovery_action='5'
        cases=C.discovery_plan(args)
        self.assertEqual(len(cases),2)
        self.assertNotIn('requires_winner',cases[0])
        self.assertTrue(all(c['recovery_actions']=='5' for c in cases))
        args.cases='arbitrary-register-write'
        with self.assertRaises(ValueError): C.discovery_plan(args)

    def test_conflicting_physical_selection_is_rejected(self):
        args=argparse.Namespace(physical_only=True,skip_physical=True)
        with self.assertRaisesRegex(ValueError,'cannot be combined'):
            C.discovery_plan(args)

    def test_retained_first_reset_survives_ring_loss_without_hiding_gaps(self):
        first=dict(trace_version=1,first=True,seq=2,ns=100,generation=2,
                   event=16,id=1,result=0,a=2,b=21,c=20,d=1)
        latest=dict(first,first=False,seq=5000,ns=200,event=5,id=0)
        counter=dict(trace_count=1,event=16,id=1,count=1,errors=0)
        result=C.trace_summary([latest,first,counter])
        self.assertTrue(result['sn_threshold_reset'])
        self.assertEqual(result['first_reset'],first)
        self.assertEqual(result['internal_sequence_gaps'],4999)
        self.assertEqual(result['events'],1)
        self.assertTrue(C.trace_summary([counter])['sn_threshold_reset'])

    def test_explicit_long_sn_selection_can_follow_separate_capture(self):
        args=argparse.Namespace(rx_only=False,physical_only=False,identity=Path('private.json'),
                               skip_physical=True,soak=180,cases='activation-long-sn',recovery_action=None)
        cases=C.discovery_plan(args)
        self.assertEqual(len(cases),1)
        self.assertEqual(cases[0]['sn_comparison_selection'],'explicit')
        self.assertNotIn('requires_sn_reset',cases[0])

    def test_portable_timing_helper_is_pinned_and_staged_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); helper=root/'sleep'; target=root/'collector.py'
            elf=b'\x7fELF\x02\x01'+b'\0'*12+b'\xb7\x00'+b'fixture'
            helper.write_bytes(elf)
            pin=dict(revision='a'*40,image_sha256='b'*64)
            with mock.patch.object(C,'artifact_pin',return_value=pin), contextlib.redirect_stdout(io.StringIO()):
                C.build_single_file(root,target,helper)
            spec=importlib.util.spec_from_file_location('timing_collector',target)
            module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
            self.assertEqual(module.TIMING_SLEEP,elf)
            self.assertEqual(module.PIN['timing_sleep_sha256'],C.digest(elf))
            files={name:name.encode() for name in ('cal','pm','dm')}
            archive=root/'inputs.tar'
            with tarfile.open(archive,'w') as out:
                for name,data in dict(files,sha256sums=b'fixture').items():
                    member=tarfile.TarInfo(name);member.size=len(data)
                    out.addfile(member,io.BytesIO(data))
            with mock.patch.object(module,'INPUTS',{n:(len(v),C.digest(v)) for n,v in files.items()}):
                payload=module.private_inputs(archive,{})
            with tarfile.open(fileobj=io.BytesIO(payload)) as staged:
                self.assertEqual(staged.getmember('sleep').mode,0o700)
                self.assertEqual(staged.extractfile('sleep').read(),elf)
                self.assertIn((C.digest(elf)+'  sleep').encode(),staged.extractfile('sha256sums').read())

    def test_report_distinguishes_planned_from_executed_tests(self):
        result=dict(stages={'activation':'timeout'},status='functional-negative',
                    trace={'milestones':{}},security={'data_rx_key_valid':{'samples':0}})
        case=dict(ids=['A01','A04','D01','D02','D03','D04','D05','D06'])
        outcome=C.test_outcomes(case,result,'')
        self.assertEqual(outcome['A01'],'timeout')
        self.assertEqual(outcome['A04'],'no-verified-profile-observed')
        self.assertTrue(all(outcome[k]=='not-run' for k in ('D02','D03','D04','D05','D06')))
        case=dict(ids=['R00','R01','R02','R03','R04','R08'])
        result=dict(stages={'passive':'no-reacquisition','recovery':'recovered-after-sequence'},
                    independent_recovery=False)
        outcome=C.test_outcomes(case,result,'recovery_action=1 phase=observed stable=0\n'
                                          'recovery_action=2 phase=observed stable=5\n')
        self.assertEqual(outcome['R00'],'no-reacquisition')
        self.assertEqual(outcome['R01'],'no-reacquisition')
        self.assertEqual(outcome['R02'],'frames-restored-after-sequence')
        self.assertEqual(outcome['R03'],'not-run')
        self.assertEqual(outcome['R04'],'not-run')
        self.assertEqual(outcome['R08'],'recovered-after-sequence')
        result['independent_recovery']=True
        self.assertEqual(C.test_outcomes(case,result,'')['R08'],'independent-recovery-observed')

if __name__=='__main__': unittest.main()
