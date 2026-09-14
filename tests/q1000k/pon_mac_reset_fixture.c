// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#define EXPORT_SYMBOL(x)
#define IS_ENABLED(x) 0
static int held, lifecycle, atomic_context, calls, fail, wrong;
static int xpon_lock, xpon_lifecycle;
struct reset_control { int asserted; };
struct an7581_xpon { bool mac_fault, resetting; struct reset_control *reset; };
static struct an7581_xpon *xpon;
static void mutex_lock(int *m) { (void)m; assert(!held && !lifecycle); lifecycle=1; }
static void mutex_unlock(int *m) { (void)m; assert(!held && lifecycle); lifecycle=0; }
#define write_lock_irqsave(l,f) do { (void)(l); assert(!held); held=1;(f)=0; } while(0)
#define write_unlock_irqrestore(l,f) do { (void)(l);(void)(f); assert(held);held=0; } while(0)
static int in_atomic(void) { return atomic_context; }
static int in_interrupt(void) { return 0; }
static int irqs_disabled(void) { return 0; }
static int rcu_preempt_depth(void) { return 0; }
static int rcu_read_lock_held(void) { return 1; }
static void udelay(unsigned int n) { assert(!held && lifecycle && n==1); }
static int reset_control_assert(struct reset_control *r)
{
    assert(!held && lifecycle && xpon->resetting); calls++;
    if(calls==fail) return -ETIMEDOUT;
    r->asserted=1; return 0;
}
static int reset_control_deassert(struct reset_control *r)
{
    assert(!held && lifecycle && xpon->resetting); calls++;
    if(calls==fail) return -ETIMEDOUT;
    r->asserted=0; return 0;
}
static int reset_control_status(struct reset_control *r)
{
    assert(!held && lifecycle && xpon->resetting); calls++;
    if(calls==fail) return -ETIMEDOUT;
    return calls==wrong ? !r->asserted : r->asserted;
}
/* PRODUCTION */
int main(void)
{
    struct reset_control reset={0};
    struct an7581_xpon provider={.reset=&reset};
    assert(an7581_xpon_reset()==-ENODEV && !calls);
    xpon=&provider; atomic_context=1;
    assert(an7581_xpon_reset()==-EWOULDBLOCK && !calls);
    atomic_context=0;
    assert(!an7581_xpon_reset() && calls==4 && !reset.asserted);
    assert(!provider.resetting && !provider.mac_fault && !held && !lifecycle);
    for(fail=1; fail<=4; fail++) {
        calls=0; provider.mac_fault=false;
        assert(an7581_xpon_reset()==-ETIMEDOUT);
        assert(provider.mac_fault && reset.asserted && !provider.resetting);
        int saved=calls;
        assert(an7581_xpon_reset()==-EIO && saved==calls);
    }
    fail=0;
    for(wrong=2; wrong<=4; wrong+=2) {
        calls=0; provider.mac_fault=false;
        assert(an7581_xpon_reset()==-EIO);
        assert(provider.mac_fault && reset.asserted && !provider.resetting);
    }
    return 0;
}
