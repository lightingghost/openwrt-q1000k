// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint8_t unchar;
typedef uint16_t u16;
typedef int XPON_Mode_t;
typedef int XGPON_SW_HW_SELECT_T;
#define Q1000K_PON_IDENTITY
#define GPON_10G_STATE_O5 5
typedef struct { unsigned char bytes[128]; } GPON_Security_t;
static struct { GPON_Security_t gponSecurity; struct { u8 reg_id[36]; } gponCfg; } priv,*gpGponPriv=&priv;
static unsigned int state=5,onu=17;
#define GPON_CURR_STATE state
#define GPON_ONU_ID onu
typedef struct { struct { u8 dest_id[2],seq_no; } raw; } PLOAM_RAW_Request_Registration_T;
struct XGMCS_XgponMsk_S { u8 key[16]; };
struct XGMCS_XgponBroadcast_Key_S { u8 key[16]; };
struct XMCS_OMCI_BROADCAST_KEY_S { u8 key[16]; };
typedef struct { unsigned int dsOmciMicMode,usOmciMicMode; } GPON_10G_DEV_OMCI_MIC_CTRL_T;
static bool owned=true;
static int fault,key_error,key_checks,replies;
static bool q1000k_protocol_owned(void) { return owned; }
static int q1000k_protocol_status(void) { return fault; }
static void q1000k_protocol_fail(int error) { assert(error<0); fault=error; }
static int q1000k_omci_registration_keys(void) { key_checks++; return key_error; }
static void ploam_send_registration_msg(u8 seq,const u8 *id) { assert(seq==23 && id==priv.gponCfg.reg_id); replies++; }
/* PRODUCTION */
int main(void)
{
    memset(&priv,0xa5,sizeof(priv)); GPON_Security_t saved=priv.gponSecurity;
    PLOAM_RAW_Request_Registration_T request={.raw={.dest_id={0,17},.seq_no=23}};
    assert(ploam_recv_request_registration(NULL)==-EINVAL && !key_checks);
    owned=false; assert(ploam_recv_request_registration(&request)==-EPERM); owned=true;
    state=4; assert(ploam_recv_request_registration(&request)==-EINVAL); state=5;
    onu=18; assert(ploam_recv_request_registration(&request)==-EINVAL); onu=17;
    assert(!key_checks && !replies);
    key_error=-EKEYREJECTED;
    assert(ploam_recv_request_registration(&request)==-EKEYREJECTED && !replies);
    gpon_key_index_change_by_hw(&priv.gponSecurity); assert(fault==-EKEYREJECTED);
    fault=0; key_error=0;
    for(int i=0;i<3;i++) assert(!ploam_recv_request_registration(&request));
    assert(replies==3 && !memcmp(&saved,&priv.gponSecurity,sizeof(saved)));
    gpon_key_index_change_by_hw(NULL); assert(fault==-EINVAL);
    fault=0; gpon_key_index_change_by_hw(&priv.gponSecurity); assert(!fault);
    assert(xmcs_set_msk(NULL)==-EOPNOTSUPP);
    assert(xmcs_set_broadcast_key(NULL)==-EOPNOTSUPP);
    assert(xmcs_set_omci_broadcast_key(NULL)==-EOPNOTSUPP);
    assert(xmcs_set_omci_mic_ctrl(NULL)==-EOPNOTSUPP);
    gpon_ploamIk_index_change_by_OMCI_base_secure(&priv.gponSecurity); assert(fault==-EOPNOTSUPP);
    gpon_omciIk_index_change_by_OMCI_base_secure(&priv.gponSecurity); assert(fault==-EOPNOTSUPP);
    assert(!memcmp(&saved,&priv.gponSecurity,sizeof(saved)));
    return 0;
}
