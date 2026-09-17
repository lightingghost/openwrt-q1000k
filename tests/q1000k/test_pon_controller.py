#!/usr/bin/env python3
"""Compile the production controller lease API with lifetime/fault injection."""
from pathlib import Path
import unittest
from pon_test_utils import run_c

ROOT=Path(__file__).resolve().parents[2]
class ControllerTests(unittest.TestCase):
    def test_portable_loader_and_receiver_snapshots(self):
        directory = ROOT / 'package/kernel/q1000k-pon-control/src'
        fixture = Path(__file__).with_name('test_en7573.c').read_text()
        run_c(fixture, flags=['-O2', '-I' + str(directory), str(directory / 'en7573.c')])

    def test_tx_session_policy_truth_table(self):
        policy=(ROOT/'package/kernel/q1000k-pon-control/src/q1000k_pon_policy.h').read_text()
        run_c('#include <assert.h>\n#include <stdbool.h>\n#include <errno.h>\n'+policy+r"""
int main(void) {
    for(int hard=0;hard<2;hard++) for(int validation=0;validation<2;validation++) for(int allow=0;allow<2;allow++) {
        bool inhibit=false;
        int ret=q1000k_pon_tx_policy(hard,validation,allow,&inhibit);
        if(allow && (hard || !validation)) assert(ret==-EACCES);
        else assert(!ret && inhibit==(hard || (validation && !allow)));
    }
    return 0;
}
""")

    def test_output_poweroff_contains_restore_failure(self):
        source=(ROOT/'package/kernel/q1000k-pon-control/src/driver.c').read_text()
        body=source[source.index('static int pon_off('):source.index('static int pon_select(')]
        fixture=r"""
#include <assert.h>
#include <stdbool.h>
struct en7573_io { int unused; };
struct en7573_oem_post { bool saved; };
struct en7573_rx_output { unsigned int control, shape; bool saved; };
struct q1000k_pon {
    struct en7573_io io;
    struct en7573_oem_post oem_post_original;
    struct en7573_rx_output rx_output_original;
    int *power[2]; bool initialized, tx_enabled; int mode;
};
static int restore_error, gpio_error, restores, powers;
static int pon_board_gate(struct q1000k_pon *p, bool disable) { assert(disable); return 0; }
static int disable_error, disables;
static int en7573_set_tx(struct en7573_io *io, bool enabled)
{ (void)io; assert(!enabled); disables++; return disable_error; }
static int en7573_restore_rx_output(struct en7573_io *io, struct en7573_rx_output *original)
{
    (void)io; restores++;
    if (!restore_error) original->saved=false;
    return restore_error;
}
static int en7573_oem_post_init(struct en7573_io *io,struct en7573_oem_post *o,bool restore)
{ (void)io; assert(restore); restores++; if(!restore_error) o->saved=false; return restore_error; }
static int gpiod_set_value_cansleep(int *gpio,int value)
{
    assert(!value); powers++; *gpio=value;
    return powers==1 ? gpio_error : 0;
}
/* PRODUCTION */
int main(void)
{
    int a=1,b=1;
    struct q1000k_pon pon={.power={&a,&b},.initialized=true,.tx_enabled=true,.mode=1};
    pon.rx_output_original.saved=true;
    restore_error=-5;
    assert(pon_off(&pon)==-5 && restores==1 && powers==2);
    assert(!a && !b && !pon.initialized && !pon.tx_enabled && pon.mode==-1);
    assert(!pon.rx_output_original.saved && disables==1);
    powers=restores=0;
    pon.rx_output_original.saved=true; gpio_error=-6;
    assert(pon_off(&pon)==-5 && restores==1 && powers==2 && pon.mode==-2);
    assert(pon.rx_output_original.saved);
    powers=restores=0; restore_error=gpio_error=0;
    assert(!pon_off(&pon) && restores==1 && powers==2 && pon.mode==-1);
    powers=restores=0;
    assert(!pon_off(&pon) && !restores && powers==2);
    pon.initialized=pon.tx_enabled=pon.oem_post_original.saved=true;
    powers=restores=0; disable_error=-5;
    assert(pon_off(&pon)==-5 && !restores && powers==2 && !pon.tx_enabled);
    return 0;
}
"""
        run_c(fixture.replace('/* PRODUCTION */',body))

    def test_consumer_lifetime_and_failures(self):
        source=(ROOT/'package/kernel/q1000k-pon-control/src/driver.c').read_text()
        body=source[source.index('/* Kernel consumer lifecycle:'):source.index('/* End kernel consumer lifecycle. */')]
        fixture=Path(__file__).with_name('pon_controller_fixture.c').read_text()
        board=source[source.index('static int pon_board_gate('):source.index('static int pon_off(')]
        run_c(fixture.replace('/* PRODUCTION */',body).replace('/* BOARD GATE */',board), flags=['-Wno-misleading-indentation','-I'+str(ROOT/'package/kernel/q1000k-pon-control/src')])
if __name__=='__main__': unittest.main()
