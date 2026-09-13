#!/usr/bin/env python3
"""Validate the Q1000K populated-skb contract and real vendor RX entry point."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_identity import BSP, REPO


class PonPacketTests(unittest.TestCase):
    def test_bounds_and_vendor_delivery(self):
        production = (REPO / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/src/q1000k_packet.c').read_text()
        production = re.sub(r'^#include[^\n]*\n', '', production, flags=re.M)
        mac = BSP.parent / 'xpon-en757x/xpon_10g'
        types = (mac / 'inc/pwan/xpon_netif.h').read_text(errors='replace')
        start = types.index('typedef union {')
        end = types.index('} PWAN_FERxMsg_T ;', start) + len('} PWAN_FERxMsg_T ;')
        types = types[start:end]
        source = (mac / 'src/pwan/xpon_netif.c').read_text(errors='replace')
        start = source.index('int pwan_cb_rx_packet(')
        end = source.index('\nint pwan_cb_event_handler(', start)
        callback = re.sub(r'/\*.*?\*/|//[^\n]*', '', source[start:end], flags=re.S)
        fixture = Path(__file__).with_name('pon_packet_fixture.c').read_text()
        run_c(fixture.replace('/* TYPES */', types)
              .replace('/* HELPERS */', production).replace('/* CALLBACK */', callback),
              flags=['-Wno-sign-compare', '-Wno-pointer-sign', '-Wno-unused-label'])


if __name__ == '__main__':
    unittest.main()
