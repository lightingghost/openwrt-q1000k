// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#define pr_info(...) ((void)0)
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
typedef uint32_t u32;
#define TRUE 1
#define FALSE 0
#define PHY_DISABLE 0
#define SCU_WAN_CONF_REG_WAN_SEL_XGSPON 10
#define FIRST_PLUG_IN 1
#define PLUG_OUT 3
/* REGISTERS */
static struct {
    int wan_sel, pma_init_done, first_plugin_flag, trans_tx_status;
    struct { struct { int txPowerEnFlag; } flags; } phyCfg;
} phy, *gpPhyPriv=&phy;
static bool recovering;
static u32 registers[0x8000];
static struct { u32 reg, end, start, value; } trace[17], expected_trace[17];
static int updates, fail_update, fail_readback, bad_readback, pll_reads;
static int delays[2], delay_count;
static int context_error, health_calls, fail_health, fault, phase, fail_phase;
static int los_value, read_error, reads, sleeps, fail_sleep;
static int an7581_pon_phy_update(u32 reg,u32 end,u32 start,u32 value) {
    assert(recovering && phase==2 && !fault && updates<17 && end>=start);
    trace[updates].reg=reg; trace[updates].end=end;
    trace[updates].start=start; trace[updates].value=value;
    if(++updates==fail_update) return -EIO;
    u32 mask=((1U << (end-start+1))-1)<<start;
    registers[(reg&0x1ffff)/4]=(registers[(reg&0x1ffff)/4]&~mask)|(value<<start);
    return 0;
}
#define IO_SPHYA_REG_BITS(reg,end,start,val) an7581_pon_phy_update(reg,end,start,val)


