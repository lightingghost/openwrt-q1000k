#!/usr/bin/env python3
"""Exercise production PON port-wide frame limits and weight units."""
from pathlib import Path
import os
import unittest
from pon_test_utils import run_c
REPO=Path(__file__).resolve().parents[2]
class PortTests(unittest.TestCase):
    def test_port_changes_require_drain_and_compare_current_config(self):
        eth=Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))
        source=(eth/'airoha_pon.c').read_text()
        start=source.index('static bool airoha_pon_port_equal(')
        end=source.index('EXPORT_SYMBOL_GPL(airoha_pon_configure_port);',start)
        fixture=Path(__file__).with_name('pon_port_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',source[start:end]))
if __name__=='__main__': unittest.main()
