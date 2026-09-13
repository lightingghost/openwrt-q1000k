// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef u16 ushort;
typedef unsigned int uint;
#define Q1000K_PON_IDENTITY
#define Q1000K_ALLOC_ID_MAX 0x3fff
#define CONFIG_GPON_10G_MAX_TCONT 32
#define CONFIG_GPON_10G_MAX_GEMPORT 4
#define GPON_10G_UNASSIGN_ALLOC_ID 0xffff
#define GPON_UNASSIGN_ONU_ID 0x3ff
#define GPON_ONU_ID 17
#define FE_GDM_SEL_GDMA2 2
#define FE_GDM_SEL_TX 1
#define FE_ENABLE 1
#define FE_DISABLE 0
#define XMCS_EVENT_TYPE_GPON 1
#define XMCS_EVENT_GPON_TCONT_ALLOCED 2
#define PLOAM_ALLOC_ID_ASSIGN 1
#define PLOAM_ALLOC_ID_DEALLOCATE 2
#define WRITE_ONCE(p,v) ((p)=(v))
static unsigned int errors_logged;
#define pr_err(...) (errors_logged++)
typedef struct { int value; } atomic_t;
static atomic_t q1000k_tcont_config_busy;
static int atomic_cmpxchg(atomic_t *p,int old,int new) {
    int value=p->value; if(value==old) p->value=new; return value;
}
static void atomic_set_release(atomic_t *p,int value) { p->value=value; }
struct info { unsigned int portId,allocId; unsigned char valid,channel; };
struct wan {
    struct { u16 allocId[32]; struct { struct info info; } gemPort[4]; } gpon;
    unsigned int activeChannelNum;
} wan, *gpWanPriv=&wan;
typedef struct { unsigned int allocId; unsigned char allocIdType; } AllocId_Config_t;
struct XMCS_TcontCfg_S { u16 allocId; };
static int selected_channel, backend_step, fault[16], calls, events;
static bool reenter;
static uint32_t quarantined, retiring;
static u8 queue_closed[32];
static bool mac_valid[32], fe_enabled[32];
static struct { char op; int channel,value; } trace[32];
static u8 recovered[4];
int gwan_create_new_tcont(ushort id);
int gwan_remove_tcont(ushort id);
int gwan_remove_all_tcont(void);

static void record(char op,int channel,int value)
{
    assert(q1000k_tcont_config_busy.value==1 && calls<32);
    trace[calls].op=op; trace[calls].channel=channel; trace[calls++].value=value;
    if(reenter) {
        reenter=false;
        assert(gwan_create_new_tcont(900)==-EBUSY);
        assert(gwan_remove_tcont(900)==-EBUSY);
        assert(gwan_remove_all_tcont()==-EBUSY);
        reenter=true;
    }
}
static int result(void) { assert(++backend_step<16); return fault[backend_step]; }
static int gponDevEnableTCont(u16 id)
{
    record('C',selected_channel,id);
    int ret=result();
    if(ret) return ret;
    if(selected_channel>0 && selected_channel<32) mac_valid[selected_channel]=true;
    return selected_channel;
}
static int gponDevDisableTCont(u16 id)
{
    record('D',selected_channel,id);
    int ret=result();
    if(ret) return ret;
    assert(quarantined & (1u<<selected_channel));
    mac_valid[selected_channel]=false;
    return selected_channel;
}
static int q1000k_transport_set_queue_close(u8 channel,u8 closed)
{
    assert(closed==255 || !(retiring & (1u<<channel)));
    record('Q',channel,closed);
    int ret=result();
    if(!ret) queue_closed[channel]=closed;
    return ret;
}
static int q1000k_transport_quiesce_channel(u8 channel)
{
    assert(channel<32);
    retiring |= 1u<<channel;
    return q1000k_transport_set_queue_close(channel,255);
}
static int FE_API_SET_CHANNEL_ENABLE(int gdm,int direction,u8 channel,int enable)
{
    assert(gdm==2 && direction==1 && channel>0 && channel<32);
    record('F',channel,enable);
    int ret=result();
    if(!ret) fe_enabled[channel]=enable;
    return ret;
}
static void q1000k_tcont_quarantine(unsigned int channel)
{
    assert(channel<32);
    if(channel) quarantined |= 1u<<channel;
}
static int gpon_recovery_set_channel(unsigned int port,unsigned int channel)
{
    assert(port<4 && wan.gpon.allocId[channel]==0xffff);
    assert(mac_valid[channel] && fe_enabled[channel] && !queue_closed[channel]);
    record('R',channel,port); recovered[port]=channel;
    return 0;
}
static void xmcs_report_event(int type,int event,unsigned int id)
{
    assert(type==1 && event==2 && wan.gpon.allocId[selected_channel]==id);
    assert(!q1000k_tcont_config_busy.value); events++;
}

