// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#define pr_err(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define IS_ENABLED(x) 0
#define AN7581_XPON_MBI_RX_STOP BIT(0)
#define AN7581_XPON_MBI_TX_STOP BIT(8)
#define AN7581_XPON_MPI_RX_STOP BIT(16)
#define AN7581_XPON_MPI_TX_STOP BIT(24)
#define DEFINE_MUTEX(x) int x
static int held, atomic_context, calls, fail, poison, containment_calls, contain_fail;
static void mutex_lock(int *m) { (void)m; assert(!held); held=1; }
static void mutex_unlock(int *m) { (void)m; assert(held); held=0; }
static int in_atomic(void) { return atomic_context; }
static int in_interrupt(void) { return 0; }
static int irqs_disabled(void) { return 0; }
static int rcu_preempt_depth(void) { return 0; }
static int rcu_read_lock_held(void) { return 1; }
static int q1000k_transport_pause(unsigned int ms);
static int q1000k_transport_set_tx_channel(unsigned int channel,bool enable);
static int q1000k_transport_retire_fe(unsigned int channel);
static int an7581_xpon_mac_stop(u32 mask,bool hold);
static int an7581_xpon_mac_wait_tx_empty(void);
static int q1000k_transport_drain_rx(void);
static int q1000k_phy_quiesce(void);
static void an7581_xpon_invalidate(void);
/* PRODUCTION */
static int step(void) { assert(held && !atomic_context); return ++calls==fail ? -ETIMEDOUT : 0; }
static int containment(void) { assert(held && q1000k_pipeline.error==-ETIMEDOUT); return ++containment_calls==contain_fail ? -ENODEV : 0; }
static int q1000k_transport_pause(unsigned int ms) { assert(ms==1000 && !calls); return step(); }
static int q1000k_transport_retire_fe(unsigned int channel)
{
    assert(channel<32 && calls==2+(int)channel);
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_INGRESS_STOPPED);
    return step();
}
static int q1000k_transport_set_tx_channel(unsigned int channel,bool enable)
{
    assert(!enable && containment_calls==(int)channel); return containment();
}
static int an7581_xpon_mac_stop(u32 mask,bool hold)
{
    assert(hold);
    if(q1000k_pipeline.error) { assert(mask==Q1000K_MAC_ALL_STOPS); return containment(); }
    switch(calls) {
    case 1: assert(mask==AN7581_XPON_MPI_RX_STOP); break;
    case 34: assert(mask==AN7581_XPON_MBI_TX_STOP); break;
    case 36: assert(mask==(AN7581_XPON_MPI_TX_STOP|AN7581_XPON_MBI_RX_STOP)); break;
    default: assert(0);
    }
    return step();
}
static int an7581_xpon_mac_wait_tx_empty(void) { assert(calls==35); return step(); }
static int q1000k_transport_drain_rx(void)
{
    assert(calls==37 && q1000k_pipeline.stage==Q1000K_PIPELINE_MAC_STOPPED);
    return step();
}
static int q1000k_phy_quiesce(void)
{
    if(q1000k_pipeline.error) { assert(containment_calls==33); return containment(); }
    assert(calls==38 && q1000k_pipeline.stage==Q1000K_PIPELINE_RX_DRAINED);
    return step();
}
static void an7581_xpon_invalidate(void) { assert(containment_calls==34); poison++; }
static void reset(void)
{
    memset(&q1000k_pipeline,0,sizeof(q1000k_pipeline));
    calls=poison=containment_calls=0;
}
int main(void)
{
    struct q1000k_pipeline_status status;
    atomic_context=1; assert(q1000k_pipeline_shutdown()==-EWOULDBLOCK && !calls);
    atomic_context=0; assert(!q1000k_pipeline_shutdown() && calls==39);
    q1000k_pipeline_status(&status);
    assert(status.stage==Q1000K_PIPELINE_PHY_STOPPED && status.retired==~0U);
    assert(!status.error && !status.containment_error && !containment_calls && !poison);
    assert(!q1000k_pipeline_shutdown() && calls==39);
    for(fail=1;fail<=39;fail++) {
        for(contain_fail=0;contain_fail<=34;contain_fail++) {
            reset(); assert(q1000k_pipeline_shutdown()==-ETIMEDOUT && calls==fail);
            assert(poison==1 && containment_calls==34);
            q1000k_pipeline_status(&status);
            assert(status.stage<Q1000K_PIPELINE_PHY_STOPPED && status.error==-ETIMEDOUT);
            assert(status.containment_error==(contain_fail ? -ENODEV : 0));
            unsigned int retired=fail<=2 ? 0 : fail<=34 ? fail-3 : 32;
            assert(status.retired==(retired==32 ? ~0U : BIT(retired)-1));
            assert(q1000k_pipeline_shutdown()==-ETIMEDOUT && calls==fail && poison==1);
        }
    }
    return 0;
}
