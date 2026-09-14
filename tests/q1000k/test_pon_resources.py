#!/usr/bin/env python3
"""Compile the real PON resource accessors and SCU bridge against MMIO fixtures."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / 'package/kernel/airoha-pon/src/bsp'


def run_c(code):
    with tempfile.TemporaryDirectory(prefix='q1000k-pon-resources-') as tmp:
        source = Path(tmp) / 'test.c'
        binary = Path(tmp) / 'test'
        source.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
                        '-I', str(SRC / 'include'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


class PonResourceTests(unittest.TestCase):
    def test_register_windows_and_absent_provider(self):
        source = (SRC / 'core/an7581_xpon.c').read_text()
        body = source[source.index('struct an7581_xpon {'):source.index('static int an7581_xpon_probe')]
        body = body[:body.index("int an7581_xpon_reset(void)")]
        body = body.replace("static DEFINE_MUTEX(xpon_lifecycle);", "")
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <an7581_xpon_map.h>
typedef uint32_t u32;
#include <an7581_xpon.h>
#define __iomem
#define EXPORT_SYMBOL(x)
#define DEFINE_RWLOCK(x) int x
#define pr_err_ratelimited(...) ((void)0)
static unsigned int held, reads, writes;
#define read_lock_irqsave(l,f) do { (void)(l); assert(!held); held=1; (f)=0; } while (0)
#define read_unlock_irqrestore(l,f) do { (void)(l); (void)(f); assert(held); held=0; } while (0)
#define write_lock_irqsave read_lock_irqsave
#define write_unlock_irqrestore read_unlock_irqrestore
static void udelay(unsigned int n) { (void)n; assert(held); }
struct device { int id; };
static unsigned char memory[3][0x1000] __attribute__((aligned(4)));
static void check_address(void *p) {
    unsigned int b;
    assert(held);
    for (b=0; b<3; b++) {
        uintptr_t off=(uintptr_t)p-(uintptr_t)memory[b];
        if (off <= an7581_xpon_sizes[b]-4 && !(off & 3)) return;
    }
    assert(!"out-of-window MMIO access");
}
static u32 readl(void *p) { check_address(p); reads++; return *(u32 *)p; }
static void writel(u32 v, void *p) { check_address(p); writes++; *(u32 *)p=v; }
''' + body + r'''
int main(void) {
    struct device dev={1};
    struct an7581_xpon provider={ .dev=&dev, .irq={74,66} };
    unsigned int reg, b, last_reads, last_writes, expected;
    assert(!get_xpon_dev());
    assert(get_xpon_irq(0)==-ENODEV);
    assert(get_xpon_irq(-1)==-EINVAL && get_xpon_irq(2)==-EINVAL);
    assert(get_xpon_data(0x5000)==UINT32_MAX);
    set_xpon_data(0x5000, 1);
    assert(!reads && !writes);
    for (b=0;b<3;b++) provider.base[b]=memory[b];
    xpon=&provider;
    assert(get_xpon_dev()==&dev && get_xpon_irq(0)==74 && get_xpon_irq(1)==66);
    /* Includes every misaligned address and the tails of each OEM window. */
    for (reg=0;reg<0x8000;reg++) {
        bool valid=false;
        for (b=0;b<3;b++) {
            unsigned int start=0x4000+b*0x1000;
            if (reg>=start && reg<=start+an7581_xpon_sizes[b]-4 && !(reg&3)) valid=true;
        }
        last_reads=reads; last_writes=writes;
        expected=reg ^ 0x12345678;
        set_xpon_data(reg, expected);
        assert(get_xpon_data(reg)==(valid ? (reg==0x5004 ? 0 : expected) : UINT32_MAX));
        assert(reads-last_reads==(unsigned)valid);
        assert(writes-last_writes==(unsigned)(valid && reg!=0x5004));
    }
    last_reads=reads; last_writes=writes;
    provider.resetting=true;
    assert(get_xpon_data(0x5000)==UINT32_MAX);
    set_xpon_data(0x5000, 1);
    assert(an7581_xpon_mac_stop(1,true)==-EBUSY);
    assert(an7581_xpon_mac_wait_tx_empty()==-EBUSY);
    assert(reads==last_reads && writes==last_writes);
    provider.resetting=false;
    set_xpon_data(0x1fb65000, 1); /* Reject physical and old KSEG addresses. */
    set_xpon_data(0xbfb65000, 1);
    assert(get_xpon_data(UINT32_MAX)==UINT32_MAX);
    assert(reads==last_reads && writes==last_writes);
    xpon=NULL;
    assert(get_xpon_irq(1)==-ENODEV && !get_xpon_dev());
    set_xpon_data(0x5000, 0);
    assert(get_xpon_data(0x5000)==UINT32_MAX);
    assert(reads==last_reads && writes==last_writes);
    return 0;
}
''')

    def test_shared_scu_atomic_masks_and_errors(self):
        override = os.environ.get('Q1000K_PON_BSP')
        bsp = Path(override) if override else next(REPO.glob(
            'build_dir/target-*/linux-airoha_an7581/airoha-pon-v2/airoha-pon/bsp'))
        source = (bsp / 'core/ecnt_scu.c').read_text()
        start = source.index('static u32 an7581_scu_read(')
        end = source.index('\n#endif', start)
        body = source[start:end]
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
#define pr_err_ratelimited(...) ((void)0)
struct regmap { u32 data[0x970/4]; };
static struct regmap map;
static unsigned int reads, writes, updates;
static int fail;
static int regmap_read(struct regmap *m, u32 r, u32 *v) {
    reads++; if (fail) return -EIO; *v=m->data[r/4]; return 0;
}
static int regmap_write(struct regmap *m, u32 r, u32 v) {
    writes++; if (fail) return -EIO; m->data[r/4]=v; return 0;
}
static int regmap_update_bits(struct regmap *m, u32 r, u32 mask, u32 v) {
    updates++; if (fail) return -EIO;
    m->data[r/4]=(m->data[r/4]&~mask)|(v&mask); return 0;
}
''' + body + r'''
int main(void) {
    unsigned int i, calls;
    const unsigned bad[]={1,0x96d,0x970,0x1fb00000,UINT32_MAX};
    map.data[0x228/4]=0xa500a5a5;
    an7581_scu_update(&map,0x388,0x228,0x0f00,0xffff0500);
    assert(map.data[0x228/4]==0xa500a5a5 && updates==1 && !reads && !writes);
    an7581_scu_update(&map,0x388,0x228,0x0f00,0x0a00);
    assert(map.data[0x228/4]==0xa500aaa5 && updates==2);
    /* A full write uses write semantics, even if a strobe already reads as 1. */
    an7581_scu_update(&map,0x970,0x96c,~0U,0x1234);
    an7581_scu_update(&map,0x970,0x96c,~0U,0x1234);
    assert(writes==2 && !reads && updates==2);
    assert(an7581_scu_read(&map,0x970,0x96c)==0x1234);
    calls=reads+writes+updates;
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        assert(an7581_scu_read(&map,0x970,bad[i])==UINT32_MAX);
        an7581_scu_update(&map,0x970,bad[i],~0U,0);
    }
    assert(an7581_scu_read(NULL,0x970,0)==UINT32_MAX);
    an7581_scu_update(NULL,0x970,0,~0U,0);
    assert(an7581_scu_read(&map,0x388,0x388)==UINT32_MAX);
    assert(reads+writes+updates==calls);
    fail=1;
    assert(an7581_scu_read(&map,0x970,0x96c)==UINT32_MAX);
    an7581_scu_update(&map,0x970,0x96c,~0U,0);
    an7581_scu_update(&map,0x970,0x96c,0xff,0);
    assert(map.data[0x96c/4]==0x1234);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
