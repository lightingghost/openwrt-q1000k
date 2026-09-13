// SPDX-License-Identifier: GPL-2.0-only
/* Real TX/RX mapping consumers and registry. Protocol hooks are fixtures. */
#define TCSUPPORT_PON_MAC_FILTER
#define PWAN_IF_DATA 0
#define PWAN_IF_OMCI 1
#define BROADCAST_OFFSET 2
#define MULTICAST_OFFSET 4
#define UNKNOWN_UNICAST_OFFSET 8
#define TXMSG_FPORT_GMAC 2
#define XPON_SUCCESS 0
#define XGPON_SW 1
#define GPON_OMCI_IK_IDX0 0
#define GPON_OMCI_IK_IDX1 1
#define QOS_TSE_MARK 0x100
#define QOS_TSID_MARK 0xff
#define DS_PKT_FORM_WAN 0x200
#define PKT_SEND_TO_WAN 1
#define PKT_SEND_TO_LAN 2
#define UP_STREAM 0
#define XPON_DROP_PRINT ((void)0)
#define PON_MSG(...) ((void)0)
#define FH_VLAN_TX_TRANS(s) ((void)0)
#define FH_VLAN_TX_PROC(s) ((void)0)
struct sk_buff {
    unsigned char data[64];
    struct { u16 gem_port,v_if; u8 gem_type; uint pon_mark,pon_mac_filter_flag,pon_tag_num; } cb;
};
#define XPON_SKB_CB(s) (&(s)->cb)
struct port_info { u8 tsid; };
static struct { struct { int usOmciMicCtrl; } gponCfg; struct { int omciIkIdx; } gponSecurity; } gpon,*gpGponPriv=&gpon;
static int omciIkIdxExchange,dropCpuTxPktsFlag,up_error,down_error,mic_error;
static unsigned int classified_gem=500,filter_action,down_calls;
typedef struct { struct { uint fport,oam,channel,nboq,gem,ndp,mic_idx,mtr_g,acnt_g0,acnt_g1; } raw; } PWAN_FETxMsg_T;
typedef struct { struct { uint crcer,oam,no_mic,gem,ppe; } raw; } PWAN_FERxMsg_T;
static int remove_omci_crc_if_exist(struct sk_buff *s) { return mic_error; }
static int gwan_add_us_omci_mic(struct sk_buff *s) { return mic_error; }
static int gwan_check_ds_omci_mic(struct sk_buff *s) { return mic_error; }
static void gwan_check_ds_omci_type(struct sk_buff *s) {}
static int isBroadcastPkt(unchar *data) { return -1; }
static int isMulticastPkt(unchar *data) { return -1; }
static int isUnknownUnicastPkt(unchar *data) { return -1; }
static int ECNT_API_GPON_FLOW_UPSTREAM_ANI_HOOK(struct sk_buff *s) {
    assert(!held); s->cb.gem_port=classified_gem; return up_error;
}
static int ECNT_API_GPON_FLOW_DOWNSTREAM_ANI_HOOK(struct sk_buff *s) {
    assert(!held && s->cb.gem_port==500 && s->cb.v_if==7); down_calls++; return down_error;
}
static void FE_API_GET_METER_IDX(struct sk_buff *s,int direction,u8 *out,uint idx) { assert(!held); *out=127; }
static void FE_API_GET_ACNT0_IDX(struct sk_buff *s,int direction,u8 *out) { assert(!held); *out=31; }
static void FE_API_GET_ACNT1_IDX(struct sk_buff *s,int direction,u8 *out) { assert(!held); *out=31; }
static int filter(struct sk_buff *s) {
    assert(!held);
    if(filter_action==1) s->cb.gem_port=501;
    if(filter_action==2) s->cb.v_if=8;
    if(filter_action==3) assert(gwan_remove_gemport(500)==-EOPNOTSUPP);
    return 0;
}
static int (*pon_check_mac_hook)(struct sk_buff *)=filter;
/* PACKET PRODUCTION */
int main(void)
{
    PWAN_FETxMsg_T tx={0}; PWAN_FERxMsg_T rx={0};
    struct sk_buff skb={0}; struct port_info info={0}; u8 flag=0;
    reset_model(); create_ready(500,4,200,7);
    assert(!gwan_prepare_tx_message(&tx,0,&skb,0,&info));
    assert(tx.raw.gem==500 && tx.raw.channel==4 && tx.raw.nboq==4 && skb.cb.v_if==7);
    assert(tx.raw.fport==2 && !tx.raw.oam && !tx.raw.ndp && tx.raw.mtr_g==127);
    filter_action=1; assert(gwan_prepare_tx_message(&tx,0,&skb,0,&info)==-ESTALE);
    filter_action=2; assert(gwan_prepare_tx_message(&tx,0,&skb,0,&info)==-ESTALE);
    filter_action=0; classified_gem=65535; assert(gwan_prepare_tx_message(&tx,0,&skb,0,&info)==-ENOENT);
    classified_gem=0; assert(gwan_prepare_tx_message(&tx,0,&skb,0,&info)==-ENOENT);
    classified_gem=500; up_error=1; assert(gwan_prepare_tx_message(&tx,0,&skb,0,&info)<0); up_error=0;
    filter_action=3;
    /* In-flight metadata remains tied to the old binding. The real native
     * adapter rejects submission to the permanently closed channel.
     */
    assert(!gwan_prepare_tx_message(&tx,0,&skb,0,&info));
    assert(tx.raw.gem==500 && tx.raw.channel==4 && closed==BIT(4));
    assert(gwan_prepare_tx_message(&tx,0,&skb,0,&info)==-ENOENT);
    reset_model(); create_ready(500,4,200,7); filter_action=0; rx.raw.gem=500;
    assert(!gwan_process_rx_message(&rx,&skb,60,&flag));
    assert(skb.cb.gem_port==500 && skb.cb.v_if==7 && !skb.cb.gem_type && !flag && down_calls==1);
    assert(wan.gpon.gemPort[0].stats.rx_packets==1 && wan.gpon.gemPort[0].stats.rx_bytes==60);
    rx.raw.gem=65535; assert(gwan_process_rx_message(&rx,&skb,60,&flag)==-ENOENT && down_calls==1);
    rx.raw.gem=500; down_error=1; assert(gwan_process_rx_message(&rx,&skb,60,&flag)<0); down_error=0;
    filter_action=3;
    assert(!gwan_process_rx_message(&rx,&skb,60,&flag) && closed==BIT(4));
    assert(gwan_process_rx_message(&rx,&skb,60,&flag)==-ENOENT);
    /* OMCI accounting must tolerate an unassigned GEM index without OOB. */
    rx.raw.oam=1; rx.raw.no_mic=0;
    assert(gwan_process_rx_message(&rx,&skb,48,&flag)==PWAN_IF_OMCI);
    return 0;
}
