#!/usr/bin/env python3
"""OMCI entity programming and native ANI classifier integration."""
from pathlib import Path
import re
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
        types='struct omci_device;\n'+vlan_types()+vlan_source()
        for name in ['omci_ani_topology','omci_priority_queue_config','omci_traffic_scheduler_config']:
            types+=re.search(r'struct '+name+r' \{.*?\n\};',header,re.S).group(0)+'\n'
        code=(MAC/'src/q1000k_services.c').read_text()
        code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
        fixture=Path(__file__).with_name('pon_services_fixture.c').read_text()
        run_c(base+types+fixture.replace('/* SERVICES */',code),flags=['-pthread'])
