#!/usr/bin/env python3
"""Unicast AES banks: drain ownership, readback, validity order and switch ack."""
from pathlib import Path
import re
import unittest
from test_pon_auth import MAC, production
from pon_test_utils import run_c
class DataKeyTests(unittest.TestCase):
    def test_verified_install_and_activation(self):
        base=Path(__file__).with_name('pon_auth_fixture.c').read_text().split('/* PRODUCTION */')[0]
        fixture=Path(__file__).with_name('pon_data_keys_fixture.c').read_text()
        source=(MAC/'inc/common/q1000k_mac_keys.h').read_text()+(MAC/'src/q1000k_mac_keys.c').read_text()
        source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
        run_c(base+fixture.replace('/* PRODUCTION */',production()+source), flags=['-Wl,--no-as-needed','-lcrypto'])
if __name__=='__main__': unittest.main()
