#!/usr/bin/env python3
"""Hardware-calibrated IFC encoding, admission, revocation and faults."""
from pathlib import Path
import re
import unittest
from test_pon_ppe import ETH
from pon_test_utils import run_c

class PonIfcTests(unittest.TestCase):
    def test_profile_programming_and_fault_containment(self):
        api=(ETH.parents[3]/'include/linux/soc/airoha/airoha_pon.h').read_text()
        header=(ETH/'airoha_eth.h').read_text()
        types=re.search(r'struct airoha_pon_flow \{.*?\n\};',api,re.S).group()
        types+='\n'+re.search(r'struct airoha_pon_ingress \{.*?\n\};',header,re.S).group()
        fixture=Path(__file__).with_name('pon_ifc_fixture.c').read_text()
        run_c(fixture.replace('/* TYPES */',types).replace('/* PRODUCTION */',(ETH/'airoha_pon_ifc.h').read_text()))

if __name__=='__main__':
    unittest.main()
