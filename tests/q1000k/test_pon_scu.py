#!/usr/bin/env python3
"""Check the production AN7581 SCU adapter's borrowed-regmap lifecycle."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from pon_test_utils import run_c

ROOT = Path(__file__).resolve().parents[2]


class ScuTests(unittest.TestCase):
    def test_init_errors_and_unload_release_only_cached_aliases(self):
        vendor = ROOT / 'build_dir/target-aarch64_cortex-a53_musl/linux-airoha_an7581/airoha-pon-v2/airoha-pon'
        relative = Path('bsp/core/ecnt_scu.c')
        source = (vendor / relative).read_text(errors='replace')
        if 'module_exit(an7581_scu_exit)' not in source:
            with tempfile.TemporaryDirectory(prefix='q1000k-scu-test.') as tmp:
                tmp = Path(tmp)
                (tmp / relative).parent.mkdir(parents=True)
                (tmp / relative).write_text(source)
                subprocess.run(['patch', '--batch', '-p1', '-i', str(ROOT / 'package/kernel/airoha-pon/patches/051-an7581-scu-module-exit.patch')],
                               cwd=tmp, check=True, capture_output=True)
                source = (tmp / relative).read_text()
        start = source.index('#ifdef AN7581_OPENWRT_SCU\nstatic int ECNT_SCU_DRV_PROBE(void)')
        lifecycle = source[start:].split('\n#else', 1)[0].split('\n', 1)[1]
        self.assertIn('module_exit(an7581_scu_exit);', lifecycle)
        fixture = Path(__file__).with_name('pon_scu_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION LIFECYCLE */', lifecycle))


if __name__ == '__main__':
    unittest.main()
