#!/usr/bin/env python3
"""Exercise production FE/RX drain orchestration and DMA/NAPI failure paths."""
from pathlib import Path
import os
import re
import unittest
from pon_test_utils import run_c
REPO=Path(__file__).resolve().parents[2]
ETH=Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))

class RxDrainTests(unittest.TestCase):
    def test_dma_idle_irq_napi_ownership_and_errors(self):
        source=(ETH/'airoha_eth.c').read_text()
        start=source.index('static int airoha_qdma_pon_rx_irq(')
        end=source.index('static int airoha_qdma_rx_napi_poll(',start)
        source=source[start:end]
        fixture=Path(__file__).with_name('pon_rx_drain_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',source),flags=['-Wno-sign-compare'])

    def test_fe_boundary_and_receive_admission(self):
        source=(ETH/'airoha_pon.c').read_text()
        start=source.index('int airoha_pon_drain_rx(')
        end=source.index('EXPORT_SYMBOL_GPL(airoha_pon_drain_rx);',start)
        source=source[start:end]
        fixture=Path(__file__).with_name('pon_rx_boundary_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */',source))
if __name__=='__main__': unittest.main()
