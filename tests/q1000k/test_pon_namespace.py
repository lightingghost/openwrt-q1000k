#!/usr/bin/env python3
"""Exercise ordered physical shutdown and every stage/containment failure."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
REPO=Path(__file__).resolve().parents[2]
MAC=REPO/'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
class NamespaceTests(unittest.TestCase):
    def test_clear_install_activate_and_every_boundary_failure(self):
        source=(MAC/'inc/common/q1000k_pipeline.h').read_text()+(MAC/'src/q1000k_pipeline.c').read_text()
        start=source.index('static int q1000k_pipeline_clear_fcs(')
        end=source.index('int q1000k_pipeline_reconfigure(',start)
        source=source[:start]+source[end:]
        source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
        fixture=Path(__file__).with_name('pon_namespace_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',source))
    def test_append_owner_context_channels_and_failure_containment(self):
        from test_pon_identity import extract
        header=(MAC/'inc/common/q1000k_pipeline.h').read_text()
        header=re.sub(r'^#include[^\n]*\n','',header,flags=re.M)
        text=(MAC/'src/q1000k_pipeline.c').read_text()
        production=extract(text,'q1000k_pipeline_table_context')+extract(text,'q1000k_pipeline_append')
        fixture=Path(__file__).with_name('pon_pipeline_append_fixture.c').read_text()
        run_c(fixture.replace('/* HEADER */',header).replace('/* PRODUCTION */',production))
if __name__=='__main__': unittest.main()
