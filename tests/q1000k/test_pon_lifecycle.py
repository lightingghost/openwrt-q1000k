#!/usr/bin/env python3
"""Fault injection against extracted production MAC lifecycle functions.

All hardware, driver-core publication, and allocation operations are host
fixtures. No module is loaded and no connection to the router is made.
"""
from pathlib import Path
import os
import re
import unittest
from test_pon_identity import BSP
from pon_test_utils import run_c

MAC = BSP.parent / 'xpon-en757x/xpon_10g/src'


def function(path, name):
    source = (MAC / path).read_text(errors='replace')
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    match = re.search(r'^(?:static )?(?:inline )?(?:int|void|bool)\s+' +
                      name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


class PonLifecycleTests(unittest.TestCase):
    def test_worker_stops_when_idle_or_queue_is_full(self):
        production = ''.join(function('xpon_daemon.c', name) for name in [
            'xpon_daemon_job_enqueue', 'xpon_daemon_job_dequeue',
            'xpon_daemon_job_dequeue_not_empty', 'xpon_daemon', 'xpon_daemon_quit'])
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
typedef unsigned int uint;
typedef unsigned long ulong;
#define TRUE 1
#define FALSE 0
#define XPON_DAEMON_JOB_QUEUE_SIZE 8
#define JOB_QUEUE_IDX_INC(n) (((n)+1)%XPON_DAEMON_JOB_QUEUE_SIZE)
#define XD_SUCCESS 0
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p)>=(uintptr_t)-4095)
#define mb() atomic_thread_fence(memory_order_seq_cst)
#define dump_stack() ((void)0)
#define printk(...) ((void)0)
typedef struct { int id,priority; } XPON_DAEMON_Job_data_t;
typedef struct {
    struct { XPON_DAEMON_Job_data_t data; bool valid; } jobs[XPON_DAEMON_JOB_QUEUE_SIZE];
    uint in_index,out_index;
    pthread_mutex_t lock;
} XPON_DAEMON_Job_Queue_t;
struct system {
    struct { XPON_DAEMON_Job_Queue_t job_queue; int wq; void *task; } xpon_daemon;
} sys, *gpPonSysData=&sys;
static pthread_t thread;
static pthread_mutex_t wait_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond=PTHREAD_COND_INITIALIZER;
static atomic_bool stop,waiting;
static atomic_int jobs_seen;
static int joins;
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!pthread_mutex_lock(l)); } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(!pthread_mutex_unlock(l)); } while(0)
static bool kthread_should_stop(void) { return atomic_load(&stop); }
#define wait_event_interruptible(wq,predicate) do { \
    pthread_mutex_lock(&wait_lock); \
    while(!(predicate)) { atomic_store(&waiting,true); pthread_cond_wait(&wait_cond,&wait_lock); } \
    pthread_mutex_unlock(&wait_lock); \
} while(0)
static void wake(void) {
    pthread_mutex_lock(&wait_lock); pthread_cond_broadcast(&wait_cond); pthread_mutex_unlock(&wait_lock);
}
static int kthread_stop(void *task) {
    assert(task==&thread);
    atomic_store(&stop,true); wake();
    assert(!pthread_join(thread,NULL)); joins++; return 0;
}
static int xpon_daemon_job_dispatch(XPON_DAEMON_Job_data_t *job) {
    assert(job->id>=0 && job->id<8); atomic_fetch_add(&jobs_seen,1); return XD_SUCCESS;
}
''' + production + r'''
static void *worker(void *unused) { xpon_daemon(NULL); return NULL; }
static void start(void) {
    atomic_store(&stop,false); atomic_store(&waiting,false);
    gpPonSysData->xpon_daemon.task=&thread;
    assert(!pthread_create(&thread,NULL,worker,NULL));
}
static void await_idle(void) { while(!atomic_load(&waiting)) sched_yield(); }
int main(void) {
    assert(!pthread_mutex_init(&sys.xpon_daemon.job_queue.lock,NULL));
    xpon_daemon_quit(); assert(!joins);
    sys.xpon_daemon.task=(void *)(intptr_t)-ENOMEM;
    xpon_daemon_quit(); assert(!joins); sys.xpon_daemon.task=NULL;
    for(int n=0;n<100;n++) {
        start(); await_idle(); xpon_daemon_quit();
        assert(!sys.xpon_daemon.task); xpon_daemon_quit();
    }
    /* stop must not need space for the former synthetic QUIT job. */
    for(int n=0;n<100;n++) {
        for(int i=0;i<8;i++) {
            XPON_DAEMON_Job_data_t job={i,8-i}; xpon_daemon_job_enqueue(&job);
        }
        assert(xpon_daemon_job_dequeue_not_empty());
        start(); xpon_daemon_quit();
        XPON_DAEMON_Job_data_t left;
        while(xpon_daemon_job_dequeue(&left)) {}
        assert(!sys.xpon_daemon.task);
    }
    /* Wake an idle worker and wait for real queued work to drain. */
    atomic_store(&jobs_seen,0); start(); await_idle();
    for(int i=0;i<8;i++) {
        XPON_DAEMON_Job_data_t job={i,i}; xpon_daemon_job_enqueue(&job);
    }
    wake(); while(atomic_load(&jobs_seen)!=8) sched_yield();
    xpon_daemon_quit(); assert(joins==201);
    return 0;
}
''', flags=['-pthread'])

    def test_each_wan_interface_failure_unwinds_netdevs_and_work(self):
        production = ''.join(function(path, name) for path, name in [
            ('pwan/gpon_wan.c', 'gwan_deinit'), ('pwan/gpon_wan.c', 'gwan_init'),
            ('pwan/epon_wan.c', 'ewan_init'), ('pwan/xpon_netif.c', 'pwan_destroy'),
            ('pwan/xpon_netif.c', 'pwan_init')])
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define TCSUPPORT_WAN_GPON
#define TCSUPPORT_WAN_EPON
#define CONFIG_GPON_10G_MAX_TCONT 4
#define GPON_10G_MAX_GEM_ID 8
#define CONFIG_GPON_10G_MAX_GEMPORT 8
#define EPON_LLID_MAX_NUM 4
#define GPON_10G_UNASSIGN_ALLOC_ID 0xffff
#define GPON_GEM_IDX_MASK 0x7fff
#define XMCS_IF_ONU_TYPE_SFU 1
#define PON_MSG(...) ((void)0)
enum { PWAN_IF_OMCI, PWAN_IF_OAM, PWAN_IF_EAPOL, PWAN_IF_DATA, PWAN_IF_NUMS };
typedef int XMCSIF_OnuType_t;
typedef struct { int valid; } GWAN_GemInfo_T;
typedef struct { int rxDrop,txDrop; } EWAN_LlidInfo_T;
struct net_device_stats { int count; };
typedef struct {
    int allocId[4],gemIdToIndex[8];
    struct { GWAN_GemInfo_T info; struct net_device_stats stats; } gemPort[8];
    int gemNumbers,hgu_mode_txq,rx_omci_cnt,rx_omci_extend_cnt,gemMibTimer;
} GWAN_Priv_T;
typedef struct {
    struct { EWAN_LlidInfo_T info; struct net_device_stats stats; } llid[4];
} EWAN_Priv_T;
typedef struct { struct { int isQosUp,isTxDropOmcc; } flags; } PWAN_Config_T;
struct wan {
    GWAN_Priv_T gpon;
    EWAN_Priv_T epon;
    PWAN_Config_T devCfg;
    void *pPonNetDev[PWAN_IF_NUMS];
    int dropUnknownPackets,dropForHookBuf,activeChannelNum,greenMaxthreshold,rxLock;
} wan, *gpWanPriv=&wan;
static bool pwan_gwan_initialized;
static int clear_channel_task,live_timer,live_task,creates,fail_at,destroys,channels,channel_error;
static void spin_lock_init(int *p) { *p=1; }
static void xmcs_get_onu_type(int *p) { *p=XMCS_IF_ONU_TYPE_SFU; }
#define GPON_CREATE_TIMER(t,fn,n) do { assert(!live_timer); live_timer=1; } while(0)
#define tasklet_init(t,fn,n) do { assert(!live_task); live_task=1; } while(0)
static void timer_shutdown_sync(int *t) { assert(live_timer); live_timer=0; }
static void tasklet_kill(int *t) { assert(live_task && !live_timer); live_task=0; }
static int gwan_channel_init(void) { assert(live_task && live_timer); channels++; return channel_error; }
static int pwan_create_net_interface(int i) {
    assert(!wan.pPonNetDev[i] && wan.rxLock && !wan.devCfg.flags.isQosUp);
    if(++creates==fail_at) return -EEXIST;
    wan.pPonNetDev[i]=&wan; return 0;
}
static int pwan_delete_net_interface(int i) {
    if(!wan.pPonNetDev[i]) return -EEXIST;
    wan.pPonNetDev[i]=NULL; destroys++; return 0;
}
''' + production + r'''
static void clean(void) {
    for(int i=0;i<PWAN_IF_NUMS;i++) assert(!wan.pPonNetDev[i]);
    assert(!pwan_gwan_initialized && !live_task && !live_timer);
    pwan_destroy();
}
int main(void) {
    for(fail_at=1;fail_at<=4;fail_at++) {
        creates=destroys=channels=0;
        assert(pwan_init()==-EEXIST && creates==fail_at && destroys==fail_at-1);
        assert(channels==(fail_at>1)); clean();
    }
    fail_at=0; creates=destroys=channels=0; channel_error=-EBUSY;
    assert(pwan_init()==-EBUSY && creates==1 && destroys==1 && channels==1);
    clean(); channel_error=0;
    for(int n=0;n<2;n++) {
        fail_at=0; creates=destroys=0;
        assert(!pwan_init() && creates==4 && pwan_gwan_initialized);
        pwan_destroy(); assert(destroys==4); clean();
    }
    return 0;
}
''')

    def test_q1000k_wan_creation_defers_hardware_until_cold_transaction(self):
        production = ''.join(function('pwan/gpon_wan.c', name) for name in
                             ['gwan_deinit', 'gwan_init'])
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#define Q1000K_PON_IDENTITY
#define CONFIG_GPON_10G_MAX_TCONT 4
#define CONFIG_GPON_10G_MAX_GEMPORT 8
#define GPON_10G_MAX_GEM_ID 8
#define GPON_10G_UNASSIGN_ALLOC_ID 0xffff
#define GPON_GEM_IDX_MASK 0x7fff
#define XMCS_IF_ONU_TYPE_SFU 1
#define PWAN_IF_OMCI 0
typedef int XMCSIF_OnuType_t;
typedef struct { int valid; } GWAN_GemInfo_T;
struct net_device_stats { int count; };
typedef struct {
    int allocId[4],gemIdToIndex[8];
    struct { GWAN_GemInfo_T info; struct net_device_stats stats; } gemPort[8];
    int gemNumbers,hgu_mode_txq,rx_omci_cnt,rx_omci_extend_cnt,gemMibTimer;
} GWAN_Priv_T;
static struct { GWAN_Priv_T gpon; } wan, *gpWanPriv=&wan;
static int live_timer,live_task,netdev,create_error,clear_channel_task;
static void xmcs_get_onu_type(int *p) { *p=XMCS_IF_ONU_TYPE_SFU; }
#define GPON_CREATE_TIMER(t,fn,n) do { assert(!live_timer); live_timer=1; } while(0)
#define tasklet_init(t,fn,n) do { assert(!live_task); live_task=1; } while(0)
static void timer_shutdown_sync(int *t) { assert(live_timer); live_timer=0; }
static void tasklet_kill(int *t) { assert(live_task && !live_timer); live_task=0; }
static int pwan_create_net_interface(int i) {
    assert(i==PWAN_IF_OMCI && !netdev && live_task && live_timer);
    if(create_error) return create_error;
    netdev=1; return 0;
}
/* Native RX is still running: changing limits must fail before drain.
 * This models the real native guard, and catches the former early call.
 */
