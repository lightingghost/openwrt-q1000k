#!/usr/bin/env python3
"""Verify the prepared vendor TX entry point's native adapter ownership."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
from test_pon_lifecycle import function
from test_pon_identity import BSP


class PonAdapterTests(unittest.TestCase):
    def test_vendor_transmit_consumes_or_frees_once(self):
        header = (BSP.parent / 'xpon-en757x/xpon_10g/inc/pwan/xpon_netif.h').read_text(errors='replace')
        start = header.index('typedef union {', header.index('} PWAN_FERxMsg_T ;'))
        end = header.index('} PWAN_FETxMsg_T ;', start) + len('} PWAN_FETxMsg_T ;')
        fixture = Path(__file__).with_name('pon_adapter_tx_fixture.c').read_text()
        run_c(fixture.replace('/* TYPES */', header[start:end]).replace(
            '/* PRODUCTION */', function('pwan/xpon_netif.c', 'pwan_net_start_xmit_impl') +
            function('pwan/xpon_netif.c', 'pwan_net_start_xmit')))


if __name__ == '__main__':
    unittest.main()
