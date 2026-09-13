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

    def test_setup_rollback_removal_and_caller_errors(self):
        names = ['q1000k_gwan_publish_tcont', 'q1000k_gwan_retire_channel',
                 'q1000k_gwan_create_tcont', 'q1000k_gwan_remove_tcont',
                 'q1000k_gwan_remove_all_tcont', 'gwan_create_new_tcont',
                 'gwan_remove_tcont', 'gwan_remove_all_tcont']
        source = ''.join(function('pwan/gpon_wan.c', n) for n in names)
        source += function('gpon/gpon_dev.c', 'gponDevAssignNewAllocId')
        source += function('xmcs/xmcs_if.c', 'xmcs_create_tcont_info')
        source += function('xmcs/xmcs_if.c', 'xmcs_remove_tcont_info')
        fixture = Path(__file__).with_name('pon_tcont_transaction_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', source))

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
