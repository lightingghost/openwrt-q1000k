"""Registration bench: independent wire layout, retained evidence and case matrix."""
import argparse
import re
import unittest
from test_pon_discovery import C, B
from test_pon_lifecycle import function as extract_function
from pon_test_utils import run_c

def function(path, name):
    return extract_function(str(B/'src/xpon-en757x/xpon_10g/src'/path), name)


class RegistrationTests(unittest.TestCase):
    def test_matrix_separates_variables_and_requires_no_fiber_actions(self):
        args=argparse.Namespace(suite='registration', physical_only=False, skip_physical=False,
                                rx_only=False, identity='explicit-private-file', cases=None)
        cases=C.discovery_plan(args)
        self.assertEqual(cases[0]['name'],'rx-startup')
        self.assertEqual([(c['ranging_mode'],c['key_inline']) for c in cases[1:]],
                         [(0,False),(1,False),(0,True),(1,True),(2,True),(3,True),(1,True)])
        self.assertTrue(all(c['samples']==300 and set('H'+str(i) for i in range(1,6)) <= set(c['ids']) for c in cases[1:]))
        self.assertFalse(any('reconnect' in c['name'] for c in cases))
        args.identity=None
        self.assertEqual([c['name'] for c in C.discovery_plan(args)], ['rx-startup'])

    def test_latency_deduplication_reset_boundaries_and_missing_tail(self):
        records=[]
        def emit(event, ident, ns, a=0,b=0,c=0,d=0,result=0):
            row=dict(critical_version=1,position=len(records)+1,seq=len(records)+1,
                     ns=ns,generation=1,event=event,id=ident,a=a,b=b,c=c,d=d,result=result)
            records.append(row)
        emit(27,1,1000000,c=10)  # Ranging_Time sequence 10.
        emit(27,15,4000000,a=9,b=10)
        emit(27,10,5000000,a=11)
        emit(27,15,6000000,a=5,b=11)
        emit(27,1,7000000,c=10)
        emit(16,0,8000000)  # Reused sequence after reset must not pair.
        emit(27,15,10000000,a=9,b=10)
        emit(26,255,11000000,a=8,b=3,c=4,d=2)
        emit(26,0,11000001,a=0x5104,b=5,c=8,d=3)
        emit(26,1,11000002,a=0x5300,b=64,c=8,d=3)
        duplicate=dict(records[3], trace_version=1); duplicate.pop('critical_version')
        result=C.registration_summary(records+[duplicate])
        self.assertEqual(result['ranging_to_ack_ms'],[3])
        self.assertEqual(result['key_request_to_enqueue_ms'],[1])
        self.assertEqual(result['key_reports_enqueued'],1)
        self.assertEqual(result['complete_snapshots'],1)
        self.assertTrue(result['critical_evidence_complete'])
        self.assertFalse(result['upstream_hardware_mic_verified'])
        self.assertFalse(C.registration_summary(records+[dict(critical_header=1,newest=12)])['critical_evidence_complete'])
        self.assertFalse(C.registration_summary(records[1:])['critical_evidence_complete'])
        base=dict(stages={'cleanup':'passed','outcome':'functional-negative'},returncode=2,
                  diagnostics_pairs_valid=True,trace={'events':20,'internal_sequence_gaps':1},registration=result)
        self.assertEqual(C.case_outcome(base),'inconclusive')
        self.assertEqual(C.case_outcome(base,critical_only=True),'functional-negative')

    def test_upstream_format_matches_vendor_and_independent_bytes(self):
        header=(B/'src/xpon-en757x/xpon_10g/inc/gpon/gpon_ploam_raw.h').read_text(encoding='latin1')
        header=header[header.index('/* Key_Report message */'):header.index('/* Sleep_Request message */')]
        vendor=function('gpon/gpon_ploam.c','ploam_send_key_report_msg')+function('gpon/gpon_ploam.c','ploam_send_acknowledge_msg')
        code=function('q1000k_omci_backend.c','qomci_key_report')
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef unsigned char unchar;
typedef unsigned char u8;
typedef uint16_t u16;
typedef unsigned int uint;
#undef __BIG_ENDIAN
#define PLOAM_DEST_ID_HIGHER 0
#define PLOAM_DEST_ID_LOWER 1
#define PLOAM_UP_MSG_KEY_REPORT 5
#define PLOAM_UP_MSG_ACKNOWLEDGE 9
#define PLOAM_KEY_FRAGMENT_LEN 32
#define PON_MSG(...)
#define GPON_PLOAM_MSG_RAW(...)
typedef u8 PLOAM_RAW_General_T;
static struct { struct { u8 ploamIkIdx; } gponSecurity; struct { u16 onu_id; } gponCfg;
    uint usPloamCounter[10]; struct { uint txPloamMsgCnt; } ploamMsgcounter;
} state,*gpGponPriv=&state;
static u8 captured[44];
static void gponDevSendPloamMsg(PLOAM_RAW_General_T *p) { memcpy(captured,p,44); }
static int q1000k_ploam_send(const u8 *p) { memcpy(captured,p,44); return 0; }
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static uint get_xpon_data(uint r) { return 0; }
struct qomci_key_request { bool confirm; u8 sequence; };
struct q1000k_key_update { u8 report_index, report[32]; };
'''+header+vendor+code+r'''
int main(void) {
    assert(sizeof(PLOAM_RAW_Key_Report_T)==44 && sizeof(PLOAM_RAW_Acknowledgment_T)==44);
    for(unsigned int onu=0;onu<=1020;onu+=17) for(int confirm=0;confirm<2;confirm++) for(int bank=1;bank<=2;bank++) {
        struct qomci_key_request request={.confirm=confirm,.sequence=173};
        struct q1000k_key_update update={.report_index=bank};
        for(int i=0;i<32;i++) update.report[i]=i*3+1;
        u8 expected[44]={ [4]=onu>>8,[5]=onu,[6]=5,[7]=173,[8]=confirm,[9]=bank };
        memcpy(expected+12,update.report,32);
        assert(!qomci_key_report(onu,&request,&update) && !memcmp(captured,expected,44));
        state.gponCfg.onu_id=onu; state.gponSecurity.ploamIkIdx=0;
        ploam_send_key_report_msg(173,confirm,bank,0,update.report,32);
        assert(!memcmp(captured,expected,44));
        for(int pik=0;pik<2;pik++) {
            memset(expected,0,44); expected[3]=pik; expected[4]=onu>>8; expected[5]=onu;
            expected[6]=9; expected[7]=173; state.gponSecurity.ploamIkIdx=pik;
            ploam_send_acknowledge_msg(173,0);
            assert(!memcmp(captured,expected,44));
        }
    }
    return 0;
}
''')

    def test_boundary_snapshot_reads_no_fifo_or_key_material(self):
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef unsigned int u32;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define QT_ACTIVATION 26
static int reads,events,fail_at;
static bool owned;
static bool q1000k_protocol_owned(void) { return owned; }
static u32 q1000k_mac_generation(void) { return 7; }
static u32 get_xpon_data(u32 reg) {
    assert(reg>=0x5000 && reg<0x6000 && !(reg&3));
    assert(reg!=0x5304 && reg!=0x5310 && reg!=0x5958 && reg!=0x595c);
    assert(!(reg>=0x5360 && reg<=0x53c4) && !(reg>=0x5210 && reg<=0x522c));
    reads++; return 0;
}
static int an7581_xpon_status(void) { return reads==fail_at ? -EIO : 0; }
static void q1000k_trace(unsigned int event,unsigned int id,int result,u32 a,u32 b,u32 c,u32 d) {
    assert(event==26); events++;
    if(id==255) assert(a==8 && b==11 && c==7 && d==26);
    else assert(c==8 && d==11);
}
'''+function('q1000k_rx_bench.c','q1000k_activation_snapshot')+r'''
int main(void) {
    q1000k_activation_snapshot(8,11); assert(!reads && !events);
    owned=true; q1000k_activation_snapshot(8,11); assert(reads==26 && events==27);
    for(int f=1;f<=26;f++) { reads=events=0; fail_at=f; q1000k_activation_snapshot(8,11); assert(reads==f && events==f+1); }
    return 0;
}
''')

if __name__=='__main__': unittest.main()
