#!/usr/bin/env python3
"""Exercise the mandatory FCS-clear command before namespace epoch reuse."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
class FcsTests(unittest.TestCase):
    def test_command_busy_all_ones_timeouts_and_provider_failures(self):
        p=Path(__file__).resolve().parents[2]/'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/src/q1000k_pipeline.c'
        s=p.read_text(); start=s.index('static int q1000k_pipeline_clear_fcs(')
        s=s[start:s.index('int q1000k_pipeline_reconfigure(',start)]
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
static int status_calls, status_fail, read_calls, write_calls, delays, done_after, bad_read;
static u32 initial;
static int an7581_xpon_status(void) { return ++status_calls==status_fail ? -ENODEV : 0; }
static u32 get_xpon_data(u32 reg) {
    assert(reg==0x527c); read_calls++;
    if(read_calls==bad_read) return ~0U;
    if(!write_calls) return initial;
    return read_calls>done_after ? BIT(8) : BIT(0);
}
static void set_xpon_data(u32 reg,u32 val) { assert(reg==0x527c && val==1); write_calls++; }
static void udelay(unsigned int n) { assert(n==1); delays++; }
'''+s+r'''
static void reset(void) { status_calls=status_fail=read_calls=write_calls=delays=done_after=bad_read=0; initial=0; }
int main(void) {
    for(int n=0;n<3000;n+=71) {
        reset(); initial=BIT(0)|BIT(8); done_after=n;
        assert(!q1000k_pipeline_clear_fcs() && write_calls==1 && status_calls==2);
        assert(delays<3000);
    }
    reset(); initial=BIT(0); assert(q1000k_pipeline_clear_fcs()==-EBUSY && !write_calls);
    reset(); status_fail=1; assert(q1000k_pipeline_clear_fcs()==-ENODEV && !read_calls && !write_calls);
    reset(); status_fail=2; assert(q1000k_pipeline_clear_fcs()==-ENODEV && write_calls==1);
    reset(); bad_read=1; assert(q1000k_pipeline_clear_fcs()==-EIO && !write_calls);
    reset(); bad_read=2; assert(q1000k_pipeline_clear_fcs()==-EIO && write_calls==1);
    reset(); done_after=9999; assert(q1000k_pipeline_clear_fcs()==-ETIMEDOUT && delays==3000 && read_calls==3001);
    return 0;
}
''')
