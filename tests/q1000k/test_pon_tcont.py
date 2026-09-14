#!/usr/bin/env python3
"""Exercise actual AN7581 T-CONT commands and integration without hardware."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_identity import BSP
from test_pon_lifecycle import function

REPO = Path(__file__).resolve().parents[2]
MAC = REPO / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'


class PonTcontTests(unittest.TestCase):
    def test_table_commands_faults_and_concurrent_allocations(self):
        source = (MAC / 'inc/common/q1000k_tcont.h').read_text() + '\n' + \
            (MAC / 'src/q1000k_tcont.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        fixture = Path(__file__).with_name('pon_tcont_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', source), flags=['-pthread'])

    def test_alloc_id_callers_propagate_physical_errors(self):
        source = function('gpon/gpon_dev.c', 'gponDevAssignNewAllocId')
        source += function('xmcs/xmcs_if.c', 'xmcs_create_tcont_info')
        source += function('xmcs/xmcs_if.c', 'xmcs_remove_tcont_info')
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#define Q1000K_PON_IDENTITY
#define Q1000K_ALLOC_ID_MAX 0x3fff
#define PLOAM_ALLOC_ID_ASSIGN 1
#define PLOAM_ALLOC_ID_DEALLOCATE 2
#define GPON_ONU_ID 17
#define XMCS_EVENT_TYPE_GPON 1
#define XMCS_EVENT_GPON_TCONT_ALLOCED 2
#define WRITE_ONCE(p,v) ((p)=(v))
typedef uint16_t u16;
typedef uint16_t ushort;
typedef unsigned int uint;
typedef struct { unsigned int allocId; unsigned char allocIdType; } AllocId_Config_t;
struct XMCS_TcontCfg_S { u16 allocId; };
static int result, creates, deletes, events;
static int gwan_create_new_tcont(u16 id) { assert(id==200); creates++; return result; }
static int gwan_remove_tcont(u16 id) { assert(id==200); deletes++; return result; }
static void xmcs_report_event(unsigned int type,unsigned int event,unsigned int id) {
    assert(type==XMCS_EVENT_TYPE_GPON && event==XMCS_EVENT_GPON_TCONT_ALLOCED && id==200); events++;
}
'''+source+r'''
int main(void) {
    AllocId_Config_t config={200,PLOAM_ALLOC_ID_ASSIGN};
    struct XMCS_TcontCfg_S request={200};
    int errors[]={-EUCLEAN,-ETIMEDOUT,-EIO,-EBUSY,-ENOMEM,-ESTALE};
    for(unsigned int i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        result=errors[i];
        config.allocIdType=PLOAM_ALLOC_ID_ASSIGN;
        assert(gponDevAssignNewAllocId((unsigned long)&config)==result);
        config.allocIdType=PLOAM_ALLOC_ID_DEALLOCATE;
        assert(gponDevAssignNewAllocId((unsigned long)&config)==result);
        assert(xmcs_create_tcont_info(&request)==result && xmcs_remove_tcont_info(request.allocId)==result);
        assert(!events);
    }
    result=0; config.allocIdType=PLOAM_ALLOC_ID_ASSIGN;
    assert(!gponDevAssignNewAllocId((unsigned long)&config) && events==1);
    assert(creates==13 && deletes==12);
    return 0;
}
''')

    def test_ploam_deallocation_does_not_ack_failed_rebuild(self):
        source=function('gpon/gpon_ploam.c','ploam_recv_assign_alloc_id')
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef unsigned int uint;
typedef uint16_t ushort;
#define GPON_PLOAM_MSG_RAW(...) ((void)0)
#define PON_MSG(...) ((void)0)
#define PLOAM_DEST_ID_HIGHER 0
#define PLOAM_DEST_ID_LOWER 1
#define GPON_10G_STATE_O5 5
#define GPON_10G_STATE_O9 9
#define GPON_CURR_STATE 5
#define GPON_ONU_ID 17
#define FALSE 0
#define PLOAM_ALLOC_ID_ASSIGN 1
#define PLOAM_ALLOC_ID_DEALLOCATE 2
#define XGPON_PLOAM_ACK_OK 0
#define XGPON_PLOAM_ACK_PROCES_ERR 1
static int ng2_o4_to_09, call_result, calls, ack;
typedef struct {
    struct { unsigned char dest_id[2], alloc_id_m, alloc_id_l, alloc_id_type, seq_no; } raw;
} PLOAM_RAW_Assign_AllocID_T;
static struct {
    struct { unsigned short onu_id; struct { unsigned int allocId; unsigned char allocIdType; } allocIdConfig; } gponCfg;
} private = { .gponCfg.onu_id=17 }, *gpGponPriv=&private;
static int gwanCheckAllocIdExist(unsigned int id) { assert(id==200); return false; }
static int gponDevAssignNewAllocId(unsigned long arg) {
    assert(arg==(unsigned long)&private.gponCfg.allocIdConfig);
    assert(private.gponCfg.allocIdConfig.allocId==200); calls++; return call_result;
}
static void ploam_send_acknowledge_msg(unsigned int seq,unsigned int result) { assert(seq==4); ack=result; }
'''+source+r'''
int main(void) {
    PLOAM_RAW_Assign_AllocID_T msg={.raw={.dest_id={0,17},.alloc_id_l=200,.seq_no=4}};
    int results[]={0,-EUCLEAN,-ETIMEDOUT,-EIO,-EBUSY,-ENOMEM};
    for(unsigned int type=1;type<=2;type++) {
        msg.raw.alloc_id_type=type;
        for(unsigned int i=0;i<sizeof(results)/sizeof(results[0]);i++) {
            call_result=results[i]; ack=77;
            assert(!ploam_recv_assign_alloc_id(&msg));
            assert(ack==(call_result ? XGPON_PLOAM_ACK_PROCES_ERR : XGPON_PLOAM_ACK_OK));
        }
    }
    assert(calls==12); return 0;
}
''')

    def test_reset_stops_when_retirement_is_incomplete(self):
        source = function('gpon/gpon.c', 'gpon_disable')
        # Keep the actual production prefix, including the gate. The remaining
        # reset/PHY body is a trap: it must not run after any retirement error.
        end = source.index('PON_MSG(')
        prefix = source[:end] + 'reset_calls++; }\n'
        source = function('gpon/gpon_ploam.c', 'ploam_recv_assign_onu_id')
        start = source.index('#ifdef Q1000K_PON_IDENTITY',
                             source.index('GPON_10G_STATE_O4) ||'))
        end = source.index('#endif', start) + len('#endif')
        prefix += 'static int reassign(void) {\n' + source[start:end] + \
            '\nreset_calls++; return 0; }\n'
        run_c(r'''
#include <assert.h>
#include <errno.h>
#define Q1000K_PON_IDENTITY
#define pr_err(...) ((void)0)
typedef int PON_PHY_Event_data_t;
typedef int GPON_RESET_TYPE_t;
static int retirement_result, gem_retirement_result, gem_calls, reset_calls;
static int gwan_remove_all_tcont(void) { return retirement_result; }
static int gwan_remove_all_gemport_for_disable(void) { gem_calls++; return gem_retirement_result; }
''' + prefix + r'''
int main(void) {
    int errors[]={-EOPNOTSUPP,-EIO,-EBUSY,-ENODEV};
    for(unsigned int i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        retirement_result=errors[i]; gem_calls=0;
        gpon_disable(0); assert(!reset_calls && gem_calls==1);
        assert(reassign()==errors[i] && !reset_calls && gem_calls==2);
    }
    retirement_result=0;
    for(unsigned int i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        gem_retirement_result=errors[i];
        gpon_disable(0); assert(!reset_calls);
        assert(reassign()==errors[i] && !reset_calls);
    }
    gem_retirement_result=0;
    gpon_disable(0); assert(!reset_calls);
    assert(reassign()==-EOPNOTSUPP && !reset_calls);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
