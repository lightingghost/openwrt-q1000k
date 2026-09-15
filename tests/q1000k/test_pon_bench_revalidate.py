import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('revalidate', Path(__file__).resolve().parents[2] /
                                             'scripts/q1000k/bench-revalidate.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RevalidateTests(unittest.TestCase):
    def test_source_guard_excludes_production_and_original_test_changes(self):
        for path in ('tests/q1000k/check_pon_bench_image.py', 'scripts/q1000k/README.md',
                     'target/linux/airoha/XGSPON-BENCH.q1000k.md'):
            self.assertTrue(MODULE.allowed_change(path), path)
        for path in ('package/kernel/airoha-pon/Makefile', 'target/linux/airoha/Makefile',
                     'tests/q1000k/test_xgspon_views.cjs', 'tests/q1000k/test_pon_phy.py',
                     'scripts/q1000k/bench-run.py', '.config', 'include/kernel.mk'):
            self.assertFalse(MODULE.allowed_change(path), path)

    def test_evidence_rejects_missing_or_changed_files(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / 'host-tests.log').write_text('OK\n')
            (output / 'sha256sums').write_text(MODULE.digest(output / 'host-tests.log') + '  host-tests.log\n')
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                MODULE.verify_evidence(output)
            (output / 'host-tests.log').write_text('FAILED\n')
            with self.assertRaisesRegex(ValueError, 'changed'):
                MODULE.verify_evidence(output)


if __name__ == '__main__':
    unittest.main()
