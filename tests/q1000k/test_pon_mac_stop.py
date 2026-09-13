#!/usr/bin/env python3
"""Compile actual MAC resource-provider stop transactions with MMIO faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
BSP = REPO / 'package/kernel/airoha-pon/src/bsp'


class MacStopTests(unittest.TestCase):
    def test_stop_acknowledgments_and_pipeline_preconditions(self):
        src = (BSP / 'core/an7581_xpon.c').read_text()
        body = src[src.index('struct an7581_xpon {'):src.index('static int an7581_xpon_probe')]
        fixture = Path(__file__).with_name('pon_mac_stop_fixture.c').read_text()
        with tempfile.TemporaryDirectory(prefix='q1000k-mac-stop-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(fixture.replace('/* PRODUCTION */', body))
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(BSP / 'include'), str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
