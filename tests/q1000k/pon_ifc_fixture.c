// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef unsigned long long u64;
#define BIT(n) (1UL<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define U32_MAX UINT32_MAX
#define VLAN_VID_MASK 4095
#define ETH_P_IP 0x800
#define ETH_P_IPV6 0x86dd
#define ETH_P_8021Q 0x8100
#define ETH_HLEN 14
#define ETH_ALEN 6
#define VLAN_HLEN 4
#define AIROHA_GDM2_IDX 2
#define FE_PSE_PORT_PPE1 4
#define FE_PSE_PORT_CDM2 5
#define REG_FE_IFC_PORT_EN 0x1f4
#define REG_FE_CSR_IFC_CFG 0x200
#define FE_IFC_EN_MASK 1
#define REG_PPE_DFT_CPORT(p,n) 0x244
#define DFT_CPORT_MASK(n) 0xf00
#define REG_GDM_FWD_CFG(n) 0x1500
/* TYPES */
struct airoha_eth {
    u32 regs[1536], keys[64][9][2], actions[64][6];
    unsigned int commands, fault_at, corrupt_at, enables;
    int corrupt_lane;
    u32 corrupt_bits;
    bool enabled, unsupported, gate_fault, global_fault, cpu_fault, open_fault;
    bool route_open_fault, route_close_fault;
};
struct airoha_gdm_dev {
    struct airoha_eth *eth;
    u64 pon_flow_epoch;
    bool pon_flow_fault, pon_ifc_initialized, pon_ifc_failed;
    struct airoha_pon_ingress pon_ingress[2];
};
static bool ppe_lock;
#define lockdep_assert_held(l) assert(*(l))
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=true; (f)=1; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(*(l) && (f)==1); *(l)=false; } while(0)
static bool airoha_is_7581(struct airoha_eth *eth) { return !eth->unsupported; }
static bool airoha_ppe_is_enabled(struct airoha_eth *eth,int p) { assert(!p); return eth->enabled; }
#define ether_addr_equal(a,b) (!memcmp(a,b,6))
#define ether_addr_copy(a,b) memcpy(a,b,6)
static bool is_valid_ether_addr(const u8 *a) { return !(a[0]&1) && memcmp(a,"\0\0\0\0\0\0",6); }
static u32 airoha_fe_rr(struct airoha_eth *e,u32 r) { assert(ppe_lock); return e->regs[r/4]; }
static void airoha_fe_wr(struct airoha_eth *e,u32 r,u32 v) {
    assert(ppe_lock && r<sizeof(e->regs));
    if(r==0x1f4 && e->gate_fault) return;
    if(r==0x1f4 && e->open_fault && (v&4)) return;
    if(r==0x200 && e->global_fault) return;
    if(r==0x244 && e->cpu_fault) return;
    if(r==0x1500 && e->route_open_fault && (v&0xffff)==0x4444) return;
    if(r==0x1500 && e->route_close_fault && (v&0xffff)==0x5555) return;
    if(r==0x1500 && (v&0xffff)==0x4444) {
        assert(e->regs[0x1f4/4]&4);
        assert(e->regs[0x200/4]&1);
        assert(e->actions[63][0]==0xc0000000 && e->actions[63][5]==0x500);
        assert(e->actions[0][0] || e->actions[1][0]);
    }
    if(r==0x200 && (v&1) && !(e->regs[r/4]&1))
        assert(!e->regs[0x1f4/4]); /* All inputs isolated before first enable. */
    e->regs[r/4]=v;
    if(r==0x1f4 && (v&4)) { assert(e->regs[0x200/4]&1); e->enables++; }
    if(r!=0x204) return;
    assert(v&1);
    assert(e->regs[0x200/4]&1); /* Factory initializes tables with IFC enabled. */
    unsigned int row=(v>>16)&0x1ff, word=(v>>8)&15, type=(v>>4)&7;
    assert(row<64 && !(e->regs[0x1f4/4]&4));
    assert((e->regs[0x1500/4]&0xffff)==0x5555);
    e->commands++;
    if(e->fault_at==e->commands) return;
    unsigned int count=2;
    u32 *target;
    if(type==0) { assert(word<9); target=e->keys[row][8-word]; }
    else {
        assert(type==2 && word<3);
        static const unsigned int offsets[]={5,3,0}, counts[]={2,3,1};
        target=&e->actions[row][offsets[word]]; count=counts[word];
    }
    for(unsigned int i=0;i<count;i++) {
        u32 *t=type ? target-i : target+i;
        if(v&2) *t=e->regs[0x210/4+i];
        else {
            u32 value=*t;
            /* The observed first key word returns 0x3f with mask 0xc0.
             * Exercise arbitrary don't-care bits in the remaining words
             * too, without changing the stored mask or significant key.
             */
            if(type==0 && i==0)
                value|=~e->keys[row][8-word][1] & (word==8 ? 0xff : UINT32_MAX);
            if(e->corrupt_at==e->commands && (e->corrupt_lane<0 || e->corrupt_lane==(int)i))
                value^=e->corrupt_bits;
            e->regs[0x210/4+i]=value;
        }
    }
    e->regs[0x204/4]|=0x80000000;
}
static void airoha_fe_rmw(struct airoha_eth *e,u32 r,u32 m,u32 v) { airoha_fe_wr(e,r,(airoha_fe_rr(e,r)&~m)|v); }
static void airoha_ppe_set_cpu_port(struct airoha_gdm_dev *d,unsigned int p,unsigned int f) {
    assert(p==0 && f==2); airoha_fe_rmw(d->eth,0x244,0xf00,0x500);
}
#define read_poll_timeout_atomic(fn,val,cond,delay,timeout,sl,...) ({ val=fn(__VA_ARGS__); (cond) ? 0 : -ETIMEDOUT; })
static void airoha_ppe_pon_fault(struct airoha_gdm_dev *d) { assert(ppe_lock); d->pon_flow_fault=true; }
struct sk_buff { u8 data[64]; unsigned int len; bool vlan; };
struct airoha_pon_rx_meta { u32 words[4]; u16 gem; u8 channel; bool omci,no_mic; };
#define skb_vlan_tag_present(s) ((s)->vlan)
static int skb_copy_bits(struct sk_buff *s,int o,void *d,int n) {
    if(o<0 || n<0 || (unsigned int)(o+n)>s->len) return -EINVAL;
    memcpy(d,s->data+o,n); return 0;
}
static u16 get_unaligned_be16(const void *p) { const u8 *b=p; return (u16)b[0]<<8|b[1]; }
static char messages[4096];
static void __attribute__((format(printf,1,2))) log_message(const char *format,...) {
    va_list ap; va_start(ap,format);
    vsnprintf(messages+strlen(messages),sizeof(messages)-strlen(messages),format,ap);
    va_end(ap);
}
#define netdev_info(d,...) log_message(__VA_ARGS__)
#define netdev_err(d,...) log_message(__VA_ARGS__)
#define netdev_warn(d,...) log_message(__VA_ARGS__)
#define dev_err(d,...) log_message(__VA_ARGS__)
/* PRODUCTION */
static void reset(struct airoha_eth *eth,struct airoha_gdm_dev *dev) {
    memset(eth,0,sizeof(*eth)); memset(dev,0,sizeof(*dev));
    messages[0]=0;
    eth->corrupt_lane=-1; eth->corrupt_bits=1;
    eth->enabled=true; dev->eth=eth; dev->pon_flow_epoch=9;
    eth->regs[0x1500/4]=0x07f15555;
}
/* Evaluate the programmed TCAM, independent of the encoding routine. */
static int classify(struct airoha_eth *eth,const u32 packet[9]) {
    for(unsigned int row=0;row<64;row++) {
        if(!(eth->actions[row][0]&0x80000000)) continue;
        bool match=true;
        for(unsigned int w=0;w<9;w++)
            if((packet[w]^eth->keys[row][w][0])&eth->keys[row][w][1]) match=false;
        if(match) {
            if(eth->actions[row][2]&0x10000000)
                return (eth->actions[row][5]>>8)&15;
            return eth->regs[0x1500/4]&15; /* Retain the ingress route. */
        }
    }
    return -1;
}
int main(void) {
    /* Factory-derived fields corrected by AN7581 wire calibration:
     * pon-downstream-20260924-r3, modes 12 and 18/19. The factory encoder
     * alone gave the wrong MAC split and forced-PPE action for this path.
     * Literal vectors are independent of the production encoder.
     * MAC 02:11:22:33:44:55, GEM 1023, 802.1Q VID 122, IPv4.
     */
    const u32 expected_key[9]={0,0x03ff0024,0x07a00000,0x800,0,0x02112233,0x44550000,0,4};
    const u32 expected_mask[9]={0xc0,0xffff0c3c,0xfff00000,0xffff,0,0xffffffff,0xffff0000,0,0xffffffff};
    const u32 expected_action[6]={0xc0000000,0,0,0,0,0};
    const u8 mac[6]={2,0x11,0x22,0x33,0x44,0x55};
    struct airoha_pon_flow flow={.epoch=9,.gem=1023,.vlan={122},.num_vlans=1,.rx_tci_mask=4095};
    u32 key[9],mask[9],action[6];
    airoha_pon_ifc_encode(&flow,0x800,mac,key,mask,action);
    assert(!memcmp(key,expected_key,sizeof(key)) && !memcmp(mask,expected_mask,sizeof(mask)));
    assert(!memcmp(action,expected_action,sizeof(action)));
    flow.rx_tci_mask=0xffff; flow.vlan[0]=0xb07a;
    airoha_pon_ifc_encode(&flow,0x86dd,mac,key,mask,action);
    assert(key[2]==0x07ab0000 && mask[2]==0xffff0000 && key[3]==0x86dd);
    flow.rx_tci_mask=4095; flow.vlan[0]=122;
    struct airoha_eth eth; struct airoha_gdm_dev dev;
    /* Replay the exact live pair before checking complete profile setup. */
    reset(&eth,&dev); eth.regs[0x200/4]=1; ppe_lock=true;
    const u32 captured_data[2]={0,0xc0};
    assert(!airoha_pon_ifc_word(&eth,1,8,false,captured_data,2));
    assert(eth.regs[0x210/4]==0x3f && eth.regs[0x214/4]==0xc0);
    ppe_lock=false;
    /* Every significant key bit must still fail verification when flipped.
     * Wildcard bits may vary. Every mask bit is checked, including zeros.
     * These literal masks come from the independent factory encoding above.
     */
    for(unsigned int w=0;w<9;w++) {
        const u32 data[2]={expected_key[w],expected_mask[w]};
        for(unsigned int lane=0;lane<2;lane++) {
            for(unsigned int bit=0;bit<32;bit++) {
                reset(&eth,&dev); eth.regs[0x200/4]=1; ppe_lock=true;
                eth.corrupt_at=2; eth.corrupt_lane=lane; eth.corrupt_bits=BIT(bit);
                int result=airoha_pon_ifc_word(&eth,0,8-w,false,data,2);
                assert(result==(lane || (expected_mask[w]&BIT(bit)) ? -EIO : 0));
                ppe_lock=false;
            }
        }
    }
    /* Actions are never subject to TCAM wildcard comparison. */
    static const unsigned int offsets[]={5,3,0}, counts[]={2,3,1};
    for(unsigned int w=0;w<3;w++) {
        u32 data[3]={};
        for(unsigned int lane=0;lane<counts[w];lane++) data[lane]=expected_action[offsets[w]-lane];
        for(unsigned int lane=0;lane<counts[w];lane++) {
            for(unsigned int bit=0;bit<32;bit++) {
                reset(&eth,&dev); eth.regs[0x200/4]=1; ppe_lock=true;
                eth.corrupt_at=2; eth.corrupt_lane=lane; eth.corrupt_bits=BIT(bit);
                assert(airoha_pon_ifc_word(&eth,0,w,true,data,counts[w])==-EIO);
                ppe_lock=false;
            }
        }
    }
    reset(&eth,&dev);
    eth.regs[0x1f4/4]=BIT(16); /* Native LAN may have IFC input bits enabled. */
    memset(eth.actions,0xff,sizeof(eth.actions)); /* No assumptions about reset RAM. */
    memset(eth.keys,0xff,sizeof(eth.keys));
    assert(!airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac));
    assert(eth.regs[0x1f4/4]==(BIT(16)|4));
    assert(dev.pon_ingress[0].valid && !dev.pon_ingress[1].valid && eth.enables==1);
    for(unsigned int i=0;i<9;i++) assert(eth.keys[0][i][0]==expected_key[i] && eth.keys[0][i][1]==expected_mask[i]);
    assert(!memcmp(eth.actions[0],expected_action,sizeof(expected_action)));
    /* Admission retains PPE ingress; only the final fallback forces CPU.
     * A second forced-PPE action broke TCP and lost GEM metadata on hardware.
     */
    const u32 fallback_action[6]={0xc0000000,0,0x10000000,0,0,0x500};
    assert(!memcmp(eth.actions[63],fallback_action,sizeof(fallback_action)));
    assert(eth.regs[0x1500/4]==0x07f14444);
    for(unsigned int i=1;i<63;i++) {
        assert(!eth.actions[i][0]);
        for(unsigned int w=0;w<9;w++) assert(!eth.keys[i][w][0] && !eth.keys[i][w][1]);
    }
    assert(classify(&eth,expected_key)==4);
    memcpy(key,expected_key,sizeof(key));
    key[1]^=1U<<16; assert(classify(&eth,key)==5); key[1]^=1U<<16;
    key[2]^=1U<<20; assert(classify(&eth,key)==5); key[2]^=1U<<20;
    key[1]|=1U<<4; assert(classify(&eth,key)==5); key[1]&=~(1U<<4);
    key[3]=0x86dd; assert(classify(&eth,key)==5); key[3]=0x800;
    key[5]^=1; assert(classify(&eth,key)==5); key[5]^=1;
    key[6]^=1U<<16; assert(classify(&eth,key)==5); key[6]^=1U<<16;
    key[5]=0x22334455; key[6]=0x02110000; /* Rejected old MAC layout. */
    assert(classify(&eth,key)==5);
    key[5]=0x02112233; key[6]=0x44550000;
    key[8]=2; assert(classify(&eth,key)==-1); key[8]=4;
    unsigned int commands=eth.commands;
    assert(!airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac) && eth.commands==commands);
    assert(!airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IPV6,mac) && eth.enables==2);
    unsigned int second_commands=eth.commands-commands;
    assert(!memcmp(eth.actions[1],expected_action,sizeof(expected_action)));
    key[3]=0x86dd; assert(classify(&eth,key)==4);
    flow.gem++; assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EBUSY); flow.gem--;
    struct sk_buff skb={.len=64}; memcpy(skb.data,mac,6);
    skb.data[12]=0x81; skb.data[14]=0xb0; skb.data[15]=122; skb.data[16]=8;
    struct airoha_pon_rx_meta meta={.gem=1023,.words={0,0,0x007f0092,0}};
    ppe_lock=true;
    assert(airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    meta.words[2]=0x007f0012; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    meta.words[2]=0x007f3f92; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    meta.words[2]=0x007f0192; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    skb.data[16]=0x86; skb.data[17]=0xdd;
    assert(airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    meta.words[2]=0x007f0092; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    /* Exhaust all nine-bit IDs, with and without HIT, for both families.
     * This rejects old bank-zero IDs, cross-family hits, scratch rows and
     * fallback 0x13f; only the native-slot captures 0x100/0x101 qualify.
     */
    for(unsigned int ipv6=0;ipv6<2;ipv6++) {
        skb.data[16]=ipv6 ? 0x86 : 8; skb.data[17]=ipv6 ? 0xdd : 0;
        for(unsigned int id=0;id<512;id++) {
            for(unsigned int hit=0;hit<2;hit++) {
                meta.words[2]=0x007e0012 | id<<8 | hit<<7;
                assert(airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)==
                       (hit && id==(ipv6 ? 0x101U : 0x100U)));
            }
        }
    }
    skb.data[16]=8; skb.data[17]=0;
    meta.words[2]=0x007f0092;
    meta.gem++; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); meta.gem--;
    skb.data[15]++; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); skb.data[15]--;
    skb.data[12]=0x88; skb.data[13]=0xa8; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); skb.data[12]=0x81; skb.data[13]=0;
    skb.data[16]=0x81; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); skb.data[16]=8;
    skb.data[0]^=2; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); skb.data[0]^=2;
    meta.omci=true; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); meta.omci=false;
    dev.pon_ingress[0].flow.rx_tci_mask=0xffff;
    assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta)); skb.data[14]=0;
    assert(airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    dev.pon_flow_epoch++; assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
    assert(!airoha_ppe_pon_ifc_close(&dev) && !(eth.regs[0x1f4/4]&4));
    assert(eth.regs[0x1500/4]==0x07f15555);
    assert(!dev.pon_ingress[0].valid && !dev.pon_ingress[1].valid);
    ppe_lock=false;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-ESTALE);
    flow.epoch++;
    assert(!airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac));
    /* Reopening one family must not reactivate the other family's stale rule. */
    assert(eth.actions[0][0]==0xc0000000 && !eth.actions[1][0]);
    flow.epoch=9;
    /* Timeout every command, and corrupt every read, during both initial
     * admission and admission of a second family with existing PPE flows.
     * A verified closed input must preserve PON RX/TX and upstream admission.
     */
    unsigned int fault_cases=0;
    for(unsigned int second=0;second<2;second++) {
        for(unsigned int fail=1;fail<=(second ? second_commands : commands);fail++) {
            for(unsigned int corrupt=0;corrupt<2;corrupt++) {
                if(corrupt && (fail&1)) continue;
                reset(&eth,&dev);
                eth.regs[0x1f4/4]=BIT(16);
                if(second) assert(!airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IPV6,mac));
                if(corrupt) eth.corrupt_at=eth.commands+fail;
                else eth.fault_at=eth.commands+fail;
                assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)<0);
                assert(dev.pon_ifc_failed && !dev.pon_flow_fault);
                assert(eth.regs[0x1f4/4]==BIT(16));
                assert(eth.regs[0x1500/4]==0x07f15555);
                assert(!dev.pon_ingress[0].valid && !dev.pon_ingress[1].valid);
                assert(strstr(messages,corrupt ? "PON IFC readback:" : "PON IFC command timeout:"));
                assert(strstr(messages,"stage=") && strstr(messages,"downstream remains in software"));
                unsigned int failed_commands=eth.commands;
                assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EOPNOTSUPP);
                assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IPV6,mac)==-EOPNOTSUPP);
                assert(eth.commands==failed_commands);
                ppe_lock=true;
                assert(!airoha_ppe_pon_ifc_frame(&dev,&skb,&meta));
                assert(!airoha_ppe_pon_ifc_close(&dev));
                ppe_lock=false;
                /* Consumer invalidation must not clear the sticky failure. */
                dev.pon_flow_epoch++; flow.epoch++;
                assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EOPNOTSUPP);
                flow.epoch=9;
                fault_cases++;
            }
        }
    }
    assert(fault_cases>600);
    reset(&eth,&dev); eth.global_fault=true;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EIO);
    assert(dev.pon_ifc_failed && !dev.pon_flow_fault && !eth.commands);
    assert(strstr(messages,"stage=enable-table-access"));
    reset(&eth,&dev); eth.cpu_fault=true;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EIO);
    assert(dev.pon_ifc_failed && !dev.pon_flow_fault && !(eth.regs[0x1f4/4]&4));
    assert(strstr(messages,"PON IFC CPU port:") && strstr(messages,"stage=cpu-port"));
    reset(&eth,&dev); eth.open_fault=true;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EIO);
    assert(dev.pon_ifc_failed && !dev.pon_flow_fault && !(eth.regs[0x1f4/4]&4));
    assert(strstr(messages,"stage=open-port"));
    reset(&eth,&dev); eth.route_open_fault=true;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EIO);
    assert(dev.pon_ifc_failed && !dev.pon_flow_fault);
    assert(eth.regs[0x1500/4]==0x07f15555 && !(eth.regs[0x1f4/4]&4));
    assert(strstr(messages,"stage=ppe-route"));
    reset(&eth,&dev); assert(!airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac));
    eth.route_close_fault=true;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IPV6,mac)==-EIO);
    assert(dev.pon_ifc_failed && dev.pon_flow_fault && !(eth.regs[0x1f4/4]&4));
    /* Failed isolation of another input never enables the global engine. */
    reset(&eth,&dev); eth.gate_fault=true; eth.regs[0x1f4/4]=BIT(16);
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EIO);
    assert(dev.pon_ifc_failed && !dev.pon_flow_fault && !eth.commands);
    assert(!eth.regs[0x200/4] && strstr(messages,"stage=isolate-inputs"));
    /* An unverified PON gate still requires the original hard shutdown. */
    reset(&eth,&dev); eth.gate_fault=true; eth.regs[0x1f4/4]=4;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EIO);
    assert(dev.pon_flow_fault && dev.pon_ifc_failed && !eth.commands);
    assert(strstr(messages,"PON IFC isolation failed; stopping PON traffic"));
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-ESTALE);
    reset(&eth,&dev); eth.unsupported=true;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EOPNOTSUPP && !eth.commands);
    reset(&eth,&dev); flow.num_vlans=2;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EOPNOTSUPP && !eth.commands);
    reset(&eth,&dev); flow.num_vlans=1; flow.rx_tci_mask=0;
    assert(airoha_ppe_pon_ingress(&dev,&flow,ETH_P_IP,mac)==-EOPNOTSUPP && !eth.commands);
    return 0;
}
