// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#define BIT(n) (UINT32_C(1)<<(n))
#define U64_MAX UINT64_MAX
#define OMCI_OLT_VENDOR_ID_LEN 4
#define OMCI_OLT_VERSION_LEN 14
#define OMCI_OLT_EQUIPMENT_ID_LEN 20
#define OMCI_CONFIG_SOURCE_DRIVER 1
#define OMCI_IDENTITY_F_SERIAL_NUMBER 1
#define OMCI_IDENTITY_F_VENDOR_ID 2
#define OMCI_IDENTITY_F_EQUIPMENT_ID 16
#define OMCI_IDENTITY_F_VERSION 8
#define OMCI_F_MIC_VALID 2
#define OMCI_CAP_PROVIDER_MIC 8
#define XPON_MODE_XGSPON 4
#define XPON_MODE_CAP(n) (1U<<(n))
#define XPON_REGISTRATION_DISCOVERY 1
#define XPON_REGISTRATION_REGISTERING 2
#define XPON_REGISTRATION_OPERATIONAL 3
#define GPON_10G_STATE_O1 1
#define GPON_10G_STATE_O2_3 2
#define GPON_10G_STATE_O4 4
#define GPON_10G_STATE_O5 5
#define GPON_10G_STATE_O7 7
#define GPON_UNASSIGN_ONU_ID 1023
#define XGPON_PLOAM_ACK_OK 0
#define XGPON_SW 0
#define CHECKSUM_NONE 0
#define PHY_XGSPON_CONFIG 7
#define DS_FEC_SETTING_FORCE_ON 1
#define OMCI_TELEMETRY_F_FEC_UPSTREAM 2
#define OMCI_FEC_STATUS_DOWN 1
#define OMCI_FEC_STATUS_UP 2
#define XPON_PHY_API_TYPE_GET 0
#define PON_GET_PHY_TX_FEC_STATUS 99
#define PON_GET_PHY_LOS_STATUS 100
struct xpon_phy_api_data_s { int api_type, cmd_id; };
#define TRAFFIC_DOWN 0
#define UPAES_MODE_NONE 0
#define GPON_SMA_INVALID 0
#define GPON_REG_ID_NOT_REPORT 0
#define GPON_REG_ID_REPORTED 1
#define KEY_STATE_KN0 0
#define XMCS_EVENT_TYPE_GPON 1
#define XMCS_EVENT_GPON_STATE_CHANGE 2
#define XPON_START_TIMER(timer,ms) do { assert(owned && (ms)==1000); (timer)=ms; } while(0)
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define __rcu
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define WARN_ON_ONCE(x) ({ bool value_=(x); assert(!value_); value_; })
#define rcu_dereference(p) (p)
#define rcu_access_pointer(p) (p)
#define rcu_assign_pointer(p,v) ((p)=(v))
#define RCU_INIT_POINTER(p,v) ((p)=(v))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct omci_device;
struct omci_priority_queue_config;
struct omci_traffic_scheduler_config;
struct omci_ani_topology;
struct omci_service_config;
struct omci_telemetry;
struct omci_olt_profile_state;
struct sk_buff { unsigned int len; u8 data[128]; int ip_summed; };
struct net_device { int unused; };
struct device { int unused; };
struct crypto_lskcipher { int unused; };
struct xpon_device { int unused; };
struct xpon_device_desc { struct net_device *netdev; unsigned int mode,modes; };
#define OMCI_ONU_TYPE_SFU 1
/* TYPES */
struct omci_device { const struct omci_device_ops *ops; void *priv; u16 onu,gem; u64 epoch,last; bool channel; u8 state; };
typedef struct { u8 msk[16],sk[16],ploamIk[2][16],omciIk[2][16],kek[2][16];
    u8 omciIkIdx,ploamIkIdx,kekIdx,aesUcKey[2][16],aesBcKey[2][16],aesUcKeyIdx;
    u8 smaValid,registerIDState,txKeyValid,state;
} GPON_Security_t;
static struct { struct { u16 onu_id,omcc; u8 ponTag[8]; int ploamCtrl,usOmciMicCtrl,dsOmciMicCtrl; u32 eqd,eqd_olt_absolute,eqd_olt_init; } gponCfg;
    GPON_Security_t gponSecurity;
    struct { unsigned int txPloamMsgCnt; } ploamMsgcounter;
    u8 state, prePloamMsg[52]; int swreplyploam_task, gpon_traffic_status,gemUpAESMode; bool typeBOnGoing,emergencyState;
} vendor,*gpGponPriv=&vendor;
static struct { int traffic_status_refresh_timer; } phy,*gpPhyData=&phy;
#define GPON_CURR_STATE (vendor.state)
static int owned, auth_held, fault, calls, fail_at, native_count, core_count, rejected_count;
static int reconciles, reconcile_error;
static int ack_count, resets, service_resets, live_skb, native_error, derivations;
static int request_during_barrier, assign_count, refresh_count;
static bool services_enabled, install_phase, optical_tx;
static int cold_count, data_installs, data_reports, random_calls;
static u8 data_tx, data_rx, data_keys[2][16], last_report[44];
static bool random_ready=true;
static bool rng_is_initialized(void) { return random_ready; }
static void get_random_bytes(void *p,size_t n) { assert(!owned && n==16 && random_ready); memset(p,++random_calls,n); }

