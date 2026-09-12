#!/usr/bin/env python3
"""Run the built kernel's real PSE reader against a read-only register fixture.

Run after `make target/linux/prepare`. This complements the target compiler;
it does not emulate the physical PSE or assert hardware measurements.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class PseTests(unittest.TestCase):
    def test_driver_snapshot(self):
        roots = list(REPO.glob('build_dir/target-*/linux-airoha_an7581/linux-*/drivers/net/ethernet/airoha'))
        self.assertEqual(len(roots), 1, 'prepare one AN7581 kernel tree first')
        root = roots[0]
        source = (root / 'airoha_ppe_debugfs.c').read_text()
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
struct airoha_eth { u32 regs[4]; unsigned seen; };
struct airoha_ppe { struct airoha_eth *eth; };
struct seq_file { void *private; };
static u32 fake_read(struct airoha_eth *eth, u32 reg) {
    unsigned i;
    switch(reg) { case 0x104: i=0; break; case 0x8c: i=1; break;
                  case 0x90: i=2; break; case 0x94: i=3; break;
                  default: abort(); }
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
    assert(argc==5);
    for(int i=0;i<4;i++) eth.regs[i]=strtoul(argv[i+1],NULL,0);
    assert(airoha_ppe_debugfs_pse_show(&m,NULL)==0);
    assert(eth.seen==15);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-pse-') as temp:
            c = Path(temp) / 'test.c'; exe = Path(temp) / 'test'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-I', str(root), str(c), '-o', str(exe)], check=True)
            fixtures = [
                ([0x001005dc, 2048, 512, 1504], dict(version=1,total=2048,reserved=512,used=16,free=1500,high=1504)),
                ([0x06000000, 2048, 512, 1504], dict(version=1,total=2048,reserved=512,used=1536,free=0,high=1504)),
                ([0x12348042, 0xffff8800, 0xabcd8200, 0xdddd05e0], dict(version=1,total=2048,reserved=512,used=4660,free=66,high=1504))]
            for regs, expected in fixtures:
                result = subprocess.check_output([str(exe), *map(str,regs)], text=True)
                actual = dict((k,int(v)) for k,v in (line.split() for line in result.splitlines()))
                self.assertEqual(actual, expected)


if __name__ == '__main__':
    unittest.main()
