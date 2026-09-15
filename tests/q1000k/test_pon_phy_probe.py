#!/usr/bin/env python3
"""Check every bounded probe, exact restoration and all operation failures."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
from test_pon_phy_lifecycle import PHY, BSP, production_source
from pon_phy_probe_steps import generate

class PhyProbeTests(unittest.TestCase):
    def test_reference_steps_have_not_drifted(self):
        self.assertEqual((PHY/'src/q1000k_phy_probe_steps.h').read_text(), generate())

    def test_all_modes_and_failures(self):
        steps = (PHY/'src/q1000k_phy_probe_steps.h').read_text()
        source, regs = production_source(PHY/'src/q1000k_phy_probe.c', extra=steps)
        marker = 'struct qprobe_step { u32 reg, end, start, value, delay_us; };'
        source = source.replace(marker, marker+'\n'+steps)
        fixture = Path(__file__).with_name('pon_phy_probe_fixture.c').read_text()
        run_c(fixture.replace('/* REGISTERS */', regs).replace('/* PRODUCTION */',source),
              flags=['-I',str(BSP/'include')])
