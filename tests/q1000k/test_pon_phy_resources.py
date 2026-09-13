#!/usr/bin/env python3
"""Compile the production optical provider against locked MMIO fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
BSP = REPO / 'package/kernel/airoha-pon/src/bsp'

class PhyResourceTests(unittest.TestCase):
    def test_windows_irq_and_atomic_bit_updates(self):
        source = (BSP / 'core/an7581_pon_phy.c').read_text()
        body = source[source.index('struct an7581_pon_phy {'):source.index('void (*ledTurnOff_hook)')]
        fixture = Path(__file__).with_name('pon_phy_resources_fixture.c').read_text()
        with tempfile.TemporaryDirectory(prefix='q1000k-phy-resources-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(fixture.replace('/* PRODUCTION */', body))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-pthread', '-I', str(BSP / 'include'), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)

if __name__ == '__main__':
    unittest.main()
