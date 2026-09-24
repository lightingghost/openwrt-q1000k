#!/usr/bin/env python3
"""OMCI entity programming and native ANI classifier integration."""
from pathlib import Path
import re
import os
import unittest
from pon_test_utils import run_c
from test_pon_vlan import vlan_types, vlan_source
from test_pon_gem import MAC, binding_source
ROOT=Path(__file__).resolve().parents[2]
class ServiceTests(unittest.TestCase):
    def test_entities_vlan_queues_and_failure_containment(self):
        base=Path(__file__).with_name('pon_gem_binding_fixture.c').read_text()
        base=base[:base.index('int main(void)')].replace('/* PRODUCTION */',binding_source())
        base=base.replace('assert(physical_phase==1); int ret=physical_step(); if(!ret) *qos=qos_model[ch];',
                          'assert(physical_phase==1 || physical_phase==2); int ret=physical_step(); if(!ret) *qos=qos_model[ch];')
        base=base.replace('assert(physical_phase==2 && !memcmp(qos,&qos_model[ch],sizeof(*qos))); return physical_step();',
                          'assert(physical_phase==2); int ret=physical_step(); if(!ret) qos_model[ch]=*qos; return ret;')
        header=(ROOT/'package/kernel/q1000k-omci/src/include/net/xpon/omci.h').read_text()
        types='typedef uint64_t u64;\nstruct omci_device;\n'+vlan_types()+vlan_source()
        eth=Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(ROOT.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))
        api=(eth.parents[3]/'include/linux/soc/airoha/airoha_pon.h').read_text()
        types+=re.search(r'struct airoha_pon_flow \{.*?\n\};',api,re.S).group(0)+'\n'
        for name in ['omci_ani_topology','omci_priority_queue_config','omci_traffic_scheduler_config']:
            types+=re.search(r'struct '+name+r' \{.*?\n\};',header,re.S).group(0)+'\n'
        code=(MAC/'src/q1000k_services.c').read_text()
        code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
        diag=(MAC/'inc/common/q1000k_dhcp6_diag.h').read_text()
        code=diag+'''
static bool q1000k_dhcp6_sample(const struct sk_buff *skb, struct q6d_sample *sample) { return false; }
static void q1000k_dhcp6_record(enum q6d_stage stage, const struct q6d_sample *sample, int result) {}
'''+code
        fixture=Path(__file__).with_name('pon_services_fixture.c').read_text()
        run_c(base+types+fixture.replace('/* SERVICES */',code),flags=['-pthread'])
