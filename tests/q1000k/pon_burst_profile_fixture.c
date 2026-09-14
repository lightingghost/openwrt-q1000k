// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define Q1000K_PON_IDENTITY
#define GPON_10G_STATE_O2_3 2
#define GPON_10G_STATE_O4 4
#define GPON_10G_STATE_O5 5
#define PLOAM_XGSPON_LINE_RATE 1
#define PLOAM_BROADCAST_ADDR 0x3ff
#define PLOAM_XGSPON_BROADCAST_ADDR 0x3fe
#define PLOAM_XGSPON_BROADCAST_ADDR_NOKIA 0x7ff
static int state,onu=17,error,calls;
static bool owned,ack;
#define GPON_CURR_STATE state
#define GPON_ONU_ID onu
typedef struct { struct { u8 dest_id[2],line_rate,prof_index,prof_version,preamble_repeat_cnt,
    preamble_lens,delimiter_lens,fec,preamble[8],delimiter[8],pon_tag[8],seq_no; } raw; } PLOAM_RAW_Profile_T;
/* PROFILE TYPE */
static struct q1000k_pon_profile saved;
static bool q1000k_protocol_owned(void) { return owned; }
static int q1000k_omci_burst_profile(const struct q1000k_pon_profile *p,const u8 tag[8],u8 sequence,bool acknowledge) {
    assert(owned && sequence==23 && tag[7]==87); saved=*p; ack=acknowledge; calls++; return error;
}
/* PRODUCTION */
int main(void) {
    PLOAM_RAW_Profile_T request={.raw={.dest_id={3,0xfe},.line_rate=1,.prof_index=3,.prof_version=15,
        .preamble_repeat_cnt=255,.preamble_lens=8,.delimiter_lens=8,.fec=1,.seq_no=23}};
    request.raw.pon_tag[7]=87; memset(request.raw.preamble,0xfe,8); memset(request.raw.delimiter,0xff,8);
    assert(ploam_recv_profile(NULL)==-EINVAL);
    assert(ploam_recv_profile(&request)==-EPERM && !calls);
    owned=true; state=1; assert(ploam_recv_profile(&request)==-EINVAL);
    state=2; request.raw.line_rate=0; assert(ploam_recv_profile(&request)==-EOPNOTSUPP);
    request.raw.line_rate=1;
    for(int address=0;address<0x800;address++) {
        request.raw.dest_id[0]=address>>8; request.raw.dest_id[1]=address;
        int before=calls,ret=ploam_recv_profile(&request);
        if(address==0x3ff || address==0x3fe || address==0x7ff) assert(!ret && !ack && calls==before+1);
        else assert(ret==-EINVAL && calls==before);
    }
    state=5; request.raw.dest_id[0]=0; request.raw.dest_id[1]=17;
    assert(!ploam_recv_profile(&request) && ack);
    assert(saved.index==3 && saved.version==15 && saved.repeat==255 && saved.preamble_len==8 && saved.delimiter_len==8);
    assert(saved.preamble[7]==0xfe && saved.delimiter[7]==0xff && saved.fec==1);
    error=-EUCLEAN; assert(ploam_recv_profile(&request)==-EUCLEAN);
    return 0;
}