static u8 hw_ploam_index, hw_omci_index, installed_profiles, profile_version[4];
int snSendInO23Cnt;
static u16 hardware_onu;
static u64 native_epoch,native_last;
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static void *kzalloc(size_t n,int flags) { return calloc(1,n); }
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; void (*fn)(struct work_struct *); };
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define INIT_DELAYED_WORK(w,f) do { (w)->fn=f; } while(0)
#define msecs_to_jiffies(n) (n)
static void schedule_delayed_work(struct delayed_work *w,unsigned int n) { assert(n==0||n==1000); w->pending=true; }
static void cancel_delayed_work_sync(struct delayed_work *w) { w->pending=false; }
static void kfree_sensitive(void *p) { free(p); }
static void synchronize_rcu(void) {}
typedef int spinlock_t;
static void spin_lock_init(spinlock_t *lock) { *lock=0; }
static void spin_lock_bh(spinlock_t *lock) { assert(!*lock && !auth_held); *lock=auth_held=1; }
static void spin_unlock_bh(spinlock_t *lock) { assert(*lock && auth_held); *lock=auth_held=0; }
static int step(void) { return ++calls==fail_at ? -ETIMEDOUT : 0; }
static int q1000k_protocol_status(void) { return fault; }
static bool q1000k_protocol_owned(void) { return owned; }
static int q1000k_protocol_enter(void) { if(fault) return fault; if(owned) return 1; owned=1; return 0; }
static void q1000k_protocol_leave(int token) { assert(owned); if(!token) owned=0; }
static void q1000k_protocol_fail(int error) { assert(error<0); if(!fault) fault=error; }
static void q1000k_protocol_task_schedule(int *task) { assert(owned); }
static int q1000k_protocol_control(void) { assert(owned); return fault; }
void q1000k_omci_state(void);
int q1000k_omci_assign(u16 onu);
int q1000k_omci_ranged(void);
int q1000k_omci_reset(bool emergency, bool phy);
static void gpon_act_change_state(u8 state) { assert(owned && (state!=5 || !q1000k_omci_ranged())); vendor.state=state; q1000k_omci_state(); }
static void ploam_send_acknowledge_msg(u8 seq,int result) { assert(owned && !result); ack_count++; }
static void q1000k_services_enable(bool enabled) { services_enabled=enabled; }
static void q1000k_services_init(void) { services_enabled=false; }
static void q1000k_services_reset(void) { assert(owned); service_resets++; }
static void q1000k_services_destroy(void) { services_enabled=false; }
static int q1000k_services_topology(struct omci_device *o,struct omci_ani_topology *t) { return 0; }
static int q1000k_services_tcont(struct omci_device *o,u16 e,u16 a,bool v) { return 0; }
static int q1000k_services_gem_config(struct omci_device *o,u16 e,const struct omci_gem_port_config *cfg,bool valid) { return 0; }
static u8 query_ring;
static int q1000k_services_gem_key_ring(u16 entity,u8 *ring) { assert(owned); if(entity!=99) return -ENOENT; *ring=query_ring; return 0; }
static int q1000k_services_queue(struct omci_device *o,u16 e,const struct omci_priority_queue_config *q) { return 0; }
static int q1000k_services_scheduler(struct omci_device *o,u16 e,const struct omci_traffic_scheduler_config *s) { return 0; }
static int q1000k_services_uni(struct omci_device *o,u16 e,bool enable) { return 0; }
static int q1000k_services_replace(struct omci_device *o,const struct omci_service_config *s,size_t n) { return 0; }
static int q1000k_pon_get_serial(u8 *s,int n) { assert(n==8); memset(s,1,n); return 0; }
static int q1000k_pon_get_registration(u8 *s,int n) { assert(n==36); memset(s,2,n); return 0; }
static struct omci_identity identity_override, identity_seen;
static bool identity_was_set;
static int q1000k_pon_get_omci_overrides(struct omci_identity *id) {
    if(identity_override.valid&OMCI_IDENTITY_F_VERSION) memcpy(id->version,identity_override.version,sizeof(id->version));
    if(identity_override.valid&OMCI_IDENTITY_F_EQUIPMENT_ID) memcpy(id->equipment_id,identity_override.equipment_id,sizeof(id->equipment_id));
    id->valid|=identity_override.valid; return 0;
}
static struct crypto_lskcipher *crypto_alloc_lskcipher(const char *alg,int a,int b) { return calloc(1,sizeof(struct crypto_lskcipher)); }
static void crypto_free_lskcipher(struct crypto_lskcipher *c) { free(c); }
static struct device *get_xpon_dev(void) { static struct device dev; return &dev; }
static struct xpon_device *xpon_device_register(struct device *d,const struct xpon_device_desc *desc) { return calloc(1,sizeof(struct xpon_device)); }
static void xpon_device_unregister(struct xpon_device *x) { free(x); }
static void xpon_device_report_registration(struct xpon_device *x,int state) { assert(!owned); }
static int reported_los=-1;
static void xpon_device_report_optical(struct xpon_device *x,bool signal,bool los) { assert(!owned && signal!=los); reported_los=los; }
static void xpon_device_report_carrier(struct xpon_device *x,bool up) { assert(!owned); }
static struct omci_device *omci_device_register(struct xpon_device *x,u32 caps,const struct omci_device_ops *ops,void *priv) {
    struct omci_device *o=calloc(1,sizeof(*o)); o->ops=ops; o->priv=priv; identity_was_set=false; return o;
}
static void *omci_device_priv(struct omci_device *o) { return o->priv; }
static void omci_device_set_identity_info(struct omci_device *o,const struct omci_identity *id) {
    assert(id->serial_source==OMCI_CONFIG_SOURCE_DRIVER && id->vendor_source==OMCI_CONFIG_SOURCE_DRIVER);
    identity_seen=*id; identity_was_set=true;
}
static int omci_device_start(struct omci_device *o) {
    assert(identity_was_set);
    if(identity_override.valid&OMCI_IDENTITY_F_VERSION) assert(!memcmp(identity_seen.version,identity_override.version,sizeof(identity_seen.version)));
    if(identity_override.valid&OMCI_IDENTITY_F_EQUIPMENT_ID) assert(!memcmp(identity_seen.equipment_id,identity_override.equipment_id,sizeof(identity_seen.equipment_id)));
    return o->ops->start(o);
}
static void omci_device_unregister(struct omci_device *o) { o->ops->stop(o); free(o); }
static int omci_device_set_auth_epoch(struct omci_device *o,u64 epoch) {
    assert(!owned && !auth_held);
    if(!epoch && request_during_barrier) {
        int mode=request_during_barrier; request_during_barrier=0; int token=q1000k_protocol_enter();
        assert(token>=0); if(mode==1) assert(!q1000k_omci_assign(19));
        else assert(!q1000k_omci_reset(true,false));
        q1000k_protocol_leave(token);
    }
    if(epoch && (!o->channel || epoch<=o->last)) return -ESTALE;
    o->epoch=epoch; if(epoch) o->last=epoch; return 0;
}
static int q1000k_transport_set_auth_epoch(u64 epoch) {
    assert(!owned && !auth_held); if(epoch && epoch<=native_last) return -ESTALE;
    native_epoch=epoch; if(epoch) native_last=epoch; return 0;
}
static int omci_device_reset_registration(struct omci_device *o) {
    assert(!owned && !auth_held); o->onu=o->gem=0xffff; o->channel=false; o->epoch=0; resets++; return 0;
}
static void omci_device_set_onu_id(struct omci_device *o,u16 onu) { assert(!owned); o->onu=onu; }
static void omci_device_set_channel(struct omci_device *o,u16 gem,bool up) { assert(!owned); o->gem=gem; o->channel=up; }
static int omci_device_reconcile_services(struct omci_device *o) { assert(!owned); reconciles++; return reconcile_error; }
static void omci_device_set_state(struct omci_device *o,u8 state) { assert(!owned); o->state=state; }
static struct sk_buff *packet(unsigned int n) { struct sk_buff *p=calloc(1,sizeof(*p)); assert(p); p->len=n; live_skb++; return p; }
static void dev_kfree_skb_any(struct sk_buff *skb) { assert(live_skb>0); live_skb--; free(skb); }
static struct sk_buff *skb_copy_expand(struct sk_buff *skb,int h,int tail,int flags) { struct sk_buff *p=packet(skb->len); *p=*skb; return p; }
static u8 *skb_put(struct sk_buff *skb,unsigned int n) { u8 *p=skb->data+skb->len; skb->len+=n; assert(skb->len<=128); return p; }
static void omci_device_receive(struct omci_device *o,struct sk_buff *skb,u16 gem,u32 flags,u64 epoch) {
    assert(flags==OMCI_F_MIC_VALID);
    if(o->epoch && epoch==o->epoch && o->channel && gem==o->gem) core_count++; else rejected_count++;
    dev_kfree_skb_any(skb);
}
static int q1000k_transport_xmit_omci(struct sk_buff *skb,u16 gem,u8 index,u64 epoch) {
    assert(!auth_held); if(native_error) return native_error;
    if(!native_epoch || epoch!=native_epoch) return -ESTALE;
    assert(skb->len==48 && skb->data[44]==(u8)(0x30+index)); native_count++; dev_kfree_skb_any(skb); return 0;
}
static int q1000k_transport_set_queue_close(u8 ch,u8 closed) { assert(owned && ch==0 && closed==0xfe); return step(); }
static int q1000k_gwan_register(u16 onu,int (*install)(void *),void *arg) {
    assert(owned); assign_count++; int ret=step(); if(ret) return ret;
    install_phase=true; ret=install(arg); install_phase=false; return ret;
}
static int q1000k_gwan_refresh(int (*install)(void *),void *arg) {
    assert(owned); refresh_count++; int ret=step(); if(ret) return ret;
    install_phase=true; ret=install(arg); install_phase=false; return ret;
}
static int q1000k_gwan_refresh_checked(int (*install)(void *),int (*ready)(void *),void *arg) {
    int ret=q1000k_gwan_refresh(install,arg); return ret ? ret : ready(arg);
}
static void xmcs_report_event(int type,int event,u8 state) { assert(owned && type==1 && event==2 && (state==1||state==7)); }
static int q1000k_phy_set_tx(bool enable) { assert(owned); int ret=step(); if(!ret) optical_tx=enable; return ret; }
static int phy_power_error=-ENODATA, phy_power_calls;
static int q1000k_phy_get_rx_power(u32 *value) {
    assert(owned); phy_power_calls++;
    if(phy_power_error) return phy_power_error;
    *value=19900; return 0;
}
static int phy_fec_value, phy_fec_calls, phy_los_value=1;
int q1000k_phy_call(struct xpon_phy_api_data_s *q) {
    if(q->cmd_id==PON_GET_PHY_LOS_STATUS) { assert(!owned); return phy_los_value; }
    assert(owned && q->api_type==XPON_PHY_API_TYPE_GET && q->cmd_id==PON_GET_PHY_TX_FEC_STATUS);
    phy_fec_calls++; return phy_fec_value;
}
static int q1000k_phy_configure(u32 mode) { assert(owned && install_phase && mode==PHY_XGSPON_CONFIG); return step(); }
static int XPON_PHY_SET_RX_ENABLE(void) { assert(owned && install_phase); return step(); }
static int XPON_PHY_SET_RX_FEC(int mode) { assert(owned && install_phase && mode==1); return step(); }
static int q1000k_mac_cold_install(const u8 sn[8],const u8 reg[36],bool emergency) {
    assert(owned && install_phase && !native_epoch && sn[7]==1 && reg[35]==2); return step();
}
static int q1000k_mac_cold_select_keys(void) { assert(owned && install_phase); return step(); }
static void gpon_INT_init(void) { assert(owned && install_phase); int ret=step(); if(ret) q1000k_protocol_fail(ret); }
static int q1000k_gwan_cold_reset(int (*install)(void *),void *arg) {
    assert(owned && !native_epoch); cold_count++; int ret=step(); if(ret) return ret;
    install_phase=true; ret=install(arg); install_phase=false; optical_tx=false; return ret;
}
/* PRODUCTION */
int q1000k_auth_key_report(struct crypto_lskcipher *tfm,const u8 kek[16],const u8 key[16],bool confirm,u8 report[32]) {
    assert(!owned && !auth_held && tfm==(confirm ? qomci_current->cipher : qomci_current->ecb_cipher));
    int ret=step(); if(ret) return ret;
    memset(report,0,32); memcpy(report,key,16); report[0]^=confirm ? 0xab : 0xef; return 0;
}
int q1000k_mac_data_keys_install(const struct q1000k_mac_data_keys *keys,bool *pending) {
    assert(owned && install_phase && !native_epoch && !qomci_current->active);
    int ret=step(); if(ret) return ret;
    data_installs++; data_tx=keys->tx_index; data_rx=keys->rx_valid;
    memcpy(data_keys,keys->key,32); *pending=data_tx!=0; return 0;
}
int q1000k_mac_data_keys_ready(bool pending) {
    assert(owned && !install_phase && !native_epoch && !qomci_current->active);
    return pending ? step() : 0;
}
int q1000k_ploam_send(const u8 message[44]) {
    assert(owned && !install_phase && !native_epoch && !qomci_current->active);
    assert(!message[0] && !message[1] && !message[2] && !message[3]);
    assert(message[4]==hardware_onu>>8 && message[5]==(u8)hardware_onu && message[6]==5);
    assert(message[9]==1 || message[9]==2);
    assert(!message[10] && !message[11]);
    for(int i=28;i<44;i++) assert(!message[i]);
    int ret=step(); if(ret) return ret;
    memcpy(last_report,message,44); data_reports++; return 0;
}
int q1000k_mac_ranging_install(u32 delay) { assert(owned && install_phase && !installed_profiles && delay<=0x3fffffff); return step(); }
int q1000k_mac_ranging_ready(void) { assert(owned && !install_phase); return step(); }
int q1000k_mac_keys_derive(struct crypto_lskcipher *tfm,const u8 reg[36],const u8 sn[8],const u8 tag[8],struct q1000k_mac_keys *keys) {
    assert(!owned && !auth_held && reg[35]==2 && sn[7]==1); derivations++; int ret=step(); if(ret) return ret;
    memset(keys,0,sizeof(*keys)); memcpy(keys->pon_tag,tag,8); memset(keys->bank[0].omci,0x30,16); memset(keys->bank[1].omci,0x31,16); return 0;
}
int q1000k_mac_profiles_invalidate(void) { assert(owned && install_phase); int ret=step(); if(!ret) installed_profiles=0; return ret; }
int q1000k_phy_profile_set(const struct q1000k_pon_profile *p) { assert(owned && install_phase && q1000k_pon_profile_valid(p)); int ret=step(); if(!ret) profile_version[p->index]=p->version; return ret; }
int q1000k_mac_profile_install(u8 index,u8 version,u16 length) {
    assert(owned && install_phase && index<4 && version<16 && length>0 && profile_version[index]==version); int ret=step(); if(!ret) installed_profiles|=BIT(index); return ret;
}
int q1000k_mac_keys_install(const struct q1000k_mac_keys *keys) { assert(owned && install_phase && !auth_held); return step(); }
int q1000k_mac_key_indices(u8 *ploam,u8 *omci) {
    assert(owned); int ret=step(); if(!ret) { *ploam=hw_ploam_index; *omci=hw_omci_index; } return ret;
}
int q1000k_mac_onu_install(u16 onu) { assert(owned && install_phase); int ret=step(); if(!ret) hardware_onu=onu; return ret; }
int q1000k_auth_omci_mic(struct crypto_lskcipher *tfm,const u8 key[16],const struct sk_buff *skb,bool has,u8 direction,u8 mic[4]) {
    assert(auth_held && !has && direction==2 && skb->len==44); memset(mic,key[0],4); return 0;
}
int q1000k_auth_omci_verify(struct crypto_lskcipher *tfm,const u8 key[16],const struct sk_buff *skb) {
    assert(auth_held); return skb->len==48 && skb->data[47]==key[0] ? 0 : -EBADMSG;
}
int q1000k_auth_ploam_verify(struct crypto_lskcipher *tfm,const u8 key[16],const u8 *message,size_t length) {
    assert(owned && tfm && key && message && length==48);
    return message[47]==key[0] ? 0 : -EBADMSG;
}
static void begin(void)
{
    static struct net_device dev;
    memset(&vendor,0,sizeof(vendor)); vendor.state=2; vendor.gponSecurity.omciIkIdx=1;
    installed_profiles=0; hw_ploam_index=hw_omci_index=0;
    owned=auth_held=fault=calls=fail_at=0; native_epoch=native_last=0;
    assert(!q1000k_omci_backend_init(&dev));
}
static void startup(void)
{
    begin();
    assert(!q1000k_omci_cold_start());
    assert(!qomci_current->keys_valid && !qomci_current->active && !native_epoch && !installed_profiles);
    assert(vendor.state==1 && qomci_current->omci->state==1 && !optical_tx);
    assert(q1000k_omci_cold_start()==-EALREADY);
    vendor.state=2; calls=0;
}
static void profile_and_assign(void)
{
    u8 tag[8]={1,2,3,4,5,6,7,8}; int token=q1000k_protocol_enter();
    struct q1000k_pon_profile profile={.repeat=2,.preamble_len=8,.delimiter_len=8,.version=1};
    assert(!q1000k_omci_burst_profile(&profile,tag,7,true)); assert(!q1000k_omci_assign(17)); q1000k_protocol_leave(token);
    q1000k_omci_control();
}
static void operational(void)
{
    int token=q1000k_protocol_enter();
    q1000k_omci_ranging(100,true,false,0,false);
    q1000k_protocol_leave(token); q1000k_omci_control();
}
int main(void)
{
    startup(); assert(q1000k_omci_assign(17)==-EPERM);
    assert(qomci_current->optical_work.pending);
    qomci_optical_work(&qomci_current->optical_work.work); assert(reported_los==1);
    phy_los_value=0; qomci_optical_work(&qomci_current->optical_work.work); assert(reported_los==0);
    phy_los_value=-EIO; qomci_optical_work(&qomci_current->optical_work.work); assert(reported_los==1);
    phy_los_value=1;
    struct omci_telemetry telemetry, saved_telemetry;
    memset(&telemetry,0xa5,sizeof(telemetry)); saved_telemetry=telemetry;
    assert(qomci_ops.get_telemetry(qomci_current->omci,&telemetry)==-ENODATA && !phy_fec_calls);
    assert(!memcmp(&telemetry,&saved_telemetry,sizeof(telemetry)));
    u8 ploam[48]={3,255}; ploam[47]=0x55;
    assert(q1000k_omci_ploam_verify(ploam,48)==-EPERM);
    int verify_owner=q1000k_protocol_enter();
    assert(!q1000k_omci_ploam_verify(ploam,48)); /* Broadcast before profiles. */
    assert(q1000k_omci_ploam_verify(ploam,52)==-EMSGSIZE);
    assert(q1000k_omci_ploam_verify(NULL,48)==-EMSGSIZE);
    ploam[0]=0; ploam[1]=17;
    assert(q1000k_omci_ploam_verify(ploam,48)==-ENOKEY);
    q1000k_protocol_leave(verify_owner);
    profile_and_assign();
    struct qomci_backend *b=qomci_current;
    assert(!fault && vendor.state==4 && b->onu==17 && hardware_onu==17 && !b->active);
    assert(ack_count==1 && b->keys_valid && !native_epoch && installed_profiles==1 && b->burst_mask==1);
    verify_owner=q1000k_protocol_enter();
    assert(q1000k_omci_ploam_verify(ploam,48)==-EBADMSG); /* No default-key fallback. */
    ploam[47]=b->keys.bank[0].ploam[0];
    assert(!q1000k_omci_ploam_verify(ploam,48));
    for(unsigned int type=0;type<256;type++) {
        ploam[2]=type; ploam[47]=b->keys.bank[0].ploam[0];
        assert(q1000k_omci_ploam_verify(ploam,48)==((type==5||type==9) ? -EBADMSG : 0));
        ploam[47]=0x55;
        assert(q1000k_omci_ploam_verify(ploam,48)==((type==5||type==9) ? 0 : -EBADMSG));
    }
    for(unsigned int type=0;type<=1;type++) for(unsigned int id=0;id<65536;id++) {
        unsigned int dest=id&1023;
        ploam[0]=id>>8; ploam[1]=id; ploam[2]=type;
        ploam[47]=0x55;
        if(dest==1022 && !type) assert(q1000k_omci_ploam_verify(ploam,48)==-EINVAL);
        else if(dest==1023 || dest==1022) assert(!q1000k_omci_ploam_verify(ploam,48));
        else if(dest!=17) assert(q1000k_omci_ploam_verify(ploam,48)==-ENOKEY);
        else {
            assert(q1000k_omci_ploam_verify(ploam,48)==-EBADMSG);
            ploam[47]=b->keys.bank[0].ploam[0];
            assert(!q1000k_omci_ploam_verify(ploam,48));
        }
    }
    b->request.reset=true; ploam[0]=0; ploam[1]=17; ploam[2]=0;
    assert(q1000k_omci_ploam_verify(ploam,48)==-ENOKEY);
    bool valid=b->keys_valid; b->keys_valid=false; ploam[47]=0x55;
    ploam[2]=5; assert(!q1000k_omci_ploam_verify(ploam,48));
    ploam[2]=9; assert(!q1000k_omci_ploam_verify(ploam,48));
    b->keys_valid=valid; b->request.reset=false;
    q1000k_protocol_leave(verify_owner);
    int same_tag_owner=q1000k_protocol_enter();
    u8 other_tag[8]={33};
    assert(!q1000k_omci_burst_profile(&b->burst[0],b->keys.pon_tag,8,true));
    assert(!b->request.profile && b->request.acks==1);
    assert(q1000k_omci_burst_profile(&b->burst[0],other_tag,9,true)==-EBUSY);
    assert(b->request.acks==1 && !memcmp(b->request.tag,b->keys.pon_tag,8));
    q1000k_protocol_leave(same_tag_owner); q1000k_omci_control();
    assert(!fault && ack_count==2);
    operational(); assert(!fault && b->active && services_enabled && b->omci->epoch==b->published && native_epoch==b->published);
    for(int value=0;value<2;value++) {
        phy_fec_value=value;
        assert(!qomci_ops.get_telemetry(b->omci,&telemetry));
        assert(telemetry.valid==OMCI_TELEMETRY_F_FEC_UPSTREAM);
        assert(telemetry.upstream_fec==(value ? OMCI_FEC_STATUS_UP : OMCI_FEC_STATUS_DOWN));
        assert(!telemetry.downstream_fec && !telemetry.bosa_rx_power_nw && !telemetry.bosa_tx_power_nw);
    }
    saved_telemetry=telemetry;
    phy_fec_value=-EIO;
    assert(qomci_ops.get_telemetry(b->omci,&telemetry)==-EIO);
    assert(!memcmp(&telemetry,&saved_telemetry,sizeof(telemetry)) && !owned);
    phy_fec_value=2;
    assert(qomci_ops.get_telemetry(b->omci,&telemetry)==-EIO);
    assert(!memcmp(&telemetry,&saved_telemetry,sizeof(telemetry)));
    for(int state=0;state<6;state++) {
        int before=phy_fec_calls;
        if(state==0) b->cold_started=false;
        if(state==1) b->started=false;
        if(state==2) b->active=false;
        if(state==3) b->request.reset=true;
        if(state==4) vendor.state=4;
        if(state==5) fault=-ETIMEDOUT;
        assert(qomci_ops.get_telemetry(b->omci,&telemetry)==(state==5 ? -ETIMEDOUT : -ENODATA));
        assert(phy_fec_calls==before && !memcmp(&telemetry,&saved_telemetry,sizeof(telemetry)) && !owned);
        b->cold_started=b->started=b->active=true; b->request.reset=false; vendor.state=5; fault=0;
    }
    phy_fec_value=1; phy_power_error=0;
    assert(!qomci_ops.get_telemetry(b->omci,&telemetry));
    assert(telemetry.valid==(OMCI_TELEMETRY_F_BOSA_RX_POWER|OMCI_TELEMETRY_F_FEC_UPSTREAM));
    assert(telemetry.bosa_rx_power_nw==19900);
    b->active=false; vendor.state=1;
    int fec_before=phy_fec_calls;
    assert(!qomci_ops.get_telemetry(b->omci,&telemetry));
    assert(telemetry.valid==OMCI_TELEMETRY_F_BOSA_RX_POWER && telemetry.bosa_rx_power_nw==19900);
    assert(phy_fec_calls==fec_before && !telemetry.upstream_fec);
    saved_telemetry=telemetry;
    phy_power_error=-EREMOTEIO;
    assert(qomci_ops.get_telemetry(b->omci,&telemetry)==-EREMOTEIO);
    assert(!memcmp(&telemetry,&saved_telemetry,sizeof(telemetry)));
    phy_power_error=-ENODATA;
    assert(qomci_ops.get_telemetry(b->omci,&telemetry)==-ENODATA);
    assert(!memcmp(&telemetry,&saved_telemetry,sizeof(telemetry)));
    b->active=true; vendor.state=5;
    assert(qomci_ops.get_telemetry(b->omci,NULL)==-EINVAL);
    assert(b->ranged && b->delay==100 && vendor.gponCfg.eqd==400);
    int ranging_owner=q1000k_protocol_enter(),prior_calls=calls;
    assert(q1000k_omci_ranging(0x40000000,true,false,0,true)==-ERANGE);
    assert(q1000k_omci_ranging(101,false,true,0,true)==-ERANGE && calls==prior_calls && b->active);
    assert(!q1000k_omci_ranging(23,false,false,22,true));
    assert(!b->active && b->delay==100 && vendor.gponCfg.eqd==400);
    assert(q1000k_omci_ranging(24,false,false,23,true)==-EBUSY);
    q1000k_protocol_leave(ranging_owner); q1000k_omci_control();
    assert(!fault && b->active && b->delay==123 && vendor.gponCfg.eqd==492);
    int before_reconcile=reconciles;
    int owner=q1000k_protocol_enter(); assert(!q1000k_omci_alloc_changed()); q1000k_protocol_leave(owner);
    q1000k_omci_control(); assert(reconciles==before_reconcile+1 && b->active && services_enabled);
    reconcile_error=-EOPNOTSUPP;
    owner=q1000k_protocol_enter(); assert(!q1000k_omci_alloc_changed()); q1000k_protocol_leave(owner);
    q1000k_omci_control(); assert(b->active && b->service_error==-EOPNOTSUPP && !fault);
    reconcile_error=0;

    struct sk_buff *p=packet(48); p->data[47]=0x30; q1000k_omci_receive(p,17,false);
    assert(core_count==1 && !live_skb);
    p=packet(48); p->data[47]=0x31; q1000k_omci_receive(p,17,false); assert(core_count==1 && !live_skb);
    p=packet(48); p->data[47]=0x30; q1000k_omci_receive(p,18,false); assert(core_count==1 && !live_skb);
    p=packet(44); assert(!qomci_xmit(b->omci,p,17,b->published) && !live_skb && native_count==1);
    p=packet(44); assert(qomci_xmit(b->omci,p,17,b->published-1)==-EKEYREJECTED && live_skb==1); dev_kfree_skb_any(p);
    native_error=-ENOBUFS; p=packet(44); assert(qomci_xmit(b->omci,p,17,b->published)==-ENOBUFS && live_skb==1); dev_kfree_skb_any(p); native_error=0;
    u64 old=b->published;
    int token=q1000k_protocol_enter();
    u8 new_tag[8]={7};
    struct q1000k_pon_profile new_profile=b->burst[0]; new_profile.version=2;
    assert(!q1000k_omci_burst_profile(&new_profile,new_tag,9,false));
    q1000k_protocol_leave(token);
    assert(!b->active && !services_enabled); q1000k_omci_control();
    assert(b->active && b->published>old && b->index==0 && !fault);
    p=packet(48); p->data[47]=0x30; q1000k_omci_receive(p,17,false); assert(core_count==2);
    token=q1000k_protocol_enter(); assert(!q1000k_omci_assign(18)); q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && hardware_onu==0xffff && b->onu==0xffff && vendor.state==1 && !b->active && !native_epoch);
    assert(b->omci->state==1 && !b->keys_valid && b->index==1 && !optical_tx);
    int cold_before=cold_count;
    token=q1000k_protocol_enter(); assert(!q1000k_omci_reset(true,true));
    u8 rejected_tag[8]={9};
    assert(q1000k_omci_profile(rejected_tag,9,true)==-EAGAIN);
    assert(q1000k_omci_assign(20)==-EAGAIN);
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && cold_count==cold_before+1 && vendor.state==7 && b->omci->state==7 && !b->keys_valid && vendor.emergencyState);
    q1000k_omci_backend_cleanup(); assert(!qomci_current && !live_skb);
    startup(); profile_and_assign(); operational(); b=qomci_current;
    assert(b->active && services_enabled);
    b->omci->ops->service_fault(b->omci,-EUCLEAN);
    assert(!b->active && !services_enabled && fault==-EUCLEAN && b->service_error==-EUCLEAN);
    q1000k_omci_backend_cleanup();
    /* A request arriving while core barriers run cannot publish stale ONU 17. */
    startup(); request_during_barrier=1; profile_and_assign();
    assert(!fault && qomci_current->onu==19 && hardware_onu==19);
    q1000k_omci_backend_cleanup();
    startup(); profile_and_assign(); operational(); int total=calls; assert(!fault); q1000k_omci_backend_cleanup();
    for(int n=1;n<=total;n++) {
        startup(); fail_at=n; profile_and_assign(); if(!fault) operational();
        assert(fault==-ETIMEDOUT && !qomci_current->active && !services_enabled && !native_epoch && !qomci_current->omci->epoch);
        assert(!owned && !auth_held); q1000k_omci_backend_cleanup();
    }
    begin(); assert(!q1000k_omci_cold_start()); total=calls; q1000k_omci_backend_cleanup();
    for(int n=1;n<=total;n++) {
        begin(); fail_at=n;
        assert(q1000k_omci_cold_start()==-ETIMEDOUT);
        assert(fault && !qomci_current->active && !qomci_current->keys_valid && !native_epoch);
        q1000k_omci_backend_cleanup();
    }
    for(int pair=1;pair<4;pair++) {
        startup(); profile_and_assign();
        GPON_Security_t before=vendor.gponSecurity;
        token=q1000k_protocol_enter(); hw_ploam_index=pair&1; hw_omci_index=!!(pair&2);
        assert(q1000k_omci_registration_keys()==-EKEYREJECTED);
        assert(!memcmp(&before,&vendor.gponSecurity,sizeof(before)));
        assert(fault==-EKEYREJECTED && !services_enabled && !qomci_current->active);
        q1000k_protocol_leave(token); q1000k_omci_backend_cleanup();
    }
    startup(); profile_and_assign(); token=q1000k_protocol_enter();
    assert(!q1000k_omci_registration_keys());
    u64 generation=qomci_current->request.generation;
    assert(vendor.gponSecurity.registerIDState==GPON_REG_ID_REPORTED);
    assert(!q1000k_omci_registration_keys() && generation==qomci_current->request.generation);
    q1000k_protocol_leave(token);
    assert(q1000k_omci_registration_keys()==-EPERM);
    q1000k_omci_backend_cleanup();
    startup(); token=q1000k_protocol_enter();
    struct q1000k_pon_profile queued={.repeat=2,.preamble_len=8,.delimiter_len=8,.version=1};
    u8 tag_a[8]={4}, tag_b[8]={5};
    assert(!q1000k_omci_burst_profile(&queued,tag_a,1,false));
    queued.index=3; queued.version=2;
    assert(!q1000k_omci_burst_profile(&queued,tag_b,2,true));
    assert(qomci_current->request.burst_mask==8 && !installed_profiles);
    queued.version=3;
    assert(q1000k_omci_burst_profile(&queued,tag_b,3,true)==-EBUSY);
    queued.repeat=0;
    assert(q1000k_omci_burst_profile(&queued,tag_b,3,true)==-EINVAL);
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && installed_profiles==8 && qomci_current->burst_mask==8 && profile_version[3]==2);
    assert(qomci_current->keys.pon_tag[0]==5);
    q1000k_omci_backend_cleanup();
    /* Data-key preparation is outside the protocol mutex. Install, ready
     * and report precede software publication and admission reopening.
     */
    startup(); profile_and_assign(); operational(); b=qomci_current;
    token=q1000k_protocol_enter();
    u64 initial=b->request.generation;
    for(int i=0;i<256;i++) if(i!=1 && i!=2) assert(q1000k_omci_key_control(false,i,16,5)==-EINVAL);
    for(int i=0;i<256;i++) if(i!=16) assert(q1000k_omci_key_control(false,1,i,5)==-EOPNOTSUPP);
    random_ready=false;
    assert(q1000k_omci_key_control(false,1,16,5)==-EAGAIN && b->active && b->request.generation==initial);
    random_ready=true;
    assert(!q1000k_omci_key_control(false,1,16,5));
    assert(q1000k_omci_key_control(false,1,16,5)==-EBUSY);
    assert(q1000k_omci_key_control(true,1,0,6)==-EBUSY);
    assert(q1000k_omci_profile(b->keys.pon_tag,1,false)==-EBUSY);
    assert(!b->active && !b->data.mac.rx_valid);
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && b->active && b->data.mac.rx_valid==1 && b->data.regenerating==1 && !data_tx);
    assert(last_report[7]==5 && !last_report[8] && last_report[9]==1);
    u8 mode=0xff;
    query_ring=1; assert(!qomci_gem_encryption(b->omci,99,&mode) && mode==1);
    query_ring=3; assert(!qomci_gem_encryption(b->omci,99,&mode) && mode==1);
    query_ring=0; assert(!qomci_gem_encryption(b->omci,99,&mode) && !mode);
    mode=0xff; assert(qomci_gem_encryption(b->omci,98,&mode)==-ENOENT && mode==0xff);
    int installed=data_installs, reported=data_reports, generated=random_calls;
    token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(false,1,16,9));
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && b->active && data_installs==installed && random_calls==generated && data_reports==reported+1);
    assert(last_report[7]==9);
    token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(true,1,0,10));
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && b->active && !b->data.regenerating && b->data.mac.tx_index==1 && data_tx==1 && data_rx==1);
    token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(false,2,16,11));
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && data_tx==1 && data_rx==3 && b->data.regenerating==2);
    token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(true,2,0,12));
    q1000k_protocol_leave(token); q1000k_omci_control();
    assert(!fault && data_tx==2 && data_rx==2 && !b->data.regenerating);
    for(int i=0;i<16;i++) assert(!data_keys[0][i] && !b->data.mac.key[0][i]);
    /* Reset in the core barrier supersedes a previously accepted request. */
    reported=data_reports;
    token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(false,1,16,13));
    q1000k_protocol_leave(token); request_during_barrier=2; q1000k_omci_control();
    assert(!fault && data_reports==reported && !data_rx && !data_tx && !b->active && vendor.state==7);
    for(int i=0;i<32;i++) assert(!((u8 *)b->data.mac.key)[i]);
    q1000k_omci_backend_cleanup();
    /* Inject failure into every crypto/install/activation/report/open step. */
    for(int failing=0,total_steps=1;failing<=total_steps;failing++) {
        startup(); profile_and_assign(); operational();
        token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(false,1,16,1));
        q1000k_protocol_leave(token); q1000k_omci_control(); assert(!fault);
        token=q1000k_protocol_enter(); assert(!q1000k_omci_key_control(true,1,0,2));
        q1000k_protocol_leave(token); calls=0; fail_at=failing; q1000k_omci_control();
        if(!failing) { assert(!fault && qomci_current->active); total_steps=calls; }
        else assert(fault==-ETIMEDOUT && !qomci_current->active && !services_enabled && !native_epoch && !qomci_current->omci->epoch);
        q1000k_omci_backend_cleanup();
    }
    identity_override=(struct omci_identity){.valid=OMCI_IDENTITY_F_VERSION|OMCI_IDENTITY_F_EQUIPMENT_ID,
        .version="TEST-version14",.equipment_id="TEST-equipment-20byt"};
    begin();
    assert(!memcmp(identity_seen.version,identity_override.version,sizeof(identity_seen.version)));
    assert(!memcmp(identity_seen.equipment_id,identity_override.equipment_id,sizeof(identity_seen.equipment_id)));
    q1000k_omci_backend_cleanup();
    assert(!live_skb); return 0;
}