static int q1000k_phy_callback_context(void) { return context_error; }
static int health(void) { return ++health_calls==fail_health ? -ETIMEDOUT : fault; }
static int an7581_pon_phy_status(void) { return health(); }
static int q1000k_phy_controller_check(void) { return health(); }
static int an7581_pon_phy_read(u32 reg,u32 *value) {
    if(recovering) {
        assert(!context_error && phase==2 && !fault);
        if(++pll_reads==fail_readback) return -ETIMEDOUT;
        *value=registers[(reg&0x1ffff)/4];
        if(pll_reads==bad_readback) *value ^= BIT(updates ? trace[updates-1].start : 8);
        return 0;
    }
    assert(!context_error && phase==1 && reg==EN7581_XGPON_PHY_SFP_STA);
    reads++; if(read_error) return read_error;
    *value=los_value<0 ? ~0U : los_value ? EN7581_XGPON_PHY_SFP_RX_LOS_ST : 0;
    return 0;
}
static void phase_step(void) {
    assert(!context_error && !fault && !!phy.pma_init_done==recovering);
    if(++phase==fail_phase) fault=-EIO;
}
static void xpon_init(int mode) { assert(mode==10 && !phase); phase_step(); }
static void fiber_plug_reset(int op,int mode) {
    assert(mode==10);
    if(recovering) assert(op==PLUG_OUT && !phase && !phy.first_plugin_flag);
    else assert((op==FIRST_PLUG_IN && phase==1) || (op==PLUG_OUT && phase==2 && sleeps==1));
    phase_step();
}
static int q1000k_phy_pma_reset(void) {
    assert(recovering && phase==1 && !phy.trans_tx_status && !phy.phyCfg.flags.txPowerEnFlag);
    phase_step(); return fault;
}
static void sleep_step(void) {
    assert(!context_error && !fault && !phy.pma_init_done);
    if(++sleeps==fail_sleep) fault=-EIO;
}
static void usleep_range(unsigned int low,unsigned int high) {
    assert(los_value==1 && low==1000 && high==1500); sleep_step();
}
static void msleep(unsigned int ms) {
    assert(!los_value && ms==350 && phase==2); sleep_step();
}
static void udelay(unsigned int us) {
    assert(recovering && phase==2 && !fault && delay_count<2);
    assert((us==6 && updates==3) || (us==500 && updates==13));
    delays[delay_count++]=us;
}
/* REFERENCE */
/* PRODUCTION */
static void reset(int los)
{
    qpma_gain_saved=false;
    gpPhyPriv=&phy; phy.wan_sel=10; phy.pma_init_done=0; phy.first_plugin_flag=1;
    recovering=false; phy.trans_tx_status=phy.phyCfg.flags.txPowerEnFlag=0;
    context_error=health_calls=fail_health=fault=phase=fail_phase=0;
    read_error=reads=sleeps=fail_sleep=0; los_value=los;
    updates=fail_update=fail_readback=bad_readback=pll_reads=delay_count=0;
    memset(registers,0xa5,sizeof(registers));
    memset(trace,0,sizeof(trace));
}
static void reset_reacquire(void) {
    reset(0); recovering=true; phy.pma_init_done=1; phy.first_plugin_flag=0;
}
static void reacquire_tests(void) {
    reset_reacquire(); assert(!q1000k_phy_rx_reacquire(false, false));
    int checks=health_calls;
    assert(phase==2 && !reads && !sleeps && !updates && !delay_count);
    for(int i=1;i<=checks;i++) {
        reset_reacquire(); fail_health=i;
        assert(q1000k_phy_rx_reacquire(false, false)==-ETIMEDOUT && health_calls==i);
    }
    for(int i=1;i<=2;i++) {
        reset_reacquire(); fail_phase=i;
        assert(q1000k_phy_rx_reacquire(false, false)==-EIO && phase==i);
    }
    reset_reacquire(); context_error=-EPERM;
    assert(q1000k_phy_rx_reacquire(false, false)==-EPERM && !health_calls && !phase);
    reset_reacquire(); phy.first_plugin_flag=1;
    assert(q1000k_phy_rx_reacquire(false, false)==-EAGAIN && !health_calls && !phase);
    reset_reacquire(); phy.pma_init_done=0;
    assert(q1000k_phy_rx_reacquire(false, false)==-EAGAIN && !health_calls && !phase);
    reset_reacquire(); phy.trans_tx_status=1;
    assert(q1000k_phy_rx_reacquire(false, false)==-EACCES && !health_calls && !phase);
    reset_reacquire(); phy.phyCfg.flags.txPowerEnFlag=1;
    assert(q1000k_phy_rx_reacquire(false, false)==-EACCES && !health_calls && !phase);
}
static void pll_tests(void) {
    reset_reacquire(); phase=2; TXPLL_on();
    assert(updates==13 && delay_count==2 && delays[0]==6 && delays[1]==500);
    memcpy(expected_trace,trace,sizeof(trace));
    u32 expected_regs[0x8000]; memcpy(expected_regs,registers,sizeof(registers));
    reset_reacquire(); assert(!q1000k_phy_rx_reacquire(true, false));
    assert(phase==2 && updates==13 && pll_reads==13 && !reads && !sleeps);
    assert(delay_count==2 && delays[0]==6 && delays[1]==500);
    assert(!memcmp(trace,expected_trace,sizeof(trace)));
    assert(!memcmp(registers,expected_regs,sizeof(registers)));
    int checks=health_calls;
    for(int i=1;i<=checks;i++) {
        reset_reacquire(); fail_health=i;
        assert(q1000k_phy_rx_reacquire(true, false)==-ETIMEDOUT && health_calls==i);
    }
    for(int i=1;i<=13;i++) {
        reset_reacquire(); fail_update=i;
        assert(q1000k_phy_rx_reacquire(true, false)==-EIO && updates==i && pll_reads==i-1);
        reset_reacquire(); fail_readback=i;
        assert(q1000k_phy_rx_reacquire(true, false)==-ETIMEDOUT && updates==i && pll_reads==i);
        reset_reacquire(); bad_readback=i;
        assert(q1000k_phy_rx_reacquire(true, false)==-EIO && updates==i && pll_reads==i);
    }
}
static void gain_tests(void) {
    for(int pll=0;pll<2;pll++) {
        reset_reacquire();
        u32 before=registers[(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl&0x1ffff)/4];
        assert(!q1000k_phy_rx_reacquire(pll,true));
        int total=updates, checks=health_calls;
        assert(total==(pll?15:2) && pll_reads==total+1);
        u32 after=registers[(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl&0x1ffff)/4];
        assert(after==((before&~0x103U)|0x101U));
        assert(trace[total-2].reg==0x1fa8b88c && trace[total-2].start==8 && trace[total-2].end==8 && trace[total-2].value==1);
        assert(trace[total-1].reg==0x1fa8b88c && trace[total-1].start==0 && trace[total-1].end==1 && trace[total-1].value==1);
        for(int i=1;i<=total;i++) {
            reset_reacquire(); fail_update=i;
            assert(q1000k_phy_rx_reacquire(pll,true)==-EIO && updates==i && pll_reads==(pll && i<=13 ? i-1 : i));
        }
        for(int i=1;i<=total+1;i++) {
            reset_reacquire(); fail_readback=i;
            assert(q1000k_phy_rx_reacquire(pll,true)==-ETIMEDOUT && pll_reads==i);
            // A corrupt saved original is indistinguishable from a real word;
            // exercise mismatch injection on actual write readbacks only.
            if(i==(pll?14:1)) continue;
            reset_reacquire(); bad_readback=i;
            assert(q1000k_phy_rx_reacquire(pll,true)==-EIO && pll_reads==i);
        }
        for(int i=1;i<=checks;i++) {
            reset_reacquire(); fail_health=i;
            assert(q1000k_phy_rx_reacquire(pll,true)==-ETIMEDOUT && health_calls==i);
        }
        reset_reacquire(); assert(!q1000k_phy_rx_reacquire(pll,true));
        assert(qpma_gain_saved && !q1000k_phy_rx_cleanup() && !qpma_gain_saved);
        assert(registers[(EN7581_XPON_PMA_rg_force_da_pxp_rx_fe_gain_ctrl&0x1ffff)/4]==before);
        int end_updates=updates;
        assert(!q1000k_phy_rx_cleanup() && updates==end_updates);
        for(int i=1;i<=2;i++) {
            reset_reacquire(); assert(!q1000k_phy_rx_reacquire(pll,true));
            fail_update=updates+i;
            assert(q1000k_phy_rx_cleanup()==-EIO && qpma_gain_saved);
            reset_reacquire(); assert(!q1000k_phy_rx_reacquire(pll,true));
            bad_readback=pll_reads+i;
            assert(q1000k_phy_rx_cleanup()==-EIO && qpma_gain_saved);
        }

    }
}
int main(void)
{
    reacquire_tests();
    pll_tests();
    gain_tests();
    for(int los=0;los<2;los++) {
        reset(los); assert(!q1000k_phy_pma_init());
        assert(phy.pma_init_done==!los && phy.first_plugin_flag==los && reads==1);
        int checks=health_calls, phases=phase, delays=sleeps;
        assert(phases==(los?3:2) && delays==(los?2:1));
        for(int i=1;i<=checks;i++) {
            reset(los); fail_health=i;
            assert(q1000k_phy_pma_init()==-ETIMEDOUT && health_calls==i);
            assert(!phy.pma_init_done && phy.first_plugin_flag);
        }
        for(int i=1;i<=phases;i++) {
            reset(los); fail_phase=i;
            assert(q1000k_phy_pma_init()==-EIO && phase==i);
            assert(!phy.pma_init_done && phy.first_plugin_flag);
        }
        for(int i=1;i<=delays;i++) {
            reset(los); fail_sleep=i;
            assert(q1000k_phy_pma_init()==-EIO && sleeps==i);
            assert(!phy.pma_init_done && phy.first_plugin_flag);
        }
    }
    reset(0); context_error=-EWOULDBLOCK;
    assert(q1000k_phy_pma_init()==-EWOULDBLOCK && !health_calls && !phase);
    reset(0); gpPhyPriv=0;
    assert(q1000k_phy_pma_init()==-EINVAL && !health_calls && !phase);
    reset(0); phy.wan_sel=1;
    assert(q1000k_phy_pma_init()==-EINVAL && !health_calls && !phase);
    reset(0); read_error=-EIO;
    assert(q1000k_phy_pma_init()==-EIO && phase==1 && !sleeps && !phy.pma_init_done);
    reset(-1);
    assert(q1000k_phy_pma_init()==-EIO && phase==1 && !sleeps && !phy.pma_init_done);
    return 0;
}
