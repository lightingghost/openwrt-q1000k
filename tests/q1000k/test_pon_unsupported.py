#!/usr/bin/env python3
"""Verify unsupported OEM controls fail without register or user-output writes."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_pon_identity import BSP, extract


class PonUnsupportedTests(unittest.TestCase):
    def test_rejection_and_ioctl_error_propagation(self):
        mac = BSP.parent / 'xpon-en757x/xpon_10g'
        xmcs = (mac / 'src/xmcs/xmcs_if.c').read_text(errors='replace')
        ic = (mac / 'src/ic/AN7581.c').read_text(errors='replace')
        header = (mac / 'inc/common/xpon_global.h').read_text(errors='replace')
        start = header.index('#define COPY_TO_USER(')
        end = header.index('/************************************************************************', start)
        source = header[start:end]
        source += extract(xmcs, 'xmcs_set_storm_ctrl_config')
        source += extract(xmcs, 'xmcs_get_storm_ctrl_config')
        source += extract(ic, 'an7581_epon_set_llid_tx_fec')
        start = xmcs.index('case IO_IOG_STORM_CTL_CONFIG:')
        end = xmcs.index('case IO_IOG_DBG_LEVEL', start)
        # Exercise the actual dispatch case, including its production copy macros.
        source += 'static int ioctl_get(int cmd, unsigned long arg) { int ret=0; switch(cmd) {\n'
        source += xmcs[start:end]
        source += 'default: return -EINVAL; } return ret; }\n'
        code = r'''
#include <assert.h>
#include <errno.h>
#include <string.h>
#define Q1000K_PON_IDENTITY
#define IO_IOG_STORM_CTL_CONFIG 1
#define printk(...) ((void)0)
/* Unsupported controls intentionally ignore their old argument contracts. */
#pragma GCC diagnostic ignored "-Wunused-parameter"
struct XMCS_StormCtrlConfig_S { unsigned int threld,timer; };
static int user_writes;
static int copy_from_user(void *dst, const void *src, unsigned int n) {
    memcpy(dst,src,n); return 0;
}
static int copy_to_user(void *dst, const void *src, unsigned int n) {
    user_writes++; memcpy(dst,src,n); return 0;
}
/* No MMIO/FE/QDMA accessors exist in this fixture. */
''' + source + r'''
int main(void) {
    struct XMCS_StormCtrlConfig_S cfg={7,123},before=cfg;
    unsigned char in[32],out[32],old[32];
    memset(in,0x56,sizeof(in)); memset(out,0xab,sizeof(out)); memcpy(old,out,sizeof(old));
    assert(xmcs_set_storm_ctrl_config(&cfg)==-EOPNOTSUPP);
    assert(xmcs_get_storm_ctrl_config(&cfg)==-EOPNOTSUPP);
    assert(!memcmp(&cfg,&before,sizeof(cfg)));
    assert(xmcs_set_storm_ctrl_config(NULL)==-EOPNOTSUPP);
    assert(xmcs_get_storm_ctrl_config(NULL)==-EOPNOTSUPP);
    assert(an7581_epon_set_llid_tx_fec(in,out)==-EOPNOTSUPP);
    assert(!memcmp(out,old,sizeof(out)));
    assert(an7581_epon_set_llid_tx_fec(NULL,NULL)==-EOPNOTSUPP);
    assert(ioctl_get(IO_IOG_STORM_CTL_CONFIG,(unsigned long)&cfg)==-EOPNOTSUPP);
    assert(!user_writes && !memcmp(&cfg,&before,sizeof(cfg)));
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-pon-unsupported-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
