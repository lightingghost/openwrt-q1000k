// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define Q1000K_PON_IDENTITY
#define GPON_10G_STATE_O4 4
#define GPON_10G_STATE_O5 5
#define PLOAM_EQD_ABSOLUTE 1
#define PLOAM_EQD_NEGATIVE 1
#define PLOAM_BROADCAST_ADDR 0x3ff
#define PLOAM_XGSPON_BROADCAST_ADDR 0x3fe
#define PLOAM_XGSPON_BROADCAST_ADDR_NOKIA 0x7ff
static int state,onu=17,calls,error;
static bool owned,ack,absolute,negative;
static u32 value;
#define GPON_CURR_STATE state
#define GPON_ONU_ID onu
typedef struct { struct { u8 dest_id[2],eqd_value[4],eqd_mode,eqd_adjust,seq_no; } raw; } PLOAM_RAW_Ranging_Time_T;
static bool q1000k_protocol_owned(void) { return owned; }
static int q1000k_omci_ranging(u32 delay,bool abs,bool neg,u8 sequence,bool acknowledge) {
    assert(owned && sequence==87); value=delay; absolute=abs; negative=neg; ack=acknowledge; calls++; return error;
}
/* PRODUCTION */
int main(void) {
    PLOAM_RAW_Ranging_Time_T request={.raw={.dest_id={0,17},.eqd_value={0x81,0x82,0x83,0x84},.eqd_mode=1,.seq_no=87}};
    assert(ploam_recv_ranging_time(NULL)==-EINVAL);
    assert(ploam_recv_ranging_time(&request)==-EPERM && !calls);
    owned=true;
    for(int s=0;s<10;s++) {
        state=s;
        if(s==4||s==5) assert(!ploam_recv_ranging_time(&request));
        else assert(ploam_recv_ranging_time(&request)==-EINVAL);
    }
    assert(value==0x81828384 && absolute && !negative && ack);
    for(int mode=0;mode<2;mode++) {
        request.raw.eqd_mode=mode;
        for(int s=4;s<=5;s++) for(int dest=0;dest<2048;dest++) {
            state=s; request.raw.dest_id[0]=dest>>8; request.raw.dest_id[1]=dest;
            int before=calls,ret=ploam_recv_ranging_time(&request);
            bool valid=mode ? dest==17 : s==5 && (dest==17||dest==1023||dest==1022||dest==2047);
            if(valid) assert(!ret && calls==before+1 && ack==(dest==17));
            else assert(ret==-EINVAL && calls==before);
        }
    }
    state=5; request.raw.dest_id[0]=0; request.raw.dest_id[1]=17;
    request.raw.eqd_mode=0; request.raw.eqd_adjust=1;
    assert(!ploam_recv_ranging_time(&request) && negative && !absolute && ack);
    for(int i=0;i<4;i++) request.raw.eqd_value[i]=0;
    assert(!ploam_recv_ranging_time(&request) && !value && !ack);
    error=-EUCLEAN; assert(ploam_recv_ranging_time(&request)==-EUCLEAN);
    return 0;
}
