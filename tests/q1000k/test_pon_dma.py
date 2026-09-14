#!/usr/bin/env python3
"""Exercise the actual native descriptor and RX assembly paths with DMA fixtures."""
from pathlib import Path
import os
import re
import unittest
from pon_test_utils import run_c

REPO = Path(__file__).resolve().parents[2]
ETH = Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(
    REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))


def function(name):
    source = (ETH / 'airoha_eth.c').read_text()
    match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


class PonDmaTests(unittest.TestCase):
    def test_descriptor_metadata_busy_dma_failure_and_cleanup(self):
        source = Path(__file__).with_name('pon_tx_fixture.c').read_text()
        masks = '\n'.join(line for line in (ETH / 'airoha_regs.h').read_text().splitlines()
                          if re.match(r'#define (?:QDMA_(?:DESC_|ETH_TXMSG_)|IRQ_(?:HEAD|ENTRY|RING|DESC|CLEAR))', line))
        source = source.replace('/* MASKS */', masks)
        source = source.replace('/* PRODUCTION */', function('__airoha_dev_xmit') +
                                function('airoha_qdma_cleanup_tx_queue') +
                                function('airoha_qdma_tx_napi_poll'))
        run_c(source, flags=['-Wno-sign-compare'])

    def test_raw_rx_scatter_errors_and_stale_buffered_frames(self):
        source = Path(__file__).with_name('pon_rx_fixture.c').read_text()
        masks = '\n'.join(line for line in ((ETH / 'airoha_regs.h').read_text() + (ETH / 'airoha_eth.h').read_text()).splitlines()
                          if re.match(r'#define (?:QDMA_(?:DESC_|ETH_RXMSG_)|AIROHA_RXD4_)', line))
        source = source.replace('/* MASKS */', masks)
        pon = (ETH / 'airoha_pon.c').read_text()
        start = pon.index('static int airoha_pon_rx_meta(')
        end = pon.index('\n}\n', start) + 3
        source = source.replace('/* RX_META */', pon[start:end])
        source = source.replace('/* PRODUCTION */', function('airoha_qdma_build_rx_skb') +
                                function('airoha_qdma_rx_process') +
                                function('airoha_qdma_pon_discard_rx'))
        run_c(source, flags=['-Wno-sign-compare'])


if __name__ == '__main__':
    unittest.main()
