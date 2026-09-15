#!/usr/bin/env python3
"""RX-only MAC guard and status tests, without hardware access."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
ROOT = Path(__file__).resolve().parents[2]

class ReceiveBenchTests(unittest.TestCase):
    def test_board_guard_and_fresh_status_errors(self):
        base = ROOT / 'package/kernel/airoha-pon/src'
        code = (base / 'bsp/include/q1000k_phy_api.h').read_text()
        code += (base / 'xpon-en757x/xpon_10g/src/q1000k_rx_bench.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.M)
        fixture = Path(__file__).with_name('pon_rx_bench_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', code))
