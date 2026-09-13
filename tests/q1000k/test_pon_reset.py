#!/usr/bin/env python3
"""Exercise actual Airoha reset callbacks, both polarities and regmap failures."""
from pathlib import Path
import os
import unittest
from pon_test_utils import run_c

REPO = Path(__file__).resolve().parents[2]
class ResetTests(unittest.TestCase):
    def test_reset_values_and_io_errors(self):
        path = Path(os.environ['Q1000K_RESET_SOURCE']) if 'Q1000K_RESET_SOURCE' in os.environ else next(
            REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/clk/clk-en7523.c'))
        source = path.read_text()
        source = source[source.index('static int en7523_reset_update('):source.index('static int en7523_reset_xlate(')]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define RST_NR_PER_BANK 32
#define REG_NP_SCU_PCIC 0x88
#define BIT(n) (1U<<(n))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct reset_controller_dev { int unused; };
struct regmap { u32 words[256]; int error; unsigned int calls; };
struct en_rst_data { struct reset_controller_dev rcdev; struct regmap *map; u32 bank_ofs[3]; };
static int regmap_update_bits(struct regmap *m,u32 reg,u32 mask,u32 val)
{
    m->calls++; assert(!(reg&3) && reg<1024 && !(val&~mask));
    if(m->error) return m->error;
    m->words[reg/4]=(m->words[reg/4]&~mask)|(val&mask); return 0;
}
static int regmap_read(struct regmap *m,u32 reg,u32 *val)
{
    m->calls++; if(m->error) return m->error;
    *val=m->words[reg/4]; return 0;
}
''' + source + r'''
int main(void)
{
    struct regmap map={0};
    struct en_rst_data rst={.map=&map,.bank_ofs={0x30,0x34,REG_NP_SCU_PCIC}};
    unsigned int id,pattern,n;
    const u32 patterns[]={0,~0U,0xa5a55a5a};
    for(id=0;id<96;id++) for(pattern=0;pattern<3;pattern++) {
        u32 *word=&map.words[rst.bank_ofs[id/32]/4],bit=BIT(id%32);
        bool inv=id/32==2;
        *word=patterns[pattern];
        assert(!en7523_reset_assert(&rst.rcdev,id));
        assert(*word==((patterns[pattern]&~bit)|(inv ? 0 : bit)));
        assert(en7523_reset_status(&rst.rcdev,id)==1);
        assert(!en7523_reset_deassert(&rst.rcdev,id));
        assert(*word==((patterns[pattern]&~bit)|(inv ? bit : 0)));
        assert(en7523_reset_status(&rst.rcdev,id)==0);
        for(n=0;n<2;n++) {
            u32 old=*word;
            map.error=n ? -ETIMEDOUT : -EIO;
            assert(en7523_reset_assert(&rst.rcdev,id)==map.error);
            assert(en7523_reset_deassert(&rst.rcdev,id)==map.error);
            assert(en7523_reset_status(&rst.rcdev,id)==map.error);
            assert(*word==old); map.error=0;
        }
    }
    return 0;
}
''')

if __name__ == '__main__':
    unittest.main()
