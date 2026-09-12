#!/usr/bin/env python3
"""Run the built kernel's real PSE reader against a read-only register fixture.

Run after `make target/linux/prepare`. This complements the target compiler;
it does not emulate the physical PSE or assert hardware measurements.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import os

REPO = Path(__file__).resolve().parents[2]


class PseTests(unittest.TestCase):
    def test_driver_snapshot(self):
        roots = list(REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-*/drivers/net/ethernet/airoha'))
        self.assertEqual(len(roots), 1, 'prepare one AN7581 kernel tree first')
        root = roots[0]
        source = Path(os.environ.get('Q1000K_PSE_SOURCE', root / 'airoha_ppe_debugfs.c')).read_text()
        start = source.index('#define REG_PSE_SHARE_BUF_STA')
        end = source.index('DEFINE_SHOW_ATTRIBUTE(airoha_ppe_debugfs_pse)', start)
        reader = source[start:end]
        # No write accessor is defined: any accidental writes fail to compile/link.
        harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(mask,v) (((v) & (mask)) >> __builtin_ctzl(mask))
#include "airoha_regs.h"
struct airoha_gdm_port { int lock; };
struct airoha_eth { u32 regs[16]; unsigned seen; struct airoha_gdm_port *ports[4]; };
static void spin_lock_bh(int *lock) { assert(!*lock); *lock=1; }
static void spin_unlock_bh(int *lock) { assert(*lock); *lock=0; }
struct airoha_ppe { struct airoha_eth *eth; };
struct seq_file { void *private; };
static u32 fake_read(struct airoha_eth *eth, u32 reg) {
    unsigned i;
    switch(reg) { case 0x104: i=0; break; case 0x8c: i=1; break;
                  case 0x90: i=2; break; case 0x94: i=3; break;
                  case 0x5a4: i=14; break; case 0x15a4: i=15; break;
                  default:
                    assert(reg>=0x120 && reg<=0x144 && !(reg&3));
                    i=4+(reg-0x120)/4; break; }
    if (i>=14 && eth->ports[i-14]) assert(eth->ports[i-14]->lock==1);
    assert(!(eth->seen & (1U << i))); /* exactly one read per register */
    eth->seen |= 1U << i;
    return eth->regs[i];
}
#define airoha_fe_rr(eth,reg) fake_read(eth,reg)
#define airoha_fe_get(eth,reg,mask) FIELD_GET(mask,airoha_fe_rr(eth,reg))
static void __attribute__((format(printf,2,3))) seq_printf(struct seq_file *m, const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap);
}
''' + reader + r'''
int main(int argc, char **argv) {
    struct airoha_eth eth = {0};
    struct airoha_ppe ppe = {.eth=&eth};
    struct seq_file m = {.private=&ppe};
    assert(argc==18);
    for(int i=0;i<16;i++) eth.regs[i]=strtoul(argv[i+1],NULL,0);
    struct airoha_gdm_port ports[2] = {0};
    unsigned present=strtoul(argv[17],NULL,0);
    for(int i=0;i<2;i++) if (present & (1<<i)) eth.ports[i]=&ports[i];
    assert(airoha_ppe_debugfs_pse_show(&m,NULL)==0);
    assert(eth.seen==65535);
    assert(ports[0].lock==0 && ports[1].lock==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-pse-') as temp:
            c = Path(temp) / 'test.c'; exe = Path(temp) / 'test'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-I', str(root), str(c), '-o', str(exe)], check=True)
            fixtures = [
                ([0x001005dc, 2048, 512, 1504], dict(version=2,total=2048,reserved=512,used=16,free=1500,high=1504)),
                ([0x06000000, 2048, 512, 1504], dict(version=2,total=2048,reserved=512,used=1536,free=0,high=1504)),
                ([0x12348042, 0xffff8800, 0xabcd8200, 0xdddd05e0], dict(version=2,total=2048,reserved=512,used=4660,free=66,high=1504))]
            for regs, expected in fixtures:
                counters = [0,1,2,100,0xffffffff,5,6,7,8,9,0xffffffff,12]
                expected.update({f'pse_drop{i}':counters[i] for i in range(10)})
                expected.update(cdm1_hwf_drop=counters[10],cdm2_hwf_drop=counters[11])
                for present in range(4):
                    result = subprocess.check_output([str(exe), *map(str,regs+counters+[present])], text=True)
                    actual = dict((k,int(v)) for k,v in (line.split() for line in result.splitlines()))
                    self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()
