#!/usr/bin/env python3
"""Fault-inject the native FE release transaction without device access."""
import os
import re
import unittest
from pathlib import Path
from pon_test_utils import run_c

REPO = Path(__file__).resolve().parents[2]
ETH = Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(
    REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))


class FeRetireTests(unittest.TestCase):
    def test_channels_completion_and_every_failed_write(self):
        source = (ETH / 'airoha_pon.c').read_text()
        start = source.index('static int airoha_pon_fe_write_checked')
        end = 'EXPORT_SYMBOL_GPL(airoha_pon_retire_fe);'
        source = source[start:source.index(end) + len(end)]
        regs = (ETH / 'airoha_regs.h').read_text()
        defines = '\n'.join(line for line in regs.splitlines() if re.match(
            r'#define (?:REG_GDM_(?:CHN_|TXCHN|RXCHN|LPBK)|GDM_RXCHN_EN_MASK|REG_CDM_HWFWD|REG_CHAN_QUEUE_STATUS|REG_QUEUE_CLOSE|MBI_.*AGE_SEL|LPBK_EN_MASK|[GC]DM[1-4]_BASE)', line))
        for name in ['CDM_BASE', 'GDM_BASE']:
            defines += '\n' + re.search(r'#define ' + name + r'\(_n\).*?(?=\n(?:#|\n))', regs, re.S).group()
        before, after = Path(__file__).with_name('pon_fe_retire_fixture.c').read_text().split('/* PRODUCTION */')
        run_c(before + defines + '\n' + source + after)


if __name__ == '__main__':
    unittest.main()
