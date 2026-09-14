// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define TRUE 1
#define FALSE 0
#define SCU_WAN_CONF_REG_WAN_SEL_XGSPON 10
#define FIRST_PLUG_IN 1
#define PLUG_OUT 3
/* REGISTERS */
static struct { int wan_sel, pma_init_done, first_plugin_flag; } phy, *gpPhyPriv=&phy;
static int context_error, health_calls, fail_health, fault, phase, fail_phase;
static int los_value, read_error, reads, sleeps, fail_sleep;
static int q1000k_phy_callback_context(void) { return context_error; }
static int health(void) { return ++health_calls==fail_health ? -ETIMEDOUT : fault; }
static int an7581_pon_phy_status(void) { return health(); }
static int q1000k_phy_controller_check(void) { return health(); }
static int an7581_pon_phy_read(u32 reg,u32 *value) {
    assert(!context_error && phase==1 && reg==EN7581_XGPON_PHY_SFP_STA);
    reads++; if(read_error) return read_error;
    *value=los_value<0 ? ~0U : los_value ? EN7581_XGPON_PHY_SFP_RX_LOS_ST : 0;
    return 0;
}
static void phase_step(void) {
    assert(!context_error && !fault && !phy.pma_init_done);
    if(++phase==fail_phase) fault=-EIO;
}
static void xpon_init(int mode) { assert(mode==10 && !phase); phase_step(); }
static void fiber_plug_reset(int op,int mode) {
    assert(mode==10 && ((op==FIRST_PLUG_IN && phase==1) || (op==PLUG_OUT && phase==2 && sleeps==1)));
    phase_step();
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
/* PRODUCTION */
static void reset(int los)
{
    gpPhyPriv=&phy; phy.wan_sel=10; phy.pma_init_done=0; phy.first_plugin_flag=1;
    context_error=health_calls=fail_health=fault=phase=fail_phase=0;
    read_error=reads=sleeps=fail_sleep=0; los_value=los;
}
int main(void)
{
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
