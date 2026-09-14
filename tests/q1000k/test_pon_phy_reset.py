#!/usr/bin/env python3
"""Check the production PHY reset sequence and its shared-SCU boundaries."""
from pathlib import Path
import os
import unittest
from pon_test_utils import run_c
from test_pon_phy_lifecycle import PHY, production_source

class PhyResetTests(unittest.TestCase):
    def test_sequence_and_failure_containment(self):
        source, regs = production_source(PHY / 'src/q1000k_phy_reset.c')
        fixture = Path(__file__).with_name('pon_phy_reset_fixture.c').read_text()
        run_c(fixture.replace('/* REGISTERS */', regs).replace('/* PRODUCTION */', source))

    def test_scu_mask_and_errors(self):
        repo = Path(__file__).resolve().parents[2]
        bsp = Path(os.environ['Q1000K_PON_BSP']) if 'Q1000K_PON_BSP' in os.environ else next(repo.glob('build_dir/target-*/linux-airoha_an7581/airoha-pon-v2/airoha-pon/bsp'))
        source = (bsp / 'core/ecnt_scu.c').read_text(encoding='latin1')
        source = source[source.index('int an7581_pon_wan_get('):source.index('static u32 an7581_scu_read(')]
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define EXPORT_SYMBOL(x)
struct regmap { int unused; };
static struct regmap map, *an7581_np_scu;
static u32 wan, pbus, mask_seen, reg_seen;
static int calls, fail, drop;
static int regmap_read(struct regmap *m,u32 reg,u32 *value)
{
    assert(m==&map); if(++calls==fail) return -ETIMEDOUT;
    assert(reg==0x70 || reg==0x92c); *value=reg==0x70 ? wan : pbus; return 0;
}
static int regmap_update_bits(struct regmap *m,u32 reg,u32 mask,u32 value)
{
    u32 *p=reg==0x70 ? &wan : &pbus;
    assert(m==&map && (reg==0x70 || reg==0x92c));
    reg_seen=reg; mask_seen=mask;
    if(++calls==fail) return -ETIMEDOUT;
    if(!drop) *p=(*p&~mask)|(value&mask); return 0;
}
''' + source + r'''
int main(void)
{
    u32 mode=123, n;
    assert(an7581_pon_wan_get(&mode)==-ENODEV && mode==123);
    assert(an7581_pon_wan_set(10)==-ENODEV && !calls);
    assert(an7581_pon_pbus_enable()==-ENODEV && !calls);
    an7581_np_scu=&map;
    assert(an7581_pon_wan_get(NULL)==-EINVAL && !calls);
    assert(an7581_pon_wan_set(0)==-EINVAL && !calls);
    wan=0xabcd340a;
    assert(!an7581_pon_wan_get(&mode) && mode==10);
    assert(!an7581_pon_wan_set(17) && wan==0xabcd3411 && reg_seen==0x70 && mask_seen==0xff);
    assert(!an7581_pon_wan_set(10) && wan==0xabcd340a);
    pbus=0xaabbccdd;
    assert(!an7581_pon_pbus_enable() && pbus==(0xaabbccdd&~4U) && reg_seen==0x92c && mask_seen==4);
    for(n=1;n<=2;n++) {
        fail=n; calls=0; assert(an7581_pon_wan_set(17)==-ETIMEDOUT);
        calls=0; assert(an7581_pon_pbus_enable()==-ETIMEDOUT);
    }
    fail=0; wan=~0U;
    assert(an7581_pon_wan_get(&mode)==-EIO);
    drop=1; assert(an7581_pon_wan_set(10)==-EIO);
    pbus=~0U; assert(an7581_pon_pbus_enable()==-EIO);
    return 0;
}
''', flags=['-Wno-misleading-indentation'])

if __name__ == '__main__':
    unittest.main()
