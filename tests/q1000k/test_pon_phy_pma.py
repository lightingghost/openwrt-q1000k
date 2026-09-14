#!/usr/bin/env python3
"""Exercise the production analog phase owner without MMIO or device access."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
from test_pon_phy_lifecycle import PHY, production_source
from test_pon_identity import BSP, extract


class PhyPmaTests(unittest.TestCase):
    def test_signal_absent_present_and_every_phase_failure(self):
        source, regs = production_source(PHY / 'src/q1000k_phy_pma.c')
        fixture = Path(__file__).with_name('pon_phy_pma_fixture.c').read_text()
        run_c(fixture.replace('/* REGISTERS */', regs).replace('/* PRODUCTION */', source))

    def test_prepared_caller_propagates_analog_errors(self):
        source = (BSP.parent / 'xpon-en757x/xpon_phy_10g/src/en7581.c').read_text(encoding='latin1')
        run_c(r'''
#include <assert.h>
#include <errno.h>
#define TCSUPPORT_CPU_EN7581 1
#define FALSE 0
static struct { int pma_init_done; } phy, *gpPhyPriv=&phy;
static int context_error, step, fail, optimized, after_opt_error;
static int next(void) { return ++step==fail ? -ETIMEDOUT : 0; }
static int q1000k_phy_callback_context(void) { return context_error; }
static int an7581_pon_phy_status(void) { return after_opt_error && optimized ? -EIO : next(); }
static int q1000k_phy_controller_check(void) { return next(); }
static int xpon_pma_param_init(void) { return next(); }
static void xpon_pma_param_disp(void) {}
static int xpon_pma_mode_init(void) { int ret=next(); if(!ret) phy.pma_init_done=1; return ret; }
static void xpon_pma_param_opt(void) { assert(phy.pma_init_done); optimized++; }
''' + extract(source, 'xpon_pma_init') + r'''
int main(void)
{
    assert(!xpon_pma_init() && phy.pma_init_done && optimized==1);
    int operations=step;
    for(int i=1;i<=operations;i++) {
        step=optimized=0; fail=i; phy.pma_init_done=0;
        assert(xpon_pma_init()==-ETIMEDOUT && !phy.pma_init_done && step==i);
        if(i<=3) assert(!optimized);
    }
    step=optimized=fail=0; after_opt_error=1;
    assert(xpon_pma_init()==-EIO && optimized==1 && !phy.pma_init_done);
    step=optimized=after_opt_error=0; context_error=-EPERM;
    assert(xpon_pma_init()==-EPERM && !step && !optimized);
    return 0;
}
''')
