#!/usr/bin/env python3
"""Compile the production controller lease API with lifetime/fault injection."""
from pathlib import Path
import unittest
from pon_test_utils import run_c

ROOT=Path(__file__).resolve().parents[2]
class ControllerTests(unittest.TestCase):
    def test_portable_loader_and_receiver_snapshots(self):
        directory = ROOT / 'package/kernel/q1000k-pon-control/src'
        fixture = Path(__file__).with_name('test_en7573.c').read_text()
        run_c(fixture, flags=['-O2', '-I' + str(directory), str(directory / 'en7573.c')])

    def test_consumer_lifetime_and_failures(self):
        source=(ROOT/'package/kernel/q1000k-pon-control/src/driver.c').read_text()
        body=source[source.index('/* Kernel consumer lifecycle:'):source.index('/* End kernel consumer lifecycle. */')]
        fixture=Path(__file__).with_name('pon_controller_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',body), flags=['-Wno-misleading-indentation'])
if __name__=='__main__': unittest.main()
