// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#define pr_err(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint16_t u16;
typedef uint32_t u32;
struct task_struct { int unused; };
static struct task_struct task1, task2;
static struct task_struct *current=&task1;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
struct airoha_pon_port_config { u16 min_len,max_len; bool byte_mode,scale16; };
static bool reset_requested;
static u32 enabled_channels;
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
static int q1000k_phy_prepare_wan(void);
static int wan_prepare_fail;
#define PHY_XGSPON_CONFIG 10
static int q1000k_phy_needs_configure(void) { return 0; }
static int q1000k_phy_configure(u32 mode) { (void)mode; assert(0); return -EINVAL; }
static int an7581_xpon_mac_request_rx_stop(void) { assert(0); return -EINVAL; }
static void an7581_xpon_invalidate(void);
static int an7581_xpon_reset(void);
static int q1000k_transport_get_port_config(struct airoha_pon_port_config *config);
static int q1000k_transport_configure_port(const struct airoha_pon_port_config *old,
    const struct airoha_pon_port_config *config);
static int q1000k_transport_reset_epoch(void);
static int q1000k_pipeline_clear_fcs(void);
static int q1000k_transport_activate_rx(u32 channels);
static int q1000k_transport_resume(void);
static int q1000k_phy_start(void);
static int q1000k_phy_receiver_startup(void);
static bool transmitter;
static int q1000k_phy_set_tx(bool enable);
/* PRODUCTION */
static int step(void) { assert(held && !atomic_context); return ++calls==fail ? -ETIMEDOUT : 0; }
static int containment(void) { assert(held && q1000k_pipeline.error<0); return ++containment_calls==contain_fail ? -ENODEV : 0; }
static int q1000k_phy_prepare_wan(void)
{
    assert(held && calls==1 && q1000k_pipeline.stage==Q1000K_PIPELINE_CPU_PAUSED);
    return wan_prepare_fail ? -ETIMEDOUT : 0;
}
static int q1000k_transport_pause(unsigned int ms) { assert(ms==1000 && !calls); return step(); }
static int q1000k_transport_retire_fe(unsigned int channel)
{
    assert(channel<32 && calls==2+(int)channel);
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_INGRESS_STOPPED);
    return step();
}
static int q1000k_transport_set_tx_channel(unsigned int channel,bool enable)
{
    if(enable) {
        assert(q1000k_pipeline.stage==Q1000K_PIPELINE_EPOCH_READY);
        assert(channel==0 || channel==7 || channel==31);
        assert(!q1000k_table_owner);
        int ret=step(); if(!ret) enabled_channels|=BIT(channel); return ret;
    }
    assert(containment_calls==(int)channel); return containment();
}
static int an7581_xpon_mac_stop(u32 mask,bool hold)
{
    if(q1000k_pipeline.error) { assert(mask==Q1000K_MAC_ALL_STOPS); return containment(); }
    if(calls>=39) {
        assert(mask==Q1000K_MAC_ALL_STOPS);
        if(hold) assert(reset_requested && q1000k_pipeline.stage==Q1000K_PIPELINE_PHY_STOPPED);
        else assert(q1000k_pipeline.stage==Q1000K_PIPELINE_PHY_ACTIVE);
        return step();
    }
    assert(hold);
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
    transmitter=false;
    if(q1000k_pipeline.error) { assert(containment_calls==33); return containment(); }
    assert(calls==38 && q1000k_pipeline.stage==Q1000K_PIPELINE_RX_DRAINED);
    return step();
}
static void an7581_xpon_invalidate(void) { assert(containment_calls==34); poison++; }
static void reset(void)
{
    memset(&q1000k_pipeline,0,sizeof(q1000k_pipeline));
    calls=poison=containment_calls=0; enabled_channels=0;
}
static int an7581_xpon_reset(void)
{
    assert(reset_requested && q1000k_pipeline.stage==Q1000K_PIPELINE_PHY_STOPPED);
    return step();
}
static int q1000k_transport_get_port_config(struct airoha_pon_port_config *config)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_PHY_STOPPED);
    int ret=step(); if(!ret) *config=(struct airoha_pon_port_config){60,16128,true,true}; return ret;
}
static int q1000k_transport_configure_port(const struct airoha_pon_port_config *old,
    const struct airoha_pon_port_config *config)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_EPOCH_READY);
    assert(old->min_len==60 && old->max_len==16128);
    assert(config->min_len==60 && config->max_len==2000 && config->byte_mode && config->scale16);
    return step();
}
static int q1000k_pipeline_clear_fcs(void)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_TABLES_CLEARED); return step();
}
static int q1000k_transport_reset_epoch(void)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_FCS_CLEARED && !q1000k_table_owner);
    return step();
}
static int q1000k_transport_activate_rx(u32 channels)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_PREPARED && channels==0x80000081 && enabled_channels==channels);
    return step();
}
static int q1000k_phy_start(void)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_RX_ACTIVE); return step();
}
static int q1000k_phy_receiver_startup(void)
{ assert(q1000k_pipeline.stage==Q1000K_PIPELINE_MAC_ACTIVE && !transmitter); return step(); }
static int q1000k_phy_set_tx(bool enable)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_MAC_ACTIVE); transmitter=enable; return step();
}
static int q1000k_transport_resume(void)
{
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_MAC_ACTIVE); return step();
}
static int clear_tables(void *arg)
{
    assert(arg==&task1 && !q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR));
    atomic_context=1;
    assert(q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR)==-EWOULDBLOCK);
    atomic_context=0;
    assert(q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL)==-EPERM);
    current=&task2;
    assert(q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR)==-EPERM);
    current=&task1;
    return step();
}
static int install_tables(void *arg)
{
    assert(arg==&task1 && !q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL));
    assert(q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR)==-EPERM);
    return step();
}
static int readiness_error,readiness_calls;
static int check_ready(void *arg)
{
    assert(arg==&task1 && !q1000k_pipeline_table_context(Q1000K_TABLE_ACTIVATE));
    assert(q1000k_pipeline.stage==Q1000K_PIPELINE_MAC_ACTIVE && !transmitter);
    readiness_calls++; return readiness_error;
}
int main(void)
{
    struct q1000k_pipeline_ops ops={.clear=clear_tables,.install=install_tables};
    assert(q1000k_pipeline_table_context(Q1000K_TABLE_CLEAR)==-EPERM);
    assert(q1000k_pipeline_reconfigure(NULL,NULL,1)==-EINVAL && !calls);
    assert(q1000k_pipeline_reconfigure(&ops,&task1,2)==-EINVAL && !calls);
    assert(q1000k_pipeline_activate()==-EINVAL && !calls);
    atomic_context=1;
    assert(q1000k_pipeline_reconfigure(&ops,&task1,1)==-EWOULDBLOCK && !calls);
    assert(q1000k_pipeline_activate()==-EWOULDBLOCK && !calls);
    atomic_context=0;
    for(unsigned int cold=0;cold<2;cold++) {
        ops.reset_mac=reset_requested=cold;
        for(int failure=0;failure<=54+(cold?2:0);failure++) {
            reset(); fail=failure; contain_fail=failure%35;
            int ret=q1000k_pipeline_reconfigure(&ops,&task1,0x80000081);
            if(!ret) {
                assert(q1000k_pipeline.stage==Q1000K_PIPELINE_PREPARED && !q1000k_pipeline.retired);
                assert(q1000k_pipeline.channels==0x80000081 && !q1000k_table_owner);
                ret=q1000k_pipeline_activate();
            }
            assert(!q1000k_table_owner && q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL)==-EPERM);
            if(failure) {
                assert(ret==-ETIMEDOUT && calls==failure && poison==1 && containment_calls==34);
                assert(q1000k_pipeline.error==-ETIMEDOUT);
                assert(q1000k_pipeline.containment_error==(contain_fail?-ENODEV:0));
                assert(q1000k_pipeline_activate()==-ETIMEDOUT && calls==failure);
            } else {
                assert(!ret && calls==54+(cold?2:0) && !poison && !containment_calls);
                assert(q1000k_pipeline.stage==Q1000K_PIPELINE_UNDRAINED);
            }
        }
    }
    reset(); reset_requested=ops.reset_mac=false;
    assert(!q1000k_pipeline_reconfigure(&ops,&task1,0x80000081));
    transmitter=false;
    assert(!q1000k_pipeline_activate_receive_only() && !transmitter);
    for(int failure=0;failure<2;failure++) {
        reset(); readiness_error=failure ? -EIO : 0;
        assert(!q1000k_pipeline_reconfigure(&ops,&task1,0x80000081));
        assert(q1000k_pipeline_activate_checked(true,check_ready,&task1)==readiness_error);
        assert(!q1000k_table_owner && transmitter==!failure);
        if(failure) assert(q1000k_pipeline.error==-EIO && poison && containment_calls==34);
    }
    assert(readiness_calls==2);
    return 0;
}
