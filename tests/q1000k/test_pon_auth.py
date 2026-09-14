#!/usr/bin/env python3
"""Official key/MIC vectors, fragmented framing and crypto failure containment."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
REPO=Path(__file__).resolve().parents[2]
MAC=REPO/'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
def production():
    source=(MAC/'inc/common/q1000k_auth.h').read_text()+(MAC/'src/q1000k_auth.c').read_text()
    return re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
class AuthTests(unittest.TestCase):
    def test_keys_mic_lengths_fragments_and_failures(self):
        fixture=Path(__file__).with_name('pon_auth_fixture.c').read_text()
        vectors=Path(__file__).with_name('pon_auth_vectors.h').read_text()
        run_c(fixture.replace('/* PRODUCTION */',production()).replace('/* VECTORS */',vectors),
              flags=['-Wl,--no-as-needed','-lcrypto'])
if __name__=='__main__': unittest.main()
