#!/usr/bin/env python3
"""Exercise the actual Q1000K PHY lifecycle with injected provider failures."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c

REPO = Path(__file__).resolve().parents[2]
BSP = REPO / 'package/kernel/airoha-pon/src/bsp'
PHY = BSP.parent / 'xpon-en757x/xpon_phy_10g'

def production_source(path=None, extra=""):
    source = (path or PHY / 'src/q1000k_phy.c').read_text()
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    definitions = {}
    headers = (PHY / 'inc/en7581_reg.h').read_text(encoding='latin1')
    headers += (BSP / 'include/ecnt_hook/ecnt_hook_pon_phy.h').read_text(encoding='latin1')
    for line in headers.splitlines():
        m = re.match(r'#define\s+(\w+)\s+(.*)', line)
        if m:
            definitions[m[1]] = m[2].split('//')[0]
    needed = set(re.findall(r'\bEN7581_\w+', source + extra + (BSP / 'include/q1000k_rx_diag.h').read_text()))
    needed.update(n for n in definitions if n.startswith('PON_GET_PHY_'))
    pending = list(needed)
    while pending:
        name = pending.pop()
        for dep in re.findall(r'\b[A-Z_]\w+', definitions[name]):
            if dep in definitions and dep not in needed:
                needed.add(dep)
                pending.append(dep)
    regs = ''.join(f'#define {n} {definitions[n]}\n' for n in sorted(needed))
    return source, regs


class PhyLifecycleTests(unittest.TestCase):
    def test_start_stop_irq_work_and_faults(self):
        source, regs = production_source()
        fixture = Path(__file__).with_name('pon_phy_lifecycle_fixture.c').read_text()
        run_c(fixture.replace('/* REGISTERS */', regs).replace('/* PRODUCTION */', source),
              flags=['-Wno-sign-compare', '-I', str(BSP / 'include')])

if __name__ == '__main__':
    unittest.main()
