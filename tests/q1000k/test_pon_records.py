#!/usr/bin/env python3
"""Fault injection for physical replacement through real GEM/T-CONT callers."""
from pathlib import Path
import unittest
from test_pon_gem import binding_source
from pon_test_utils import run_c
class RecordTests(unittest.TestCase):
    def test_reuse_cas_identity_and_each_physical_failure(self):
        fixture=Path(__file__).with_name('pon_gem_binding_fixture.c').read_text()
        fixture=fixture[:fixture.index('int main(void)')]
        body=Path(__file__).with_name('pon_records_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',binding_source())+body,flags=['-pthread'])

    def test_live_append_preserves_omcc_and_rejects_replacement_or_partial_failure(self):
        fixture=Path(__file__).with_name('pon_gem_binding_fixture.c').read_text()
        fixture=fixture[:fixture.index('int main(void)')]
        body=Path(__file__).with_name('pon_append_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',binding_source())+body,flags=['-pthread'])