/* PRODUCTION */

static void reset_model(void)
{
    memset(&wan,0,sizeof(wan));
    for(unsigned int i=0;i<32;i++) wan.gpon.allocId[i]=0xffff;
    wan.gpon.allocId[0]=17; wan.activeChannelNum=1;
    wan.gpon.gemPort[0].info=(struct info){.portId=0,.allocId=200,.valid=1,.channel=33};
    wan.gpon.gemPort[1].info=(struct info){.portId=1,.allocId=201,.valid=1,.channel=33};
    wan.gpon.gemPort[2].info=(struct info){.portId=2,.allocId=200,.valid=1,.channel=33};
    memset(queue_closed,255,sizeof(queue_closed));
    memset(fe_enabled,0,sizeof(fe_enabled)); memset(mac_valid,0,sizeof(mac_valid));
    memset(fault,0,sizeof(fault)); memset(trace,0,sizeof(trace)); memset(recovered,33,sizeof(recovered));
    selected_channel=4; backend_step=calls=events=errors_logged=0;
    quarantined=retiring=0; reenter=false; q1000k_tcont_config_busy.value=0;
}
static void reset_trace(void) { calls=backend_step=0; memset(fault,0,sizeof(fault)); }
int main(void)
{
    struct wan before;
    reset_model(); before=wan;
    assert(gwan_create_new_tcont(0xffff)==-EINVAL);
    assert(gwan_create_new_tcont(17)==-EOPNOTSUPP);
    assert(gwan_remove_tcont(0xffff)==-EINVAL);
    assert(gwan_remove_tcont(999)==-ENOENT);
    assert(!memcmp(&wan,&before,sizeof(wan)) && !calls);
    q1000k_tcont_config_busy.value=1;
    assert(gwan_create_new_tcont(200)==-EBUSY && !calls);
    assert(gwan_remove_tcont(17)==-EBUSY && !calls);
    assert(gwan_remove_all_tcont()==-EBUSY && !calls);
    q1000k_tcont_config_busy.value=0;
    reenter=true;
    assert(!gwan_create_new_tcont(200));
    assert(!q1000k_tcont_config_busy.value && wan.activeChannelNum==2 && !events);
    assert(wan.gpon.allocId[4]==200 && wan.gpon.allocId[0]==17);
    assert(wan.gpon.gemPort[0].info.channel==4 && wan.gpon.gemPort[2].info.channel==4);
    assert(wan.gpon.gemPort[1].info.channel==33 && recovered[0]==4 && recovered[2]==4);
    assert(calls==6 && backend_step==4);
    assert(trace[0].op=='C' && trace[1].op=='Q' && trace[1].value==255);
    assert(trace[2].op=='F' && trace[2].value==1 && trace[3].op=='Q' && !trace[3].value);
    assert(trace[4].op=='R' && trace[5].op=='R');
    before=wan; reset_trace();
    assert(gwan_create_new_tcont(200)==-EEXIST && !calls && !memcmp(&wan,&before,sizeof(wan)));
    assert(gwan_remove_tcont(200)==-EOPNOTSUPP);
    assert(calls==1 && trace[0].op=='Q' && trace[0].value==255);
    assert(retiring==(1u<<4) && quarantined==(1u<<4) && queue_closed[4]==255 && mac_valid[4]);
    assert(!memcmp(&wan,&before,sizeof(wan))); /* no false removal or global FCS reset */

    for(unsigned int failure=1;failure<=4;failure++) {
        reset_model(); before=wan; fault[failure]=-ETIMEDOUT; reenter=true;
        assert(gwan_create_new_tcont(200)==-ETIMEDOUT);
        assert(!memcmp(&wan,&before,sizeof(wan)) && recovered[0]==33 && recovered[2]==33);
        assert(!q1000k_tcont_config_busy.value && !events);
        if(failure==1) assert(calls==1 && !quarantined);
        else {
            assert(calls==(int)failure+3 && quarantined==(1u<<4) && retiring==(1u<<4));
            assert(trace[failure].op=='Q' && trace[failure].value==255);
            assert(trace[failure+1].op=='F' && !trace[failure+1].value);
            assert(trace[failure+2].op=='D' && !mac_valid[4] && !fe_enabled[4]);
        }
    }
    reset_model(); before=wan;
    fault[3]=-ERANGE; fault[4]=-EIO; fault[5]=-EOPNOTSUPP; fault[6]=-ETIMEDOUT;
    assert(gwan_create_new_tcont(200)==-ERANGE && errors_logged==3);
    assert(!memcmp(&wan,&before,sizeof(wan)) && quarantined==(1u<<4) && mac_valid[4]);
    reset_model(); fault[3]=1;
    assert(gwan_create_new_tcont(200)==-EIO);
    reset_model(); selected_channel=32;
    assert(gwan_create_new_tcont(200)==-EIO && calls==1);
    reset_model(); selected_channel=0;
    assert(gwan_create_new_tcont(200)==-EIO && calls==1);

    reset_model(); wan.gpon.allocId[4]=200; wan.gpon.allocId[31]=300; before=wan;
    fault[1]=-ENODEV; fault[2]=-EIO;
    assert(gwan_remove_all_tcont()==-ENODEV && calls==3);
    assert(trace[0].channel==0 && trace[1].channel==4 && trace[2].channel==31);
    assert(quarantined==((1u<<4)|(1u<<31)) && !memcmp(&wan,&before,sizeof(wan)));
    reset_trace();
    assert(gwan_remove_all_tcont()==-EOPNOTSUPP && calls==3);
    assert(!memcmp(&wan,&before,sizeof(wan)));
    reset_trace(); fault[1]=-EAGAIN;
    assert(gwan_remove_all_tcont()==-EAGAIN && calls==3);
    assert(!memcmp(&wan,&before,sizeof(wan)));
    reset_trace(); fault[1]=-EAGAIN;
    assert(gwan_remove_tcont(200)==-EAGAIN && retiring & (1u<<4));
    assert(!memcmp(&wan,&before,sizeof(wan)));
    reset_model(); wan.gpon.allocId[0]=0xffff;
    assert(!gwan_remove_all_tcont() && !calls);

    reset_model();
    struct XMCS_TcontCfg_S cfg={200};
    AllocId_Config_t alloc={.allocId=0x10100,.allocIdType=PLOAM_ALLOC_ID_ASSIGN};
    assert(xmcs_create_tcont_info(NULL)==-EINVAL);
    assert(gponDevAssignNewAllocId(0)==-EINVAL);
    assert(gponDevAssignNewAllocId((unsigned long)&alloc)==-EINVAL && !calls);
    alloc.allocId=200; alloc.allocIdType=9;
    assert(gponDevAssignNewAllocId((unsigned long)&alloc)==-EINVAL && !calls);
    fault[1]=-EIO;
    assert(xmcs_create_tcont_info(&cfg)==-EIO && wan.activeChannelNum==1 && !events);
    reset_model(); alloc.allocIdType=PLOAM_ALLOC_ID_ASSIGN;
    assert(!gponDevAssignNewAllocId((unsigned long)&alloc) && events==1 && wan.activeChannelNum==2);
    assert(gponDevAssignNewAllocId((unsigned long)&alloc)==-EEXIST && events==1 && wan.activeChannelNum==2);
    alloc.allocIdType=PLOAM_ALLOC_ID_DEALLOCATE;
    assert(gponDevAssignNewAllocId((unsigned long)&alloc)==-EOPNOTSUPP && wan.activeChannelNum==2);
    assert(xmcs_remove_tcont_info(200)==-EOPNOTSUPP && wan.activeChannelNum==2);
    assert(xmcs_remove_tcont_info(999)==-ENOENT && wan.activeChannelNum==2);
    return 0;
}
