// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define Q1000K_PON_IDENTITY
#define WRITE_ONCE(x,v) ((x)=(v))
#define GPON_10G_STATE_O7 7
typedef union { struct { u8 dest_id[2],msg_id,seq_no,payload[44]; } raw; u32 value[13]; } PLOAM_RAW_General_T;
typedef PLOAM_RAW_General_T PLOAM_RAW_Deactivate_OnuID_T;
typedef PLOAM_RAW_General_T PLOAM_RAW_Key_Control_T;
typedef struct { struct { u8 dest_id[2],msg_id,seq_no,mode,sn[8]; } raw; } PLOAM_RAW_Disable_SN_T;
static struct { int state; bool emergencyState; struct { u8 sn[8]; } gponCfg;
    unsigned int dsPloamCounter[30]; int (*ploamRecvHandler[30])(PLOAM_RAW_General_T *);
} vendor,*gpGponPriv=&vendor;
#define GPON_CURR_STATE vendor.state
#define GPON_ONU_ID 17
static bool owned;
static int auth_error, auth_calls, handler_error, handler_calls, resets, reset_error, fault;
static bool emergency;
static u32 q1000k_ploam_rejected;
static int q1000k_ploam_last_error;
static bool q1000k_protocol_owned(void) { return owned; }
static void q1000k_protocol_fail(int error) { if(error<0) fault=error; }
static int q1000k_omci_ploam_verify(const u8 *message,size_t length) {
    assert(owned && message && length==48); auth_calls++; return auth_error;
}
static int handle(PLOAM_RAW_General_T *message) {
    assert(owned && !auth_error && auth_calls && !(message->raw.dest_id[0]&0xfc)); handler_calls++; return handler_error;
}
static int q1000k_omci_reset(bool disabled,bool phy) {
    assert(owned && !phy); resets++; emergency=disabled;
    if(!reset_error) vendor.emergencyState=disabled;
    return reset_error;
}
/* No raw MMIO, PHY TX, reboot or task scheduling functions exist here. */
/* PRODUCTION */
int main(void)
{
    PLOAM_RAW_General_T message={};
    PLOAM_RAW_Disable_SN_T sn={.raw={.dest_id={3,255}}};
    const int supported[]={1,3,4,5,6,9,10};
    assert(ploam_parser_down_message(NULL)==-EPERM);
    owned=true;
    assert(ploam_parser_down_message(NULL)==-EINVAL && !auth_calls);
    gpGponPriv=NULL;
    assert(ploam_parser_down_message(&message)==-EINVAL && !auth_calls);
    gpGponPriv=&vendor;
    for(unsigned int i=0;i<30;i++) vendor.ploamRecvHandler[i]=handle;
    for(unsigned int id=0;id<256;id++) {
        bool allowed=false;
        for(unsigned int i=0;i<sizeof(supported)/sizeof(supported[0]);i++) allowed|=id==(unsigned int)supported[i];
        message.raw.msg_id=id; message.raw.dest_id[0]=0xff;
        int before=handler_calls;
        auth_error=-EBADMSG;
        assert(ploam_parser_down_message(&message)==-EBADMSG && handler_calls==before && !fault);
        auth_error=0;
        int ret=ploam_parser_down_message(&message);
        assert(ret==(allowed ? 0 : -EOPNOTSUPP));
        assert(handler_calls==before+allowed && !fault);
    }
    message.raw.msg_id=1;
    for(int ret=-1;ret>=-133;ret--) {
        handler_error=ret; fault=0;
        assert(ploam_parser_down_message(&message)==ret);
        assert(q1000k_ploam_last_error==ret);
        assert(fault==((ret==-EIO||ret==-ENODEV||ret==-ETIMEDOUT||ret==-EUCLEAN) ? ret : 0));
    }
    handler_error=0; fault=0;
    vendor.ploamRecvHandler[1]=NULL;
    assert(ploam_parser_down_message(&message)==-EOPNOTSUPP);
    assert(q1000k_ploam_rejected);
    auth_error=-EIO;
    int before=handler_calls;
    assert(ploam_parser_down_message(&message)==-EIO && fault==-EIO && handler_calls==before);
    assert(ploam_recv_key_control(NULL)==-EOPNOTSUPP);
    gponDevUnicastKeyExchange(0); assert(fault==-EOPNOTSUPP);

    assert(ploam_recv_deactivate_onu(NULL)==-EINVAL);
    assert(ploam_recv_disable_serial_number(NULL)==-EINVAL);
    owned=false;
    assert(ploam_recv_deactivate_onu(&message)==-EPERM);
    assert(ploam_recv_disable_serial_number(&sn)==-EPERM && !resets);
    owned=true; vendor.state=5;
    for(unsigned int dest=0;dest<2048;dest++) {
        message.raw.dest_id[0]=dest>>8; message.raw.dest_id[1]=dest;
        int old=resets, ret=ploam_recv_deactivate_onu(&message);
        if(dest==17||dest==1023) assert(!ret && resets==old+1 && !emergency);
        else assert(ret==-EINVAL && resets==old);
    }
    sn.raw.mode=PLOAM_DISABLE_DENIED_ALL;
    assert(!ploam_recv_disable_serial_number(&sn) && emergency && vendor.emergencyState && vendor.state==5);
    message.raw.dest_id[0]=0; message.raw.dest_id[1]=17;
    before=resets;
    assert(!ploam_recv_deactivate_onu(&message) && resets==before && vendor.emergencyState);
    /* Enable in the same receive batch cancels the pending policy, while
     * the deferred reset still owns state publication and TX remains off.
     */
    sn.raw.mode=PLAOM_DISABLE_ALLOWED_ALL;
    assert(!ploam_recv_disable_serial_number(&sn) && !emergency && !vendor.emergencyState && vendor.state==5);
    before=resets;
    assert(!ploam_recv_disable_serial_number(&sn) && resets==before);
    vendor.state=7; vendor.emergencyState=true;
    assert(!ploam_recv_disable_serial_number(&sn) && !emergency && !vendor.emergencyState && vendor.state==7);
    vendor.state=5;
    for(unsigned int mode=0;mode<256;mode++) {
        sn.raw.mode=mode; sn.raw.sn[7]=1;
        before=resets;
        int ret=ploam_recv_disable_serial_number(&sn);
        if(mode==0||mode==255) assert(!ret && resets==before); /* Other serial. */
        else if(mode==15||mode==240) assert(!ret);
        else assert(ret==-EOPNOTSUPP && resets==before);
    }
    sn.raw.sn[7]=0; sn.raw.mode=PLOAM_DISABLE_DENIED_SPECIFIC;
    assert(!ploam_recv_disable_serial_number(&sn) && emergency);
    sn.raw.mode=PLAOM_DISABLE_ALLOWED_SPECIFIC; reset_error=-EIO;
    assert(ploam_recv_disable_serial_number(&sn)==-EIO && vendor.emergencyState);
    return 0;
}
