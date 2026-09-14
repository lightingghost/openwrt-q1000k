// SPDX-License-Identifier: GPL-2.0-only
/* Production registry and callers; only MAC/native/FE callbacks are modeled. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef u16 ushort;
typedef u8 unchar;
typedef unsigned int uint;
#define Q1000K_PON_IDENTITY
#define Q1000K_ALLOC_ID_MAX 0x3fff
#define CONFIG_GPON_10G_MAX_TCONT 32
#define CONFIG_GPON_10G_MAX_GEMPORT 256
#define GPON_GEM_IDX_MASK 0x7fff
#define GPON_10G_UNASSIGN_ALLOC_ID 0xffff
#define GPON_UNASSIGN_ONU_ID 0x3ff
#define GPON_OMCC_ID 17
#define GPON_MAX_ANI_INTERFACE 256
#define GPON_MULTICAST_CHANNEL 32
#define GPON_UNKNOWN_CHANNEL 33
#define GPON_UNICAST_GEM 0
#define GPON_MULTICAST_GEM 1
#define NO_ENCRYPTION 0
#define READ_ONCE(x) (x)
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define kfree free
#define BIT(n) (UINT32_C(1)<<(n))
typedef enum { ENUM_CFG_NETIDX,ENUM_CFG_CHANNEL,ENUM_CFG_ENCRYPTION,ENUM_CFG_LOOPBACK } ENUM_GWanGemCfgType_t;
typedef struct { uint portId,ani; u8 channel,rxLb,rxEncrypt,txEncrypt,valid; u16 allocId; } GWAN_GemInfo_T;
struct net_device_stats { unsigned long tx_packets,tx_bytes,rx_packets,rx_bytes; };
static struct {
    struct { u16 allocId[32],gemIdToIndex[65536]; unsigned int gemNumbers;
        struct { GWAN_GemInfo_T info; struct net_device_stats stats; } gemPort[256];
    } gpon;
    unsigned int activeChannelNum;
} wan,*gpWanPriv=&wan;
struct XMCS_GemPortCreate_S { u16 gemPortId,allocId; uint gemType,gemEncrypt; };
typedef atomic_int atomic_t;
#define ATOMIC_INIT(x) (x)
static int atomic_cmpxchg(atomic_t *p,int old,int value) {
    atomic_compare_exchange_strong(p,&old,value); return old;
}
#define atomic_set_release(p,v) atomic_store_explicit(p,v,memory_order_release)
#define DEFINE_SPINLOCK(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
static _Thread_local bool held;
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!held); assert(!pthread_mutex_lock(l)); held=true; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(held); held=false; assert(!pthread_mutex_unlock(l)); } while(0)
#define lockdep_assert_held(l) assert(held)
static bool faulted,reenter,yield_io;
static int write_error,quiesce_error;
static unsigned int writes,quiesces;
static u32 quarantine,closed;
static bool hardware[65536];
struct q1000k_gem_value;
static void q1000k_tcont_quarantine(unsigned int channel);
static int q1000k_transport_quiesce_channel(u8 channel);
struct airoha_pon_qos { u16 weights[8]; u8 mode; bool byte_mode,scale16; };
static struct airoha_pon_qos qos_model[32];
static u16 tcont_model[32];
static u8 queue_model[32];
static int physical_ops,physical_fail,physical_phase,protocol_error,async_protocol_fault_step;
static bool physical_started, producers_drained, cold_expected, reset_done, receive_only;
static u32 rx_channels;
static bool optical_tx;
static int tx_query_error, tx_queries;
static int q1000k_phy_get_tx(bool *enabled) {
    tx_queries++; if(tx_query_error) return tx_query_error;
    *enabled=optical_tx; return 0;
}
static pthread_mutex_t protocol_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int protocol_owned;
static int q1000k_protocol_enter(void) {
    if(protocol_owned) return 1;
    assert(!pthread_mutex_lock(&protocol_lock)); protocol_owned=1; return 0;
}
static void q1000k_protocol_leave(int token) {
    if(token) return;
    assert(protocol_owned); protocol_owned=0; assert(!pthread_mutex_unlock(&protocol_lock));
}
static int q1000k_protocol_status(void) { return protocol_error; }
static void q1000k_protocol_fail(int error) { assert(error<0); protocol_error=error; memset(queue_model,255,sizeof(queue_model)); }
static void synchronize_rcu(void) { assert(protocol_owned && !held); producers_drained=true; }
static int physical_step(void) {
    assert(!held && protocol_owned); physical_ops++;
    if(physical_ops==async_protocol_fault_step) protocol_error=-ENOSPC;
    return physical_ops==physical_fail ? -ETIMEDOUT : 0;
}
static int q1000k_transport_get_queue_close(u8 ch,u8 *closed) {
    int ret=physical_step(); if(!ret) *closed=queue_model[ch]; return ret;
}
static int q1000k_transport_set_queue_close(u8 ch,u8 closed) {
    assert(physical_phase==3); int ret=physical_step(); if(!ret) queue_model[ch]=closed; return ret;
}
static int q1000k_transport_get_qos(u8 ch,struct airoha_pon_qos *qos) {
    assert(physical_phase==1); int ret=physical_step(); if(!ret) *qos=qos_model[ch]; return ret;
}
static int q1000k_transport_set_qos(u8 ch,const struct airoha_pon_qos *qos) {
    assert(physical_phase==2 && !memcmp(qos,&qos_model[ch],sizeof(*qos))); return physical_step();
}
static int q1000k_tcont_clear_namespace(void) {
    assert(physical_phase==1); int ret=physical_step(); if(!ret) for(int i=1;i<32;i++) tcont_model[i]=0xffff; return ret;
}
static int q1000k_tcont_install(unsigned int ch,u16 alloc,u16 onu) {
    assert(physical_phase==2 && ch>0 && ch<32 && onu==17 && tcont_model[ch]==0xffff);
    int ret=physical_step(); if(!ret) tcont_model[ch]=alloc; return ret;
}
static int q1000k_protocol_reset_jobs(void) { assert(physical_phase==1 && reset_done); return physical_step(); }
static int q1000k_mac_cold_release(void) {
    assert(physical_phase==1 && reset_done); return physical_step();
}
static int q1000k_gem_clear_namespace(u16 preserve) {
    assert(physical_phase==1 && reset_done && preserve==0xffff);
    int ret=physical_step(); if(!ret) memset(hardware,0,sizeof(hardware)); return ret;
}
/* PRODUCTION */
int q1000k_pipeline_reconfigure(const struct q1000k_pipeline_ops *ops,void *arg,u32 channels)
{
    assert(producers_drained && q1000k_gwan_changing && ops->reset_mac==cold_expected && atomic_load(&q1000k_tcont_config_busy)==1);
    physical_started=true; int ret=physical_step(); if(ret) return ret;
    optical_tx=false; receive_only=false;
    memset(queue_model,255,sizeof(queue_model)); physical_phase=1;
    if(ops->reset_mac) { ret=physical_step(); if(ret) return ret; reset_done=true; }
    ret=ops->clear(arg); if(ret) return ret;
    ret=physical_step(); if(ret) return ret; /* The separate namespace fixture tests real FCS/epoch commands. */
    physical_phase=2; ret=ops->install(arg); if(ret) return ret;
    rx_channels=channels; return 0;
}
int q1000k_pipeline_activate(void)
{
    assert(q1000k_gwan_changing && physical_phase==2);
    for(unsigned int i=1;i<32;i++) assert(wan.gpon.allocId[i]==tcont_model[i]);
    int ret=physical_step(); if(!ret) { physical_phase=3; optical_tx=!receive_only; } return ret;
}

