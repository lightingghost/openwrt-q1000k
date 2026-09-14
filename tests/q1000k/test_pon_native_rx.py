#!/usr/bin/env python3
"""Native RX routing, complete drop returns and per-interface statistics."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
from test_pon_lifecycle import function

class NativeReceiveTests(unittest.TestCase):
    def test_every_drop_consumes_once_and_success_accounts_before_header_pull(self):
        fixture = Path(__file__).with_name('pon_native_rx_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', function('xpondrv.c', 'xpondrv_rx_packet')))