static int gwan_channel_init(void) { return -EBUSY; }
static int pwan_delete_net_interface(int i) { assert(netdev); netdev=0; return 0; }
''' + production + r'''
int main(void) {
    create_error=-ENOMEM;
    assert(gwan_init(&wan.gpon)==-ENOMEM && !netdev && !live_timer && !live_task);
    create_error=0;
    for(int n=0;n<2;n++) {
        assert(!gwan_init(&wan.gpon) && netdev && live_timer && live_task);
        assert(wan.gpon.allocId[3]==0xffff && wan.gpon.gemIdToIndex[7]==0x7fff);
        pwan_delete_net_interface(PWAN_IF_OMCI); gwan_deinit();
        assert(!netdev && !live_timer && !live_task);
    }
    return 0;
}
''')

    def test_gpon_crypto_failure_never_enables_interrupts(self):
        production = ''.join(function('gpon/gpon_init.c', name) for name in [
            'gpon_init', 'gpon_start_interrupts', 'gpon_quiesce', 'gpon_stop_work',
            'gpon_deinit'])
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define TCSUPPORT_CPU_ARMV8_64
#define PON_MSG(...) ((void)0)
#define PHY_GPON_CONFIG 1
#define XPON_RESET_HOLD_ON 1
enum { XMCS_IF_WAN_DETECT_MODE_XGPON=6, XMCS_IF_WAN_DETECT_MODE_XGSPON,
       XMCS_IF_WAN_DETECT_MODE_NGPON2_10G_10G,
       XMCS_IF_WAN_DETECT_MODE_NGPON2_10G_2G,
       XMCS_IF_WAN_DETECT_MODE_NGPON2_2G_2G };
struct system { int sysPonMode; } sys, *gpPonSysData=&sys;
struct priv {
    struct { int onuResponseTime,ploamFilter; } gponCfg;
    int silence_timer;
    struct { int TK4_timer,TK5_timer; } gponSecurity;
} priv,*gpGponPriv=&priv;
static void *g_xgpon_mac_reg_BASE;
static bool gpon_initialized,gpon_work_stopped;
static int fault,masked,crypto,act,ploam,dev,interrupts,order,timers;
static void gpon_INT_deinit(void) { masked=1; interrupts=0; }
static int gpon_init_private_data(void *p) { assert(masked); return fault==1 ? -ENODATA : 0; }
static int gpon_security_init(void) {
    assert(!crypto && !act && !ploam && !interrupts);
    if(fault==2) return -ENOMEM; crypto=1; return 0;
}
static void gponDevSetRspTime(int n) { assert(crypto); }
static int XPON_PHY_SET_MODE(int n) { assert(crypto); return fault==4 ? -ENODEV : 0; }
static void gponDevSetPloamFilterMode(int *p) { assert(crypto); }
static void ploam_init(void) { assert(crypto && !ploam); ploam=1; }
static void gpon_act_init(void) { assert(crypto && !act); act=1; }
static int gponDevMpiStop(int n) { assert(crypto && !act && !ploam); return fault==3 ? -ETIMEDOUT : 0; }
static void gpon_dev_init(void) { assert(crypto && act && ploam); dev=1; }
static void gponDevResetCtrl(int n) { assert(dev); }
static void gpon_INT_init(void) { assert(crypto && act && ploam && dev); interrupts=1; }
static void timer_shutdown_sync(int *t) { assert(crypto); timers++; }
static void gpon_act_stop_timers(void) { assert(timers==3); }
static void gpon_act_deinit(void) { assert(!interrupts && act && ++order==2); act=0; }
static void ploam_deinit(void) { assert(!interrupts && ploam && timers==3 && ++order==1); ploam=0; }
static void gpon_security_exit(void) { assert(!act && !ploam && crypto); if(fault!=3 && fault!=4) assert(++order==3); crypto=0; }
static void iounmap(void *p) { assert(0); }
''' + production + r'''
int main(void) {
    gpon_deinit(); gpon_start_interrupts(); gpon_quiesce(); assert(!masked);
    sys.sysPonMode=XMCS_IF_WAN_DETECT_MODE_XGSPON;
    for(fault=1;fault<=4;fault++) {
        assert(gpon_init()==(fault==1 ? -ENODATA : fault==2 ? -ENOMEM : fault==3 ? -ETIMEDOUT : -ENODEV));
        assert(!gpon_initialized && !crypto && !act && !ploam && !interrupts);
        gpon_deinit();
    }
    fault=0;
    for(int i=0;i<2;i++) {
        order=timers=0;
        assert(!gpon_init() && gpon_initialized && !interrupts);
        assert(gpon_init()==-EALREADY);
        gpon_start_interrupts(); assert(interrupts);
        gpon_quiesce(); assert(!interrupts);
        gpon_stop_work(); assert(order==2 && timers==3);
        gpon_start_interrupts(); assert(!interrupts);
        gpon_stop_work(); assert(order==2 && timers==3);
        gpon_deinit(); assert(order==3 && !crypto && !gpon_initialized);
        gpon_deinit(); assert(order==3);
    }
    sys.sysPonMode=0; assert(!gpon_init() && !gpon_initialized);
    gpon_start_interrupts(); assert(!interrupts);
    return 0;
}
''', flags=['-Wno-misleading-indentation'])

    def test_every_outer_failure_unwinds_only_owned_resources(self):
        production = ''.join(function('xpondrv.c', name) for name in [
            'xpon_is_ready', 'xpondrv_qdma_deinit', 'xpondrv_qdma_init',
            'xpondrv_cleanup', 'xpondrv_init'])
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define Q1000K_PON_IDENTITY
#define TCSUPPORT_CPU_EN7581
#define TCSUPPORT_CPU_ARMV8
#define GFP_KERNEL 0
#define PON_MAC_START 1
#define PON_MAC_STOP 0
#define ECNT_REGISTER_SUCCESS 0
#define XMCS_IF_WAN_DETECT_MODE_XGSPON 7
#define ECNT_QDMA_WAN 1
#define ECNT_DRIVER_API 1
#define ECNT_FE 2
#define ECNT_FE_API 2
#define QDMA_ENABLE 1
#define QDMA_LOOPBACK_DISABLE 0
#define MODULE_VERSION_10GXPONMAC "test"
#define LED_FLICKER 1
#define pr_info(...) ((void)0)
static char error_log[256];
#define pr_err(...) snprintf(error_log,sizeof(error_log),__VA_ARGS__)
#define smp_load_acquire(p) (*(p))
#define smp_store_release(p,v) (*(p)=(v))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((intptr_t)(p))
static bool xpon_ready;
static struct { bool globals,monitor,wan,mci,epon,gpon,qdma,gasp,proc,hook,api,protocol; } xpon_owned;
struct phy { int trans_status_refresh_timer,traffic_status_refresh_timer; } phy;
struct system {
    int sysMACStartup;
    struct { int xponCntRxPkt; } Omci_Oam_Monitor;
    struct { void *task; } xpon_daemon;
} system_data, *gpPonSysData;
static struct phy *gpPhyData;
#define PWAN_IF_DATA 3
static struct { void *pPonNetDev[4]; } wan_data, *gpWanPriv;
static void *gpMcsPriv,*gpGponPriv,*gpEponPriv;
static int mode=-1,fix_reg_list,xpondrv_hook_dispatch_ops;
enum { ID=1, UNION, ALLOC, ATTACH, PROTOCOL, GLOBALS, WAN, MCI, GPON, OMCI, PROC, HOOK, API, WORKER, COLD, PROTOSTART, STEPS };
static int step,fail_at,live[STEPS],irq_resources=1,providers=1;
static int ready_published,rcu_drained,tx_stopped,pipeline_error,mac_error,xpon_protocol_ops;
static bool xpon_is_ready(void); /* production definition is non-static */
static int acquire(int id) {
    assert(!xpon_ready && id==++step && !live[id]);
    if (id==fail_at) return -EIO;
    live[id]=1; return 0;
}
static void release(int id) { assert(live[id]); live[id]=0; }
static int q1000k_pon_identity_init(void) { return acquire(ID); }
static int q1000k_protocol_init(int irq,void *ops) {
    assert(irq==100 && ops==&xpon_protocol_ops); return acquire(PROTOCOL);
}
static int q1000k_protocol_status(void) { return live[PROTOCOL] ? 0 : -ENODEV; }
static int q1000k_protocol_start(void) {
    assert(xpon_ready && live[WORKER] && live[PROTOCOL] && ++step==PROTOSTART);
    if(fail_at==PROTOSTART) return -EIO;
    live[PROTOSTART]=1; return 0;
}
static void q1000k_protocol_stop(void) {
    assert(!xpon_ready && live[ALLOC]);
    if(live[PROTOSTART]) release(PROTOSTART);
    release(PROTOCOL);
}

