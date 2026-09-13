#!/usr/bin/env python3
"""Run OMCI TX framing and the real vendor MIC caller with injected failures."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_identity import BSP, REPO, extract


def production():
    mac = BSP.parent / 'xpon-en757x/xpon_10g'
    source = (mac / 'src/pwan/gpon_wan.c').read_text()
    types = source[source.index('typedef struct Omci_Hander{'):]
    types = types[:types.index('}__attribute__((packed)) Omci_Header_T, *pOmci_Header;') +
                  len('}__attribute__((packed)) Omci_Header_T, *pOmci_Header;')]
    helper = (REPO / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/src/q1000k_omci.c').read_text()
    helper = re.sub(r'^#include[^\n]*\n', '', helper, flags=re.M)
    # Make the static function visible to the extraction helper, without
    # replacing its implementation or its compile-time Q1000K branch.
    source = source.replace('static int remove_omci_crc_if_exist(',
                            'int remove_omci_crc_if_exist(')
    return types + '\n' + helper + extract(source, 'remove_omci_crc_if_exist') + \
        extract(source, 'gwan_add_us_omci_mic')


class PonOmciTxTests(unittest.TestCase):
    def test_framing_and_mic_error_ownership(self):
        fixture = Path(__file__).with_name('pon_omci_tx_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', production()))


if __name__ == '__main__':
    unittest.main()
