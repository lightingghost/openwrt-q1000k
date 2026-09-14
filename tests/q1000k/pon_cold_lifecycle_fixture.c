// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
typedef unsigned int uint;
typedef int PHY_Event_Source_t;
typedef int XPON_RESET_MODE_t;
#define Q1000K_PON_IDENTITY
#define GPON_10G_STATE_O1 1
#define GPON_10G_STATE_O2_3 2
#define GPON_10G_STATE_O4 4
#define GPON_10G_STATE_O5 5
#define GPON_10G_STATE_O7 7
#define PON_LINK_STATUS_OFF 0
#define PON_LINK_STATUS_GPON 1
#define PON_PHY_LOS 0
#define PON_PHY_RDY 1
#define PHY_LINK_STATUS_LOS 0
#define PHY_LINK_STATUS_READY 1
#define XMCS_EVENT_TYPE_GPON 1
#define XMCS_EVENT_GPON_STATE_CHANGE 1
#define XMCS_EVENT_GPON_PHY_READY 2
#define XMCS_EVENT_GPON_LOS 3
#define DACT_INDEX 0
#define LOS_INDEX 1
#define LOF_INDEX 2
#define ROGUE_INDEX 3
enum { PHY_EVENT_TRANS_LOS_INT, PHY_EVENT_PHY_ILLG_INT, PHY_EVENT_TRANS_LOS_ILLG_INT,
    PHY_EVENT_PHY_LOF_INT, PHY_EVENT_PHYRDY_INT, PHY_EVENT_START_ROGUE_MODE,
    PHY_EVENT_TF_INT, PHY_EVENT_TX_POWER_OFF, PHY_EVENT_STOP_ROGUE_MODE,
    PHY_EVENT_NO_LOS_NO_READY, PHY_EVENT_TRANS_INT, PHY_EVENT_TRANS_SD_FAIL_INT, PHY_EVENT_I2CM_INT };
