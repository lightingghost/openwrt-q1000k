#!/usr/bin/env python3
"""Compile real class 171 operations and exercise their inverse mappings."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
ROOT = Path(__file__).resolve().parents[2]
MAC = ROOT / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
def vlan_types():
    header = (ROOT / 'package/kernel/q1000k-omci/src/include/net/xpon/omci.h').read_text()
    return '#define OMCI_EXT_VLAN_RULE_LEN 16\n#define OMCI_VLAN_FILTER_MAX_ENTRIES 12\n' + '\n'.join(
        re.search(r'struct ' + name + r' \{.*?\n\};', header, re.S).group(0)
        for name in ('omci_vlan_filter_entry', 'omci_vlan_tagging_filter', 'omci_extended_vlan_rule', 'omci_service_config'))
def vlan_source():
    source = (MAC / 'inc/common/q1000k_vlan.h').read_text() + (MAC / 'src/q1000k_vlan.c').read_text()
    return re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
class VlanTests(unittest.TestCase):
    def test_tag_count_priority_zero_inverse_and_unsupported_rules(self):
        fixture = Path(__file__).with_name('pon_vlan_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', vlan_types() + vlan_source()))
if __name__ == '__main__':
    unittest.main()
