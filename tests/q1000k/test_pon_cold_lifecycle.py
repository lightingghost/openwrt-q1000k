#!/usr/bin/env python3
"""Execute real cold PHY/state callers; failed hardware cannot publish ready."""
from pathlib import Path
import unittest
from test_pon_lifecycle import function
from pon_test_utils import run_c

class ColdLifecycleTests(unittest.TestCase):
    def test_ready_loss_state_errors_and_no_legacy_reset_bypass(self):
        code = function('gpon/gpon_act.c', 'gpon_act_change_state')
        for name in ['gpon_phy_ready_handler', 'gpon_phy_loss_handler', 'gpon_enable', 'gpon_sw_resync']:
            code += function('gpon/gpon.c', name)
        for name in ['gponDevResetCtrl', 'gponDevMacReset', 'gpon_dev_init']:
            code += function('gpon/gpon_dev.c', name)
        code += function('xpondrv.c', 'xpon_phy_event_dispatch')
        fixture = Path(__file__).with_name('pon_cold_lifecycle_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', code))