int q1000k_pipeline_activate_receive_only(void) { receive_only=true; return q1000k_pipeline_activate(); }

bool q1000k_gem_faulted(void) { return faulted; }
int q1000k_gem_replace(u16 gem,const struct q1000k_gem_value *expected,
                      const struct q1000k_gem_value *value)
{
    struct q1000k_gwan_binding binding;
    assert(!held && atomic_load(&q1000k_tcont_config_busy)==1);
    if(!q1000k_gwan_changing) {
        assert(expected->valid==0 && value->valid==1 && !value->encrypted);
        assert(q1000k_gwan_binding(gem,false,&binding)==-ENOENT);
    } else {
        assert(physical_phase==1 || physical_phase==2);
        int ret=physical_step(); if(ret) return ret;
    }
    writes++;
    if(reenter) {
        assert(gwan_create_new_gemport(800,33,0,200)==-EBUSY);
        assert(gwan_config_gemport(gem,ENUM_CFG_NETIDX,3)==-EBUSY);
        assert(gwan_remove_gemport(gem)==-EBUSY);
        assert(gwan_remove_all_gemport()==-EBUSY);
    }
    if(yield_io) sched_yield();
    if(faulted) return -EIO;
    if(write_error) return write_error;
    if(hardware[gem] != !!expected->valid) return -ESTALE;
    hardware[gem]=!!value->valid; return 0;
}
static void q1000k_tcont_quarantine(unsigned int channel)
{
    assert(!held && channel>0 && channel<32); quarantine |= BIT(channel);
}
static int q1000k_transport_quiesce_channel(u8 channel)
{
    assert(!held && channel>0 && channel<32 && atomic_load(&q1000k_tcont_config_busy)==1);
    assert(quarantine & BIT(channel)); quiesces++; closed |= BIT(channel);
    if(reenter) assert(gwan_config_gemport(500,ENUM_CFG_NETIDX,4)==-EBUSY);
    return quiesce_error;
}
static void reset_model(void)
{
    assert(!held); memset(&wan,0,sizeof(wan)); memset(hardware,0,sizeof(hardware));
    for(unsigned int i=0;i<65536;i++) wan.gpon.gemIdToIndex[i]=0x7fff;
    for(unsigned int i=0;i<32;i++) wan.gpon.allocId[i]=0xffff;
    wan.gpon.allocId[0]=17; wan.activeChannelNum=1;
    memset(q1000k_gwan_retiring_gems,0,sizeof(q1000k_gwan_retiring_gems));
    q1000k_gwan_retiring_channels=quarantine=closed=0;
    atomic_store(&q1000k_tcont_config_busy,0);
    faulted=reenter=yield_io=false; writes=quiesces=0; write_error=quiesce_error=0;
    q1000k_gwan_error=0; q1000k_gwan_changing=false;
    physical_ops=physical_fail=physical_phase=protocol_error=async_protocol_fault_step=0; physical_started=producers_drained=cold_expected=reset_done=receive_only=false; rx_channels=0;
    optical_tx=true; tx_query_error=tx_queries=0;
    memset(queue_model,0,sizeof(queue_model)); memset(qos_model,0,sizeof(qos_model));
    for(unsigned int i=0;i<32;i++) { tcont_model[i]=wan.gpon.allocId[i]; qos_model[i].mode=i%8; qos_model[i].weights[0]=i+1; }
}
static void publish(u8 channel,u16 alloc_id)
{
    assert(!atomic_cmpxchg(&q1000k_tcont_config_busy,0,1));
    q1000k_gwan_publish_tcont(channel,alloc_id); tcont_model[channel]=alloc_id;
    atomic_store(&q1000k_tcont_config_busy,0);
}
static void failed_binding(u16 gem,bool tx,int error)
{
    struct q1000k_gwan_binding b,before;
    memset(&b,0xa5,sizeof(b)); before=b;
    assert(q1000k_gwan_binding(gem,tx,&b)==error && !memcmp(&b,&before,sizeof(b)));
}
static void create_ready(u16 gem,u8 channel,u16 alloc_id,uint ani)
{
    if(wan.gpon.allocId[channel]==0xffff) publish(channel,alloc_id);
    assert(!gwan_create_new_gemport(gem,channel,0,alloc_id));
    assert(!gwan_config_gemport(gem,ENUM_CFG_NETIDX,ani));
}
static atomic_bool finished;
static atomic_ulong snapshots;
static void *reader(void *unused)
{
    do {
        for(unsigned int gem=1000;gem<1256;gem++) {
            struct q1000k_gwan_binding b;
            int ret=q1000k_gwan_binding(gem,true,&b);
            assert(!ret || ret==-ENOENT || ret==-ENODATA || ret==-ESHUTDOWN);
            if(!ret) {
                assert(b.gem==gem && b.alloc_id==200 && b.channel==4 && b.ani==gem-1000 && !b.multicast);
                q1000k_gwan_account(gem,true,60);
                atomic_fetch_add(&snapshots,1);
            }
        }
    } while(!atomic_load(&finished));
    return NULL;
}
static void *creator(void *arg)
{
    unsigned int lane=(uintptr_t)arg;
    for(unsigned int gem=1000+lane;gem<1256;gem+=4) {
        int ret;
        do { ret=gwan_create_new_gemport(gem,33,0,200); if(ret==-EBUSY) sched_yield(); } while(ret==-EBUSY);
        assert(!ret);
        do { ret=gwan_config_gemport(gem,ENUM_CFG_NETIDX,gem-1000); if(ret==-EBUSY) sched_yield(); } while(ret==-EBUSY);
        assert(!ret);
    }
    return NULL;
}
static int cold_install(void *arg) {
    assert(arg==&wan && reset_done && physical_phase==2);
    for(unsigned int i=0;i<65535;i++) assert(!hardware[i]);
    return physical_step();
}
int main(void)
{
    struct q1000k_gwan_binding b;
    struct XMCS_GemPortCreate_S cfg={.gemPortId=65534,.allocId=200};
    reset_model();
    assert(xmcs_create_gem_port(NULL)==-EINVAL && xmcs_set_gem_encrypt(NULL)==-EINVAL);
    assert(gwan_create_new_gemport(0,33,0,200)==-EOPNOTSUPP);
    assert(gwan_create_new_gemport(17,33,0,200)==-EOPNOTSUPP);
    assert(gwan_create_new_gemport(500,0,0,17)==-EOPNOTSUPP);
    assert(gwan_create_new_gemport(0xffff,33,0,200)==-EINVAL);
    assert(gwan_create_new_gemport(500,32,0,200)==-EINVAL);
    assert(gwan_create_new_gemport(500,31,1,200)==-EINVAL);
    assert(gwan_create_new_gemport(500,33,0,0xffff)==-EINVAL);
    assert(gwan_create_new_gemport(500,4,0,200)==-ESTALE && !writes);
    failed_binding(0xffff,true,-EINVAL); failed_binding(500,true,-ENOENT);
    q1000k_gwan_account(0xffff,true,60); q1000k_gwan_account(500,true,60);
    assert(q1000k_gwan_binding(500,true,NULL)==-EINVAL); /* Null output is rejected. */
    for(unsigned int channel=34;channel<256;channel++)
        assert(gwan_create_new_gemport(500,channel,0,200)==-EINVAL);
    int errors[]={-ETIMEDOUT,-EIO,-EBUSY,-ESTALE};
    for(unsigned int i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        write_error=errors[i];
        assert(xmcs_create_gem_port(&cfg)==(errors[i]==-ESTALE ? -EEXIST : errors[i]));
        assert(!wan.gpon.gemNumbers && wan.gpon.gemIdToIndex[65534]==0x7fff && !hardware[65534]);
    }
    write_error=0; reenter=true;
    assert(!xmcs_create_gem_port(&cfg));
    assert(wan.gpon.gemNumbers==1 && hardware[65534] && wan.gpon.gemPort[0].info.channel==33);
    assert(xmcs_create_gem_port(&cfg)==-EEXIST);
    assert(gpon_recover_create_gemport()==-EOPNOTSUPP);
    assert(!gwan_config_gemport(65534,ENUM_CFG_NETIDX,9)); failed_binding(65534,true,-ENODATA);
    publish(4,200);
    assert(!q1000k_gwan_binding(65534,true,&b) && b.gem==65534 && b.channel==4 && b.ani==9 && b.alloc_id==200);
    assert(gwan_config_gemport(65534,ENUM_CFG_NETIDX,10)==-EOPNOTSUPP);
    publish(5,201);
    assert(gwan_config_gemport(65534,ENUM_CFG_CHANNEL,5)==-EOPNOTSUPP);
    assert(!gwan_config_gemport(65534,ENUM_CFG_CHANNEL,4));
    assert(!gwan_config_gemport(65534,ENUM_CFG_ENCRYPTION,0));
    cfg.gemEncrypt=1; assert(xmcs_set_gem_encrypt(&cfg)==-EOPNOTSUPP);
    assert(gwan_config_gemport_encrypt(65534,256,0)==-EINVAL);
    assert(gwan_config_gemport(65534,ENUM_CFG_LOOPBACK,1)==-EOPNOTSUPP);
    assert(gwan_config_gemport(65534,ENUM_CFG_NETIDX,257)==-EINVAL);
    assert(gwan_config_gemport(65534,999,1)==-EINVAL);
    for(unsigned int channel=34;channel<256;channel++) {
        wan.gpon.gemPort[0].info.channel=channel;
        failed_binding(65534,true,-ENODATA); /* Every malformed table channel is bounded before alloc lookup. */
    }
    wan.gpon.gemPort[0].info.channel=4;
    q1000k_gwan_account(65534,true,60); q1000k_gwan_account(65534,false,100);
    assert(wan.gpon.gemPort[0].stats.tx_packets==1 && wan.gpon.gemPort[0].stats.rx_bytes==100);
    create_ready(500,4,200,3);
    assert(!xmcs_remove_gem_port(65534) && wan.gpon.gemNumbers==1 && !hardware[65534]);
    failed_binding(65534,true,-ENOENT);
    assert(!q1000k_gwan_binding(500,true,&b) && b.channel==4 && b.ani==3);
    assert(!gwan_create_new_gemport(65534,4,0,200));
    assert(!gwan_remove_all_gemport_for_disable() && !wan.gpon.gemNumbers && !hardware[500]);

    reset_model(); assert(!gwan_remove_all_gemport());
    assert(!gwan_create_new_gemport(600,32,1,0xffff));
    failed_binding(600,false,-ENODATA); assert(!gwan_config_gemport(600,ENUM_CFG_NETIDX,255));
    assert(!q1000k_gwan_binding(600,false,&b) && b.multicast && b.ani==255);
    failed_binding(600,true,-EOPNOTSUPP);
    assert(!gwan_remove_gemport(600) && !wan.gpon.gemNumbers);
    failed_binding(600,false,-ENOENT);
    assert(!gwan_create_new_gemport(601,33,0,202));
    assert(!gwan_remove_gemport(601) && !wan.gpon.gemNumbers);
    publish(6,202);

    reset_model(); create_ready(500,4,200,7);
    assert(gwan_config_gemport(500,ENUM_CFG_NETIDX,256)==-EOPNOTSUPP);
    assert(wan.gpon.gemPort[0].info.ani==7 && closed==BIT(4));
    failed_binding(500,false,-ESHUTDOWN);
    reset_model(); create_ready(500,4,200,7); faulted=true;
    failed_binding(500,true,-EIO); assert(gwan_config_gemport(500,ENUM_CFG_NETIDX,7)==-EIO);
    assert(gwan_remove_gemport(500)==-EUCLEAN && protocol_error==-EIO);
    failed_binding(500,false,-EIO);

    reset_model(); create_ready(500,4,200,7);
    wan.gpon.gemIdToIndex[500]=255; failed_binding(500,true,-ENOENT);
    assert(gwan_create_new_gemport(500,4,0,200)==-EEXIST && writes==1);
    reset_model(); hardware[500]=true;
    assert(gwan_create_new_gemport(500,33,0,200)==-EEXIST && !wan.gpon.gemNumbers);

    reset_model(); publish(4,200); yield_io=true;
    pthread_t readers[4],creators[4]; atomic_store(&finished,false);
    for(unsigned int i=0;i<4;i++) assert(!pthread_create(&readers[i],NULL,reader,NULL));
    for(unsigned int i=0;i<4;i++) assert(!pthread_create(&creators[i],NULL,creator,(void *)(uintptr_t)i));
    for(unsigned int i=0;i<4;i++) assert(!pthread_join(creators[i],NULL));
    while(!atomic_load(&snapshots)) sched_yield();
    assert(gwan_create_new_gemport(65000,4,0,200)==-ENOSPC && wan.gpon.gemNumbers==256 && writes==256);
    for(unsigned int gem=1000;gem<1256;gem++) {
        assert(!q1000k_gwan_binding(gem,true,&b));
        assert(b.index<256 && b.gem==gem && b.ani==gem-1000);
    }
    unsigned int gem=wan.gpon.gemPort[255].info.portId;
    assert(!q1000k_gwan_binding(gem,true,&b) && b.index==255);
    assert(!gwan_remove_all_gemport() && !wan.gpon.gemNumbers);
    atomic_store(&finished,true);
    for(unsigned int i=0;i<4;i++) assert(!pthread_join(readers[i],NULL));
    reset_model(); cold_expected=true;
    create_ready(500,4,200,7); hardware[65534]=hardware[0]=true;
    physical_ops=0;
    assert(!q1000k_gwan_cold_reset(cold_install,&wan));
    int cold_steps=physical_ops;
    assert(reset_done && receive_only && !optical_tx && !tx_queries && !wan.gpon.gemNumbers && rx_channels==1);
    for(int i=0;i<32;i++) assert(wan.gpon.allocId[i]==0xffff && queue_model[i]==255);
    for(int n=1;n<=cold_steps;n++) {
        reset_model(); cold_expected=true; create_ready(500,4,200,7);
        hardware[65534]=true; physical_ops=0; physical_fail=n;
        int ret=q1000k_gwan_cold_reset(cold_install,&wan);
        assert(ret==(physical_started ? -EUCLEAN : -ETIMEDOUT));
        if(!physical_started) assert(wan.gpon.gemNumbers==1 && hardware[500] && hardware[65534]);
        else assert(protocol_error);
    }
    return 0;
}