typedef struct { int id,src; } PON_PHY_Event_data_t;
static struct { unsigned int state; int to1_timer,hardware_timer,activationCnt; bool emergencyState;
    struct { int to1Timer, hardware_timer; } gponCfg;
} priv,*gpGponPriv=&priv;
static struct { int sysLinkStatus,ponPhyStaus; } sys,*gpPonSysData=&sys;
static struct { int phy_link_status; } phy,*gpPhyData=&phy;
#define GPON_CURR_STATE (priv.state)
static int owned,enter_error,fault,state_error,tx_error,reset_error;
static int state_calls,reset_calls,ready_events,state_events,loss_events,omci_notifications;
static bool tx_enabled,requested_phy_reset,requested_emergency;
static int taskLedConfig,led_schedules,snSendInO23Cnt,max_cnt=3;
static int q1000k_protocol_status(void) { return fault; }
static bool q1000k_protocol_owned(void) { return owned; }
static int q1000k_protocol_enter(void) { if(enter_error) return enter_error; if(owned) return 1; owned=1; return 0; }
static void q1000k_protocol_leave(int token) { assert(owned); if(!token) owned=0; }
static void q1000k_protocol_fail(int error) { assert(error<0); fault=error; }
static int q1000k_mac_activation_set(unsigned char state) {
    assert(owned && (state==2 || state==4 || state==5)); state_calls++; return state_error;
}
static int q1000k_phy_set_tx(bool enable) { assert(owned); if(!tx_error) tx_enabled=enable; return tx_error; }
static int q1000k_omci_reset(bool emergency,bool phy_reset) {
    assert(owned); reset_calls++; requested_phy_reset=phy_reset; requested_emergency=emergency; return reset_error;
}
static void q1000k_omci_state(void) { assert(owned); omci_notifications++; }
static void tasklet_schedule(int *task) { assert(task==&taskLedConfig && owned); led_schedules++; }
static void gpon_set_alarmBit(int i) { assert(owned); }
static void gpon_clear_alarmBit(int i) { assert(owned); }
#define GPON_START_TIMER(t,ms) do { assert(owned); (t)=(ms); } while(0)
static void xmcs_report_event(int type,int event,unsigned int value) {
    assert(owned && type==1);
    if(event==1) { assert(value==priv.state); state_events++; }
    else if(event==2) ready_events++;
    else { assert(event==3); loss_events++; }
}
static int key_error;
static int q1000k_omci_registration_keys(void) { assert(owned); return key_error; }
static int ranging_error;
static int q1000k_omci_ranged(void) { assert(owned); return ranging_error; }
static int q1000k_protocol_timer_delete(int *timer,bool sync,bool shutdown) { assert(owned && sync && !shutdown); *timer=0; return 1; }
/* PRODUCTION */
static void reset(void) {
    memset(&priv,0,sizeof(priv)); memset(&sys,0,sizeof(sys)); memset(&phy,0,sizeof(phy));
    owned=1; priv.state=1; priv.gponCfg.to1Timer=10000; priv.gponCfg.hardware_timer=1000;
    ranging_error=key_error=enter_error=fault=state_error=tx_error=reset_error=0;
    state_calls=reset_calls=ready_events=state_events=loss_events=omci_notifications=led_schedules=0;
    tx_enabled=requested_phy_reset=requested_emergency=false;
}
int main(void) {
    reset(); gpon_phy_ready_handler(7);
    assert(priv.state==2 && ready_events==1 && tx_enabled && !fault && omci_notifications==1);
    gpon_phy_ready_handler(7); assert(state_calls==1 && ready_events==2);
    gpon_act_change_state(4); assert(priv.to1_timer==10000 && priv.state==4);
    gpon_act_change_state(5); assert(priv.hardware_timer==1000 && priv.activationCnt==1);
    assert(!priv.to1_timer);
    gpon_phy_loss_handler(7); assert(reset_calls==1 && !requested_phy_reset && !sys.sysLinkStatus && loss_events==1);
    assert(priv.state==5); /* Reset completion, not this request, publishes O1. */
    reset(); state_error=-ETIMEDOUT; gpon_phy_ready_handler(7);
    assert(fault==-ETIMEDOUT && !ready_events && !state_events && priv.state==1 && !tx_enabled);
    reset(); tx_error=-EIO; gpon_phy_ready_handler(7);
    assert(fault==-EIO && !ready_events && priv.state==1 && !tx_enabled);
    reset(); priv.state=4; key_error=-EKEYREJECTED; gpon_act_change_state(5);
    assert(fault==-EKEYREJECTED && priv.state==4 && !state_calls && !state_events);
    reset(); priv.state=4; ranging_error=-EAGAIN; gpon_act_change_state(5);
    assert(fault==-EAGAIN && priv.state==4 && !state_calls && !state_events);
    reset(); priv.state=4; gpon_act_change_state(2);
    assert(!fault && reset_calls==1 && priv.state==4 && !state_calls && !tx_enabled);
    reset(); priv.state=6; gpon_phy_ready_handler(7);
    assert(fault==-EOPNOTSUPP && !ready_events && !state_calls);
    reset(); gpon_act_change_state(260); assert(fault==-EOPNOTSUPP && !state_calls && priv.state==1);
    reset(); gpon_act_change_state(7); assert(reset_calls==1 && requested_emergency && priv.state==1);
    reset(); reset_error=-EIO; gpon_act_change_state(1); assert(fault==-EIO && !state_events);
    reset(); gpon_phy_loss_handler(7); assert(!reset_calls && !tx_enabled && loss_events==1);
    reset(); priv.state=7; gpon_phy_ready_handler(7); assert(!tx_enabled && ready_events==1);
    reset(); owned=0; gpon_phy_ready_handler(7); assert(fault==-EPERM && !ready_events);
    reset(); owned=0; gpon_phy_loss_handler(7); assert(fault==-EPERM && !loss_events);
    reset(); enter_error=-EWOULDBLOCK; gpon_act_change_state(5); assert(fault==-EWOULDBLOCK && !state_calls);
    reset(); gponDevResetCtrl(0); assert(fault==-EOPNOTSUPP && !reset_calls);
    reset(); gponDevMacReset(1); assert(fault==-EOPNOTSUPP && !reset_calls);
    reset(); gpon_dev_init(); assert(fault==-EOPNOTSUPP && !reset_calls);
    reset(); gpon_enable(); assert(fault==-EOPNOTSUPP && !reset_calls);
    reset(); snSendInO23Cnt=max_cnt; gpon_sw_resync(); assert(reset_calls==1 && requested_phy_reset && !snSendInO23Cnt);
    reset(); PON_PHY_Event_data_t event={.id=PHY_EVENT_PHYRDY_INT,.src=7};
    xpon_phy_event_dispatch(&event); assert(ready_events==1 && priv.state==2);
    event.id=PHY_EVENT_TF_INT; xpon_phy_event_dispatch(&event);
    assert(reset_calls==1 && requested_emergency && priv.emergencyState);
    event.id=PHY_EVENT_STOP_ROGUE_MODE; xpon_phy_event_dispatch(&event);
    assert(reset_calls==2 && !requested_emergency && !priv.emergencyState);
    reset(); event.id=999; xpon_phy_event_dispatch(&event); assert(fault==-EOPNOTSUPP && !ready_events);
    return 0;
}
