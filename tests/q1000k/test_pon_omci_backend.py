#!/usr/bin/env python3
"""Actual OMCI provider/session code with modeled core and physical boundaries."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_gem import MAC
ROOT=Path(__file__).resolve().parents[2]
class BackendTests(unittest.TestCase):
    def test_session_order_rekey_reset_packet_ownership_and_faults(self):
        omci=(ROOT/'package/kernel/q1000k-omci/src/include/net/xpon/omci.h').read_text()
        types=''
        for name in ['omci_identity','omci_device_ops']:
            types+=re.search(r'struct '+name+r' \{.*?\n\};',omci,re.S).group(0)+'\n'
        code=(ROOT/'package/kernel/airoha-pon/src/bsp/include/q1000k_phy_api.h').read_text()+(MAC/'inc/common/q1000k_auth.h').read_text()+(MAC/'inc/common/q1000k_mac_keys.h').read_text()
        code+=(MAC/'inc/common/q1000k_mac_cold.h').read_text()+(MAC/'src/q1000k_omci_backend.c').read_text()
        code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
        fixture=Path(__file__).with_name('pon_omci_backend_fixture.c').read_text()
        run_c(fixture.replace('/* TYPES */',types).replace('/* PRODUCTION */',code))
