#!/usr/bin/env python3
"""Exercise exclusive MAC reset, access exclusion and containment errors."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
REPO=Path(__file__).resolve().parents[2]
class MacResetTests(unittest.TestCase):
    def test_reset_does_not_hold_irq_lock_or_replay_shared_word(self):
        src=(REPO/'package/kernel/airoha-pon/src/bsp/core/an7581_xpon.c').read_text()
        source=src[src.index('int an7581_xpon_reset(void)'):src.index('static int an7581_xpon_probe')]
        fixture=Path(__file__).with_name('pon_mac_reset_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',source))
if __name__=='__main__': unittest.main()
