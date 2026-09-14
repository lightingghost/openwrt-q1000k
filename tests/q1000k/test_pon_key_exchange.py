#!/usr/bin/env python3
"""All seven normative ONU key contexts and checked PLOAM FIFO writes."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_gem import MAC


def key_source():
    code = ''.join((MAC / 'inc/common' / n).read_text() for n in
                   ('q1000k_auth.h', 'q1000k_mac_keys.h', 'q1000k_key_exchange.h'))
    code += (MAC / 'src/q1000k_key_exchange.c').read_text()
    return re.sub(r'^#include[^\n]*\n', '', code, flags=re.M)


class KeyExchangeTests(unittest.TestCase):
    def test_context_matrix_reports_and_fifo_failures(self):
        fixture = Path(__file__).with_name('pon_key_exchange_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', key_source()))
