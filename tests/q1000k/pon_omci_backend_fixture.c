// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define OMCI_OLT_VENDOR_ID_LEN 4
#define OMCI_OLT_VERSION_LEN 14
#define OMCI_OLT_EQUIPMENT_ID_LEN 20
#define OMCI_CONFIG_SOURCE_DRIVER 1
#define OMCI_IDENTITY_F_SERIAL_NUMBER 1
#define OMCI_IDENTITY_F_VENDOR_ID 2
#define OMCI_IDENTITY_F_EQUIPMENT_ID 16
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
#define DS_FEC_SETTING_FORCE_ON 3
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
/* TYPES */
struct omci_device { const struct omci_device_ops *ops; void *priv; u16 onu,gem; u64 epoch,last; bool channel; u8 state; };
typedef struct { u8 msk[16],sk[16],ploamIk[2][16],omciIk[2][16],kek[2][16];
    u8 omciIkIdx,ploamIkIdx,kekIdx,aesUcKey[2][16],aesBcKey[2][16],aesUcKeyIdx;
    u8 smaValid,registerIDState,txKeyValid,state;
} GPON_Security_t;
static struct { struct { u16 onu_id,omcc; u8 ponTag[8]; int ploamCtrl,usOmciMicCtrl,dsOmciMicCtrl; u32 eqd; } gponCfg;
    GPON_Security_t gponSecurity;
    u8 state, prePloamMsg[52]; int swreplyploam_task, gpon_traffic_status,gemUpAESMode; bool typeBOnGoing;
} vendor,*gpGponPriv=&vendor;
static struct { int traffic_status_refresh_timer; } phy,*gpPhyData=&phy;
#define GPON_CURR_STATE (vendor.state)
static int owned, auth_held, fault, calls, fail_at, native_count, core_count, rejected_count;
static int reconciles, reconcile_error;
static int ack_count, resets, service_resets, live_skb, native_error, derivations;
static int request_during_barrier, assign_count, refresh_count;
static bool services_enabled, install_phase, optical_tx;
static int cold_count;
static u8 hw_ploam_index, hw_omci_index;
int snSendInO23Cnt;
static u16 hardware_onu;
static u64 native_epoch,native_last;
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static void *kzalloc(size_t n,int flags) { return calloc(1,n); }
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
static void gpon_act_change_state(u8 state) { assert(owned); vendor.state=state; q1000k_omci_state(); }
static void ploam_send_acknowledge_msg(u8 seq,int result) { assert(owned && !result); ack_count++; }
static void q1000k_services_enable(bool enabled) { services_enabled=enabled; }
static void q1000k_services_init(void) { services_enabled=false; }
static void q1000k_services_reset(void) { assert(owned); service_resets++; }
static void q1000k_services_destroy(void) { services_enabled=false; }
static int q1000k_services_topology(struct omci_device *o,struct omci_ani_topology *t) { return 0; }
static int q1000k_services_tcont(struct omci_device *o,u16 e,u16 a,bool v) { return 0; }
static int q1000k_services_gem(struct omci_device *o,u16 e,u16 g,u16 t,u8 d,bool v,bool enc) { return 0; }
static int q1000k_services_queue(struct omci_device *o,u16 e,const struct omci_priority_queue_config *q) { return 0; }
static int q1000k_services_scheduler(struct omci_device *o,u16 e,const struct omci_traffic_scheduler_config *s) { return 0; }
static int q1000k_services_uni(struct omci_device *o,u16 e,bool enable) { return 0; }
static int q1000k_services_replace(struct omci_device *o,const struct omci_service_config *s,size_t n) { return 0; }
static int q1000k_pon_get_serial(u8 *s,int n) { assert(n==8); memset(s,1,n); return 0; }
static int q1000k_pon_get_registration(u8 *s,int n) { assert(n==36); memset(s,2,n); return 0; }
static struct crypto_lskcipher *crypto_alloc_lskcipher(const char *alg,int a,int b) { return calloc(1,sizeof(struct crypto_lskcipher)); }
static void crypto_free_lskcipher(struct crypto_lskcipher *c) { free(c); }
static struct device *get_xpon_dev(void) { static struct device dev; return &dev; }
static struct xpon_device *xpon_device_register(struct device *d,const struct xpon_device_desc *desc) { return calloc(1,sizeof(struct xpon_device)); }
static void xpon_device_unregister(struct xpon_device *x) { free(x); }
static void xpon_device_report_registration(struct xpon_device *x,int state) { assert(!owned); }
static void xpon_device_report_carrier(struct xpon_device *x,bool up) { assert(!owned); }
static struct omci_device *omci_device_register(struct xpon_device *x,u32 caps,const struct omci_device_ops *ops,void *priv) {
    struct omci_device *o=calloc(1,sizeof(*o)); o->ops=ops; o->priv=priv; return o;
}
static void *omci_device_priv(struct omci_device *o) { return o->priv; }
static void omci_device_set_identity_info(struct omci_device *o,const struct omci_identity *id) {
    assert(id->serial_source==OMCI_CONFIG_SOURCE_DRIVER && id->vendor_source==OMCI_CONFIG_SOURCE_DRIVER);
}
static int omci_device_start(struct omci_device *o) { return o->ops->start(o); }
static void omci_device_unregister(struct omci_device *o) { o->ops->stop(o); free(o); }
static int omci_device_set_auth_epoch(struct omci_device *o,u64 epoch) {
    assert(!owned && !auth_held);
    if(!epoch && request_during_barrier) {
        request_during_barrier=0; int token=q1000k_protocol_enter();
        assert(token>=0); assert(!q1000k_omci_assign(19)); q1000k_protocol_leave(token);
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
static void xmcs_report_event(int type,int event,u8 state) { assert(owned && type==1 && event==2 && (state==1||state==7)); }
static int q1000k_phy_set_tx(bool enable) { assert(owned); int ret=step(); if(!ret) optical_tx=enable; return ret; }
static int q1000k_phy_configure(u32 mode) { assert(owned && install_phase && mode==PHY_XGSPON_CONFIG); return step(); }
static int XPON_PHY_SET_RX_ENABLE(void) { assert(owned && install_phase); return step(); }
static int XPON_PHY_SET_RX_FEC(int mode) { assert(owned && install_phase && mode==3); return step(); }
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
int q1000k_mac_keys_derive(struct crypto_lskcipher *tfm,const u8 reg[36],const u8 sn[8],const u8 tag[8],struct q1000k_mac_keys *keys) {
    assert(!owned && !auth_held && reg[35]==2 && sn[7]==1); derivations++; int ret=step(); if(ret) return ret;
    memset(keys,0,sizeof(*keys)); memcpy(keys->pon_tag,tag,8); memset(keys->bank[0].omci,0x30,16); memset(keys->bank[1].omci,0x31,16); return 0;
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
static void begin(void)
{
    static struct net_device dev;
    memset(&vendor,0,sizeof(vendor)); vendor.state=2; vendor.gponSecurity.omciIkIdx=1;
    hw_ploam_index=hw_omci_index=0;
    owned=auth_held=fault=calls=fail_at=0; native_epoch=native_last=0;
    assert(!q1000k_omci_backend_init(&dev));
}
static void startup(void)
{
    begin();
    assert(!q1000k_omci_cold_start());
    assert(!qomci_current->keys_valid && !qomci_current->active && !native_epoch);
    assert(vendor.state==1 && qomci_current->omci->state==1 && !optical_tx);
    assert(q1000k_omci_cold_start()==-EALREADY);
    vendor.state=2; calls=0;
}
static void profile_and_assign(void)
{
    u8 tag[8]={1,2,3,4,5,6,7,8}; int token=q1000k_protocol_enter();
    assert(!q1000k_omci_profile(tag,7,true)); assert(!q1000k_omci_assign(17)); q1000k_protocol_leave(token);
    q1000k_omci_control();
}
static void operational(void)
{
    int token=q1000k_protocol_enter();
    int ret=q1000k_omci_registration_keys();
    if(!ret) gpon_act_change_state(5);
    q1000k_protocol_leave(token); q1000k_omci_control();
}
int main(void)
{
    startup(); assert(q1000k_omci_assign(17)==-EPERM); profile_and_assign();
    struct qomci_backend *b=qomci_current;
    assert(!fault && vendor.state==4 && b->onu==17 && hardware_onu==17 && !b->active);
    assert(ack_count==1 && b->keys_valid && !native_epoch);
    operational(); assert(!fault && b->active && services_enabled && b->omci->epoch==b->published && native_epoch==b->published);
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
    u8 new_tag[8]={7}; assert(!q1000k_omci_profile(new_tag,9,false));
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
    assert(!fault && cold_count==cold_before+1 && vendor.state==7 && b->omci->state==7 && !b->keys_valid);
    q1000k_omci_backend_cleanup(); assert(!qomci_current && !live_skb);
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
    assert(!live_skb); return 0;
}
