#!/usr/bin/env python3
"""Exercise actual native scheduler commands; no register access on hardware."""
import os
import re
import unittest
from pathlib import Path
from pon_test_utils import run_c

REPO = Path(__file__).resolve().parents[2]
ETH = Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(
    REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))
HEADER = Path(os.environ['Q1000K_PON_HEADER']) if 'Q1000K_PON_HEADER' in os.environ else (
    ETH.parents[3] / 'include/linux/soc/airoha/airoha_pon.h')

class PonQosTests(unittest.TestCase):
    def test_channels_schedulers_commands_and_faults(self):
        source = (ETH / 'airoha_pon.c').read_text()
        source = source[source.index('/* RTNL serializes the indirect QDMA1'):source.index('int airoha_pon_set_queue_close(struct')]
        regs = '\n'.join(line for line in (ETH / 'airoha_regs.h').read_text().splitlines()
                         if re.match(r'#define (?:REG_TXWRR_|TWRR_|REG_CHAN_QOS_MODE)', line))
        struct = re.search(r'struct airoha_pon_qos \{.*?\n\};', HEADER.read_text(), re.S).group()
        before, after = Path(__file__).with_name('pon_qos_fixture.c').read_text().split('/* PRODUCTION */')
        run_c(before + regs + '\n' + struct + '\n' + source + after)

if __name__ == '__main__':
    unittest.main()
