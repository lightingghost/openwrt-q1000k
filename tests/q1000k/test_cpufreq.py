#!/usr/bin/env python3
"""Compile the production CPU clock routines against an ordered MMIO fixture."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import os

REPO = Path(__file__).resolve().parents[2]


class CpuFreqTests(unittest.TestCase):
    def test_clock_reading_and_transitions(self):
        override = os.environ.get('Q1000K_CPU_SOURCE')
        source = Path(override) if override else next(REPO.glob(
            'build_dir/target-*/linux-airoha_an7581/linux-*/drivers/pmdomain/mediatek/airoha-cpu-pmdomain.c'))
        code = source.read_text()
        code = code[code.index('/* ATF SMC interface'):code.index('static int airoha_cpu_pmdomain_probe')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(m,v) (((v) & (m)) >> __builtin_ctzl(m))
#define FIELD_PREP(m,v) (((unsigned long)(v) << __builtin_ctzl(m)) & (m))
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
typedef int raw_spinlock_t;
static int locked, stopped, stops, writes, pll_writes, delays;
#define raw_spin_lock_irqsave(l,f) do { assert(!locked); locked=1; (f)=0; } while(0)
#define raw_spin_unlock_irqrestore(l,f) do { assert(locked); locked=0; (void)(f); } while(0)
struct clk_hw { int unused; };
struct clk_rate_request { int unused; };
struct generic_pm_domain { int unused; };
struct clk_ops {
 unsigned long (*recalc_rate)(struct clk_hw *, unsigned long);
 int (*is_enabled)(struct clk_hw *);
 int (*determine_rate)(struct clk_hw *, struct clk_rate_request *);
};
struct arm_smccc_res { unsigned long a0; };
static unsigned long smc_result;
#define arm_smccc_1_1_invoke(a,b,c,d,e,f,g,h,r) ((r)->a0=smc_result)
struct regmap { u32 regs[0x300/4]; };
static struct regmap map;
static u32 mcu[0x800/4];
static int read_fail=-1, write_fail=-1, corrupt=-1, block_source=-1;
static unsigned source(void) { return (mcu[0x7c0/4] >> 9) & 3; }
static int regmap_read(struct regmap *m, unsigned reg, u32 *value) {
 assert(locked);
 if ((int)reg==read_fail) { read_fail=-1; return -EIO; }
 *value=m->regs[reg/4];
 if ((int)reg==corrupt && pll_writes>=2) { *value^=1; corrupt=-1; }
 return 0;
}
static int regmap_write(struct regmap *m, unsigned reg, u32 value) {
 assert(locked && stopped); writes++;
 if ((int)reg==write_fail) { write_fail=-1; return -EIO; }
 if (reg==0x1e0) assert(value & (1U << (source()-1))); /* never gate active source */
 if (reg==0x2b4 || reg==0x2b8) {
   assert(source()==3); assert((m->regs[0x268/4] & 255)==0x12); pll_writes++;
 }
 m->regs[reg/4]=value;
 return 0;
}
static u32 readl(void *p) { assert(locked); return *(u32 *)p; }
static void writel(u32 value, void *p) {
 assert(locked && stopped); writes++;
 if (p==&mcu[0x7c0/4]) {
   unsigned next=(value>>9)&3;
   assert((mcu[0x640/4]&31)==0x12);
   if ((int)next==block_source) { block_source=-1; return; }
   assert(map.regs[0x1e0/4] & (1U << (next-1)));
   if (next==1 && pll_writes) assert(delays>0);
 }
 *(u32 *)p=value;
}
static void udelay(unsigned us) { assert(stopped && locked && source()==3 && us>=20); delays++; }
static int stop_machine(int (*cb)(void *), void *arg, void *mask) {
 assert(!stopped && !locked); stopped=1; stops++;
 int ret=cb(arg); assert(!locked); stopped=0; return ret;
}
''' + code + r'''
static struct airoha_cpu_pmdomain_priv priv;
static void reset(void) {
 memset(&map,0,sizeof(map)); memset(mcu,0,sizeof(mcu)); memset(&priv,0,sizeof(priv));
 priv.pll_map=&map; priv.mcucfg=mcu;
 map.regs[0x1e0/4]=0xa001; map.regs[0x268/4]=0xabcdef00;
 map.regs[0x2b4/4]=0x98000000; /* bit31 retained, 24 * 50 MHz */
 map.regs[0x2b8/4]=0x80000101;
 mcu[0x640/4]=0xabcde000; mcu[0x7c0/4]=0x80000204;
 stops=writes=pll_writes=delays=0;
 read_fail=write_fail=corrupt=block_source=-1;
}
int main(void) {
 reset();
 unsigned long invalid[]={0,1,99,2001,0x82000301UL,~0UL};
 for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
   smc_result=invalid[i]; assert(airoha_cpu_smc_clk_get()==0);
 }
 smc_result=1200; priv.use_smc=true;
 assert(airoha_cpu_pmdomain_clk_get(&priv.hw,0)==1200000000UL);
 smc_result=0; assert(airoha_cpu_pmdomain_set_performance_state(&priv.pd,4)==0);
 smc_result=1; assert(airoha_cpu_pmdomain_set_performance_state(&priv.pd,4)==-EINVAL);
 assert(writes==0); priv.use_smc=false;
 map.regs[0x2b4/4]=0x98800000; /* include half a PCW unit */
 assert(airoha_cpu_pmdomain_clk_get(&priv.hw,0)==1225000000UL);
 map.regs[0x2b8/4]|=0x10;
 assert(airoha_cpu_pll_clk_get(&priv)==612500000UL);
 read_fail=0x2b4; assert(airoha_cpu_pll_clk_get(&priv)==0);
 mcu[0x7c0/4]=0x600; assert(airoha_cpu_pll_clk_get(&priv)==400000000UL);
 mcu[0x7c0/4]=0; assert(airoha_cpu_pll_clk_get(&priv)==0);
 reset();
 for (unsigned state=0;state<=14;state++) {
   int before=stops;
   assert(airoha_cpu_pmdomain_set_performance_state(&priv.pd,state)==0);
   assert(stops==before+1);
   assert(airoha_cpu_pll_clk_get(&priv)==(500UL+state*50)*1000000);
   assert(map.regs[0x2b4/4] & 0x80000000U);
   assert(!(map.regs[0x2b4/4] & 0xffffff));
   assert((map.regs[0x2b8/4] & ~0x71U)==0x80000100U);
   assert(map.regs[0x1e0/4]==0xa001);
   assert(map.regs[0x268/4]==0xabcdef00);
   assert(mcu[0x640/4]==0xabcde000 && mcu[0x7c0/4]==0x80000204);
   int count=writes;
   assert(airoha_cpu_pll_set_freq(&priv,state)==0 && writes==count && stops==before+1);
 }
 int count=writes;
 assert(airoha_cpu_pll_set_freq(&priv,15)==-EINVAL);
 assert(airoha_cpu_pll_set_freq(&priv,~0U)==-EINVAL && count==writes);
 reset(); read_fail=0x1e0;
 assert(airoha_cpu_pll_set_freq(&priv,0)==-EIO && writes==0);
 reset(); write_fail=0x2b4;
 assert(airoha_cpu_pll_set_freq(&priv,0)==-EIO);
 assert(airoha_cpu_pll_clk_get(&priv)==1200000000UL);
 assert(map.regs[0x268/4]==0xabcdef00 && map.regs[0x1e0/4]==0xa001);
 reset(); corrupt=0x2b8;
 assert(airoha_cpu_pll_set_freq(&priv,0)==-EIO);
 assert(airoha_cpu_pll_clk_get(&priv)==1200000000UL);
 reset(); block_source=3;
 assert(airoha_cpu_pll_set_freq(&priv,0)==-EIO && pll_writes==0);
 assert(airoha_cpu_pll_clk_get(&priv)==1200000000UL);
 reset(); block_source=1;
 assert(airoha_cpu_pll_set_freq(&priv,0)==-EIO);
 assert(airoha_cpu_pll_clk_get(&priv)==400000000UL && (map.regs[0x1e0/4]&5)==5);
 puts("SMC/PLL rates, all stock OPPs, ordered transitions and failure recovery passed");
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-cpufreq-') as temp:
            c = Path(temp) / 'test.c'
            exe = Path(temp) / 'test'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-unused-const-variable',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
