#!/usr/bin/env python3
"""Verified MAC key word ordering, access ownership and failure atomicity."""
from pathlib import Path
import re
import unittest
from test_pon_auth import MAC, production
from pon_test_utils import run_c
class MacKeyTests(unittest.TestCase):
    def test_banks_registers_and_failures(self):
        fixture=Path(__file__).with_name('pon_auth_fixture.c').read_text()
        fixture=fixture[:fixture.index('/* PRODUCTION */')]
        extra=Path(__file__).with_name('pon_mac_keys_fixture.c').read_text()
        code=(MAC/'inc/common/q1000k_mac_keys.h').read_text()+(MAC/'src/q1000k_mac_keys.c').read_text()
        code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
        run_c(fixture+extra.replace('/* PRODUCTION */',production()+code),
              flags=['-Wl,--no-as-needed','-lcrypto'])