static void *get_xpon_dev(void) { return irq_resources ? &system_data : NULL; }
static int get_xpon_irq(int n) { return irq_resources ? 100+n : -ENODEV; }
static int ecnt_hook_is_registered(int a,int b) { return providers; }
static int an7581_xpon_status(void) { return mac_error; }
static void INIT_LIST_HEAD(int *p) { assert(!xpon_ready); }
static int init_union_ic_function(void) { return acquire(UNION); }
static void *kzalloc(unsigned n,int flags) {
    assert(n==sizeof(system_data));
    if (acquire(ALLOC)) return NULL;
    memset(&system_data,0,sizeof(system_data)); return &system_data;
}
static void kfree(void *p) {
    assert(p==&system_data && rcu_drained && !xpon_ready);
    for(int i=WAN;i<STEPS;i++) assert(!live[i]);
    release(ALLOC);
}
static int xpondrv_init_global_data(void) {
    gpPhyData=&phy; gpWanPriv=&wan_data; wan_data.pPonNetDev[3]=&system_data; gpMcsPriv=gpGponPriv=gpEponPriv=&system_data;
    return acquire(GLOBALS);
}
static void omci_oam_monitor_init(void *p) { assert(live[GLOBALS]); }
static int pwan_init(void) { return acquire(WAN); }
static int xpon_mci_init(void) { return acquire(MCI); }
static int gpon_init(void) { return acquire(GPON); }
static int q1000k_omci_backend_init(void *dev) { assert(dev==&system_data); return acquire(OMCI); }
static int q1000k_omci_cold_start(void) {
    assert(xpon_ready && live[WORKER] && live[OMCI] && !live[PROTOSTART] && ++step==COLD);
    ready_published++;
    if(fail_at==COLD) return -EIO;
    live[COLD]=1; return 0;
}
static void q1000k_omci_backend_cleanup(void) {
    assert(!xpon_ready && !live[PROTOCOL]);
    if(live[COLD]) release(COLD);
    if(live[OMCI]) release(OMCI);
}
static void gpon_quiesce(void) { assert(live[GPON] && !xpon_ready); }
static void gpon_stop_work(void) { assert(live[GPON] && live[WAN] && rcu_drained); }
static int xpondrv_rx_packet(void) { return 0; }
static const char *pon_lower="qpon0";
static bool q1000k_transport_running(void) { return live[ATTACH]; }
static int q1000k_transport_start(const char *lower,int (*receive)(void)) {
    assert(!strcmp(lower,"qpon0") && receive==xpondrv_rx_packet);
    return acquire(ATTACH);
}
static int q1000k_transport_stop(void) { assert(!xpon_ready); release(ATTACH); return 0; }
static int xpon_dying_gasp_init(void) { assert(0); return -EOPNOTSUPP; }
static int xpon_proc_init(void) { return acquire(PROC); }
static int ecnt_register_hook(void *p) { return acquire(HOOK); }
static int xpon_api_init(void) { return acquire(API); }
static int xpon_daemon(void *p) { return 0; }
static void *kthread_run(void *fn,void *arg,const char *name) {
    return acquire(WORKER) ? (void *)(intptr_t)-EIO : &system_data;
}
static void gpon_start_interrupts(void) {
    assert(xpon_ready && live[WORKER]); ready_published++;
}
static void change_alarm_led_status(int s) { assert(xpon_ready); }
#define XPON_START_TIMER(t,n) assert(xpon_ready && live[WORKER])
static void ecnt_unregister_hook(void *p) { assert(!xpon_ready); release(HOOK); }
static void xpon_api_deinit(void) { assert(!xpon_ready); release(API); }
static void free_irq(int irq,void *p) { assert(0); }
static void pwan_quiesce(void) { assert(live[WAN] && !xpon_ready); tx_stopped=1; }
static void synchronize_rcu(void) { assert(!xpon_ready); rcu_drained=1; }
static void xpon_proc_dest(void) { assert(rcu_drained); release(PROC); }
static void xpon_mci_destroy(void) { assert(rcu_drained); release(MCI); }
static int stopped_timers;
static void timer_shutdown_sync(int *p) { assert(live[GLOBALS]); stopped_timers++; }
static void xpon_daemon_quit(void) {
    if(live[WORKER]) { assert(stopped_timers>=2); release(WORKER); }
    gpPonSysData->xpon_daemon.task=NULL;
}
static void stop_omci_oam_monitor(void) { assert(!live[WORKER]); }
static int q1000k_pipeline_shutdown(void) {
    assert(rcu_drained && live[ATTACH] && live[GPON] && live[WAN]);
    assert(!live[WORKER] && !live[API] && !live[HOOK] && !live[MCI] && !live[PROTOCOL]);
    return pipeline_error;
}
static void gpon_deinit(void) { assert(!live[WAN] && !live[ATTACH]); release(GPON); }
static void epon_deinit(void) { assert(0); }
static void pwan_destroy(void) { assert(rcu_drained && tx_stopped); release(WAN); }
''' + production + r'''
static void reset(int fault) {
    assert(!gpPonSysData && !gpPhyData && !gpWanPriv && !gpMcsPriv && !gpGponPriv && !gpEponPriv);
    memset(live,0,sizeof(live)); step=0; fail_at=fault; rcu_drained=0;
    stopped_timers=ready_published=tx_stopped=0;
}
static void clean(void) {
    assert(!gpPonSysData && !gpPhyData && !gpWanPriv && !gpMcsPriv && !gpGponPriv && !gpEponPriv);
    assert(!xpon_is_ready());
    for(int i=ALLOC;i<STEPS;i++) if(i!=GLOBALS) assert(!live[i]);
    typeof(xpon_owned) empty={0}; assert(!memcmp(&empty,&xpon_owned,sizeof(empty)));
    xpondrv_cleanup(); /* failed/duplicate cleanup cannot release anything twice */
}
int main(void) {
    for(int fault=1;fault<STEPS;fault++) {
        reset(fault);
        int expected=fault==ALLOC ? -ENOMEM : fault==HOOK ? -EBUSY : -EIO;
        assert(xpondrv_init()==expected && step==fault && ready_published==(fault>=COLD));
        if(fault==WAN) assert(strstr(error_log,"at wan-init: -5"));
        if(fault==COLD) assert(strstr(error_log,"at cold-start: -5"));
        clean();
        reset(0); assert(!xpondrv_init() && xpon_is_ready() && ready_published==1);
        xpondrv_cleanup(); clean(); /* complete startup and teardown after every failure */
    }
    for(pipeline_error=-1;pipeline_error>=-10;pipeline_error--) {
        reset(0); assert(!xpondrv_init()); xpondrv_cleanup(); clean();
    }
    pipeline_error=0; mac_error=-EIO;
    reset(0); assert(xpondrv_init()==-EIO && step==ID); clean(); mac_error=0;
    reset(0); irq_resources=0; assert(xpondrv_init()==-ENODEV && step==ID); clean();
    reset(0); irq_resources=1; providers=0; assert(!xpondrv_init()); xpondrv_cleanup(); clean();
    reset(0); providers=1; mode=6; assert(xpondrv_init()==-EOPNOTSUPP && step==ID); clean();
    return 0;
}
''')

    def test_cdev_failure_release_and_publication(self):
        production = ''.join(function('xmcs/xmcs_mci.c', name) for name in [
            'pon_mci_open', 'xpon_mci_destroy', 'xpon_mci_init'])
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef unsigned dev_t;
struct inode {}; struct file {};
struct kobject { int dynamic; };
struct cdev { struct kobject kobj; void *owner,*ops; } device;
struct priv {
    struct cdev *pPonMciDev;
    int xmcsWaitQueue,xmcsEventStatus,fdetLock;
    struct { int report_init_O1; } ctrlFlag;
} priv, *gpMcsPriv=&priv;
static bool xmci_region_registered,ready;
static int fault,region,allocated,published,puts,deletes;
static int owner,xmci_fops;
#define THIS_MODULE (&owner)
#define COSNT_XMCI_MAJOR_NUN 190
#define CONST_XMCI_DEV_NAME "pon"
#define MKDEV(a,b) ((a)*256+(b))
static bool xpon_is_ready(void) { return ready; }
static void init_waitqueue_head(int *p) { *p=1; }
static void spin_lock_init(int *p) { *p=1; }
static int register_chrdev_region(dev_t d,int count,const char *name) {
    assert(!region); if(fault==1) return -EBUSY; region=1; return 0;
}
static void unregister_chrdev_region(dev_t d,int count) {
    assert(region && !allocated); region=0;
}
static struct cdev *cdev_alloc(void) {
    assert(region && !allocated);
    if(fault==2) return NULL;
    allocated=1; device.kobj.dynamic=1; return &device;
}
static int cdev_add(struct cdev *d,dev_t n,int count) {
    assert(allocated && d->kobj.dynamic && d->owner==THIS_MODULE && d->ops==&xmci_fops);
    assert(priv.xmcsWaitQueue==1 && priv.fdetLock==1 && !priv.xmcsEventStatus && !priv.ctrlFlag.report_init_O1);
    if(fault==3) return -EEXIST;
    published=1; return 0;
}
static void kobject_put(struct kobject *k) {
    assert(allocated && k->dynamic && !published); allocated=0; puts++;
}
static void cdev_del(struct cdev *d) {
    assert(published && allocated && d->kobj.dynamic); published=allocated=0; deletes++;
}
''' + production + r'''
int main(void) {
    assert(pon_mci_open(NULL,NULL)==-ENODEV);
    xpon_mci_destroy();
    gpMcsPriv=NULL; assert(xpon_mci_init()==-ENODEV); xpon_mci_destroy(); gpMcsPriv=&priv;
    for(fault=1;fault<=3;fault++) {
        assert(xpon_mci_init()==(fault==1 ? -EBUSY : fault==2 ? -ENOMEM : -EEXIST));
        assert(!region && !allocated && !published && !xmci_region_registered && !priv.pPonMciDev);
        xpon_mci_destroy();
    }
    assert(puts==1 && deletes==0);
    fault=0; assert(!xpon_mci_init());
    assert(xpon_mci_init()==-EALREADY && region && published);
    ready=true; assert(!pon_mci_open(NULL,NULL)); ready=false;
    xpon_mci_destroy(); xpon_mci_destroy();
    assert(deletes==1 && !region && !allocated && !xmci_region_registered);
    assert(!xpon_mci_init()); xpon_mci_destroy();
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
