// SPDX-License-Identifier: GPL-2.0-only
#define __rcu
#define OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI 1
#define OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI 2
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define WARN_ON_ONCE(x) assert(!(x))
#define RCU_INIT_POINTER(p,v) ((p)=(v))
#define rcu_access_pointer(p) (p)
#define rcu_dereference(p) (p)
#define rcu_replace_pointer(p,v,condition) ({ assert(condition); __typeof__(p) old_=(p); (p)=(v); old_; })
#define kcalloc(n,z,f) calloc(n,z)
#define struct_size(p,member,n) (sizeof(*(p))+(n)*sizeof((p)->member[0]))
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define CHECKSUM_NONE 0
#define CHECKSUM_PARTIAL 1
#define ETH_P_8021Q 0x8100
#define ETH_P_8021AD 0x88a8
#define VLAN_VID_MASK 4095
#define VLAN_PRIO_SHIFT 13
struct sk_buff { unsigned int len; u8 data[100]; bool vlan; u16 tci, protocol, vlan_proto; int ip_summed; bool shared, gso; };
static bool q1000k_protocol_owned(void) { return protocol_owned; }
static u16 get_unaligned_be16(const void *p) { const u8 *b=p; return b[0]*256+b[1]; }
static int skb_copy_bits(const struct sk_buff *skb,int offset,void *out,int n) {
    if(offset<0 || n<0 || (unsigned int)(offset+n)>skb->len) return -EFAULT;
    memcpy(out,skb->data+offset,n); return 0;
}
#define skb_vlan_tag_present(skb) ((skb)->vlan)
#define skb_vlan_tag_get(skb) ((skb)->tci)
static u32 tx_word0,tx_word1;
static int transmit_count,transmit_result;
static int q1000k_transport_xmit(struct sk_buff *skb,u32 word0,u32 word1) {
    assert(!held); tx_word0=word0; tx_word1=word1; transmit_count++; return transmit_result;
}
static bool skb_shared(struct sk_buff *skb) { return skb->shared; }
static bool skb_is_gso(struct sk_buff *skb) { return skb->gso; }
static int skb_linearize(struct sk_buff *skb) { return 0; }
static int skb_checksum_help(struct sk_buff *skb) { skb->ip_summed=0; return 0; }
static int __skb_put_padto(struct sk_buff *skb,unsigned int n,bool release) {
    assert(!release && n<=100); if(skb->len<n) { memset(skb->data+skb->len,0,n-skb->len); skb->len=n; } return 0;
}
static int __vlan_insert_tag(struct sk_buff *skb,u16 protocol,u16 tag) {
    assert(skb->len+4<=100); memmove(skb->data+16,skb->data+12,skb->len-12);
    skb->data[12]=protocol>>8; skb->data[13]=protocol; skb->data[14]=tag>>8; skb->data[15]=tag; skb->len+=4; return 0;
}
static void __vlan_hwaccel_clear_tag(struct sk_buff *skb) { skb->vlan=false; }
/* SERVICES */
static void make_tag(struct sk_buff *skb,u16 vid,u8 pcp)
{
    memset(skb,0,sizeof(*skb)); skb->len=60;
    skb->data[12]=0x81; skb->data[13]=0;
    u16 tci=vid|((u16)pcp<<13); skb->data[14]=tci>>8; skb->data[15]=tci;
    skb->data[16]=8;
}
int main(void)
{
    struct omci_ani_topology topology;
    struct sk_buff skb;
    struct omci_service_config s={.cookie=1,.uni_entity_id=1,.gem_ctp_entity_id=100,
        .gem_port_id=500,.tcont_entity_id=0x8000,.alloc_id=200,.vlan_id=1894,
        .vlan_valid=true,.queue=3,.direction=3};
    struct omci_service_config rules[2];
    reset_model(); q1000k_services_init();
    assert(!q1000k_services_topology(NULL,&topology) && topology.tcont_count==31 && topology.queues_per_tcont==8);
    assert(q1000k_services_tcont(NULL,0x7fff,200,true)==-EINVAL);
    assert(!q1000k_services_tcont(NULL,0x8000,200,true));
    assert(q1000k_services_tcont(NULL,0x8001,200,true)==-EEXIST);
    assert(q1000k_services_gem(NULL,100,500,0x8000,3,true,true)==-EOPNOTSUPP);
    assert(!q1000k_services_gem(NULL,100,500,0x8000,3,true,false));
    assert(wan.gpon.gemPort[0].info.channel==33);
    assert(q1000k_services_replace(NULL,&s,1)==-ENODATA);
    assert(!gwan_create_new_tcont(200));
    assert(!q1000k_services_uni(NULL,1,true));
    assert(!q1000k_services_replace(NULL,&s,1));
    assert(queue_model[1]==(255^BIT(3)) && !qs_changing);
    make_tag(&skb,1894,0);
    assert(q1000k_services_tx(&skb)==-ENOLINK);
    q1000k_services_enable(true);
    assert(!q1000k_services_tx(&skb) && (tx_word0&7)==3 && ((tx_word0>>3)&31)==1 && (tx_word0>>14)==500);
    assert(tx_word1==(0x7f2007ff|BIT(15)));
    assert(!q1000k_services_rx(&skb,500));
    skb.vlan=true; skb.tci=1894; skb.vlan_proto=0x8100;
    assert(!q1000k_services_tx(&skb) && !skb.vlan && skb.len==64);
    make_tag(&skb,1894,0); skb.len=18; skb.ip_summed=CHECKSUM_PARTIAL;
    assert(!q1000k_services_tx(&skb) && skb.len==60 && !skb.ip_summed);
    skb.gso=true; assert(q1000k_services_tx(&skb)==-EOPNOTSUPP); skb.gso=false;
    make_tag(&skb,1895,0); assert(q1000k_services_tx(&skb)==-ENOENT);
    rules[0]=s; rules[1]=s; rules[1].cookie=2; rules[1].pcp_valid=true; rules[1].pcp=5; rules[1].queue=6;
    assert(!q1000k_services_replace(NULL,rules,2));
    make_tag(&skb,1894,5); assert(!q1000k_services_tx(&skb) && (tx_word0&7)==6);
    make_tag(&skb,1894,4); assert(!q1000k_services_tx(&skb) && (tx_word0&7)==3);
    int before=physical_ops;
    rules[1].pcp_valid=false;
    assert(q1000k_services_replace(NULL,rules,2)==-EEXIST && physical_ops==before && !qs_changing);
    rules[1].pcp_valid=true;
    physical_fail=physical_ops+1;
    assert(q1000k_services_replace(NULL,&s,1)==-ETIMEDOUT && !qs_changing);
    physical_fail=0;
    assert(!q1000k_services_tx(&skb));
    assert(q1000k_services_tcont(NULL,0x8000,0xffff,true)==-EBUSY);
    assert(!q1000k_services_uni(NULL,1,false));
    assert(q1000k_services_tx(&skb)==-ENOENT && q1000k_services_rx(&skb,500)==-ENOENT);
    assert(!q1000k_services_uni(NULL,1,true));
    assert(!q1000k_services_gem(NULL,100,0,0,0,false,false));
    assert(qs_changing && !hardware[500]);
    assert(!q1000k_services_replace(NULL,NULL,0) && !qs_changing && queue_model[1]==255);
    assert(q1000k_services_tx(&skb)==-ENOENT);
    /* Teardown follows stopped packet and protocol producers. */
    int token=q1000k_protocol_enter(); q1000k_services_destroy(); q1000k_protocol_leave(token);
    return 0;
}
