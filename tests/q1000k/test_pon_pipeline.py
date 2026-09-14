#!/usr/bin/env python3
"""Exercise ordered physical shutdown and every stage/containment failure."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
REPO=Path(__file__).resolve().parents[2]
MAC=REPO/'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
class PipelineTests(unittest.TestCase):
    def test_shutdown_orders_physical_stages_and_preserves_failure(self):
        source=(MAC/'inc/common/q1000k_pipeline.h').read_text()+(MAC/'src/q1000k_pipeline.c').read_text()
        source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
        fixture=Path(__file__).with_name('pon_pipeline_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',source))
if __name__=='__main__': unittest.main()
