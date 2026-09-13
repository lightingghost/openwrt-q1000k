// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define Q1000K_PON_IDENTITY
#define XPON_CHANNEL_NUMBER 32
#define CONFIG_GPON_10G_MAX_TCONT 32
#define CONFIG_QDMA_QUEUE 8
#define XPON_QUEUE_NUMBER 8
#define QDMA_TXQOS_TYPE_WRR 0
#define QDMA_TXQOS_TYPE_SP 1
#define QDMA_TXQOS_TYPE_SPWRR7 2
#define QDMA_TXQOS_TYPE_SPWRR6 3
#define QDMA_TXQOS_TYPE_SPWRR5 4
#define QDMA_TXQOS_TYPE_SPWRR4 5
#define QDMA_TXQOS_TYPE_SPWRR3 6
#define QDMA_TXQOS_TYPE_SPWRR2 7
#define XMCS_IF_QOS_TYPE_WRR 0
#define XMCS_IF_QOS_TYPE_SP 1
#define XMCS_IF_QOS_TYPE_SPWRR7 2
#define XMCS_IF_QOS_TYPE_SPWRR6 3
#define XMCS_IF_QOS_TYPE_SPWRR5 4
#define XMCS_IF_QOS_TYPE_SPWRR4 5
#define XMCS_IF_QOS_TYPE_SPWRR3 6
#define XMCS_IF_QOS_TYPE_SPWRR2 7
struct XMCS_ChannelQoS_S { uint8_t channel; int qosType; struct { uint8_t weight; } queue[8]; };
typedef struct { uint8_t channel; int qosType; struct { uint16_t weight; } queue[8]; } QDMA_TxQosScheduler_T;
struct airoha_pon_qos { uint16_t weights[8]; uint8_t mode; bool byte_mode,scale16; };
static struct airoha_pon_qos native;
static int get_error,set_error,get_calls,set_calls;
static int q1000k_transport_get_qos(uint8_t c,struct airoha_pon_qos *out)
{
    assert(c==31); get_calls++;
    if(get_error) return get_error;
    *out=native; return 0;
}
static int q1000k_transport_set_qos(uint8_t c,const struct airoha_pon_qos *in)
{
    assert(c==31); set_calls++;
    if(set_error) return set_error;
    native=*in; return 0;
}
/* PRODUCTION */
int main(void)
{
    struct XMCS_ChannelQoS_S cfg={.channel=31},result,saved;
    int errors[]={-ENODEV,-EBUSY,-EAGAIN,-EIO,-ETIMEDOUT,-ESHUTDOWN};
    unsigned int m,q,e;
    for(m=0;m<8;m++) {
        cfg.qosType=m; native.byte_mode=true; native.scale16=true;
        for(q=0;q<8;q++) cfg.queue[q].weight=q+10;
        assert(!xmcs_set_channel_scheduler(&cfg));
        assert(native.mode==m && native.byte_mode && native.scale16);
        result=cfg;
        assert(!xmcs_get_channel_scheduler(&result));
        assert(result.qosType==cfg.qosType);
        for(q=0;q<8;q++) assert(result.queue[q].weight==cfg.queue[q].weight);
    }
    for(e=0;e<sizeof(errors)/sizeof(errors[0]);e++) {
        get_error=errors[e]; set_calls=0; saved=cfg;
        assert(xmcs_set_channel_scheduler(&cfg)==errors[e] && !set_calls);
        assert(xmcs_get_channel_scheduler(&cfg)==errors[e]);
        assert(!memcmp(&cfg,&saved,sizeof(cfg)));
        get_error=0; set_error=errors[e];
        assert(xmcs_set_channel_scheduler(&cfg)==errors[e]);
        set_error=0;
    }
    for(q=0;q<8;q++) {
        uint16_t old=native.weights[q];
        native.weights[q]=256; saved=cfg;
        assert(xmcs_get_channel_scheduler(&cfg)==-ERANGE);
        assert(!memcmp(&cfg,&saved,sizeof(cfg)));
        native.weights[q]=old;
    }
    cfg.channel=32; get_calls=set_calls=0;
    assert(xmcs_set_channel_scheduler(&cfg)==-EINVAL);
    assert(xmcs_get_channel_scheduler(&cfg)==-EINVAL && !get_calls && !set_calls);
    cfg.channel=31; cfg.qosType=8;
    assert(xmcs_set_channel_scheduler(&cfg)==-EINVAL && !get_calls && !set_calls);
    return 0;
}
