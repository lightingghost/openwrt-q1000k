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
#define htons(x) (x)
#define ntohs(x) (x)
#define skb_reset_mac_header(skb) ((void)(skb))
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
static int __skb_vlan_pop(struct sk_buff *skb,u16 *tci) {
    if(skb->len<18) return -EMSGSIZE;
    *tci=get_unaligned_be16(skb->data+14);
    memmove(skb->data+12,skb->data+16,skb->len-16); skb->len-=4;
    skb->protocol=get_unaligned_be16(skb->data+12); return 0;
}
static bool fixture_fix_vlans;
static int q1000k_pon_fix_vlans(void) { return fixture_fix_vlans; }
#undef q1000k_trace
#define QT_OMCI_GEM 28
static struct { int error; u32 a,b,c,d; } gem_trace[2];
static void fixture_gem_trace(unsigned int event,unsigned int id,int error,u32 a,u32 b,u32 c,u32 d) {
    assert(event==28 && id>=1 && id<=2);
    gem_trace[id-1]=(typeof(gem_trace[0])){error,a,b,c,d};
}
#define q1000k_trace(...) fixture_gem_trace(__VA_ARGS__)
/* SERVICES */
static void make_tag(struct sk_buff *skb,u16 vid,u8 pcp)
{
    memset(skb,0,sizeof(*skb)); skb->len=60;
    skb->data[12]=0x81; skb->data[13]=0;
    u16 tci=vid|((u16)pcp<<13); skb->data[14]=tci>>8; skb->data[15]=tci;
    skb->data[16]=8;
}
static void test_deferred_gem(void)
{
    struct omci_gem_port_config down={.port_id=550,.tcont_entity_id=0xffff,.direction=2,
        .qos={.upstream_queue=0x1234,.upstream_descriptor=0x4567,
              .downstream_queue=0xffff,.downstream_descriptor=0xffff}};
    struct omci_gem_port_config up={.port_id=600,.tcont_entity_id=0x8000,.direction=3,
        .qos={.upstream_queue=0x8003}};
    struct q1000k_gwan_binding binding;
    struct q1000k_gwan_table before,after;
    struct sk_buff skb;
    reset_model(); q1000k_services_init();
    assert(!q1000k_services_gem_config(NULL,99,&down,true));
    assert(hardware[550] && qs_alloc[0]==0xffff);
    assert(!q1000k_gwan_snapshot(&before));
    assert(before.gem[0].alloc_id==0xffff && before.gem[0].channel==33);
    assert(q1000k_gwan_binding(550,true,&binding)==-ENODATA);
    assert(q1000k_gwan_binding(550,false,&binding)==-ENODATA);
    /* Upstream-only fields are ignored in direction 2; downstream QoS and
     * unsupported broadcast key rings are still rejected without changes. */
    down.qos.downstream_queue=1;
    assert(q1000k_services_gem_config(NULL,99,&down,true)==-EOPNOTSUPP);
    down.qos.downstream_queue=0xffff; down.encryption_key_ring=2;
    assert(q1000k_services_gem_config(NULL,99,&down,true)==-EOPNOTSUPP);
    down.encryption_key_ring=0;
    assert(!q1000k_gwan_snapshot(&after) && !memcmp(&before,&after,sizeof(before)));
    /* No allocation: creation succeeds, but never opens an upstream queue. */
    assert(!q1000k_services_gem_config(NULL,100,&up,true));
    assert(gem_trace[0].a==((100U<<16)|600) && gem_trace[0].b==((0x8000U<<16)|0x300));
    assert(gem_trace[0].c==0xffff && gem_trace[0].d==3);
    assert(gem_trace[1].b==0x80030000 && !gem_trace[1].error);
    assert(hardware[600] && qs_alloc[0]==0xffff);
    assert(q1000k_gwan_binding(600,true,&binding)==-ENODATA);
    assert(!q1000k_gwan_snapshot(&before)); after=before;
    after.gem[1].channel=1;
    int ops=physical_ops;
    assert(q1000k_gwan_apply(&before,&after)==-EINVAL && physical_ops==ops);
    assert(q1000k_services_gem(NULL,101,601,0xffff,3,true,0)==-EINVAL);
    /* OMCI allocation followed by PLOAM assignment completes the binding. */
    assert(!q1000k_services_tcont(NULL,0x8000,200,true));
    assert(q1000k_gwan_binding(600,true,&binding)==-ENODATA);
    assert(!gwan_create_new_tcont(200));
    assert(!q1000k_gwan_binding(600,true,&binding) && binding.alloc_id==200 && binding.channel==1);
    assert(q1000k_gwan_binding(550,true,&binding)==-ENODATA);
    assert(!q1000k_services_replace(NULL,NULL,0));
    q1000k_services_enable(true); make_tag(&skb,1894,0);
    assert(q1000k_services_tx(&skb)==-ENOENT && queue_model[1]==255);
    /* Deallocation retains the GEM, revokes binding, and permits rebinding. */
    assert(!q1000k_services_tcont(NULL,0x8000,0xffff,true));
    assert(hardware[600] && q1000k_gwan_binding(600,true,&binding)==-ENODATA);
    assert(!q1000k_services_tcont(NULL,0x8000,200,true));
    assert(!q1000k_gwan_binding(600,true,&binding) && binding.channel==1);
    assert(!q1000k_services_gem_config(NULL,100,&up,false));
    assert(gem_trace[0].c==200 && gem_trace[0].d==2);
    assert(!q1000k_services_gem_config(NULL,99,&down,false));
    assert(!hardware[600] && !hardware[550]);
    int token=q1000k_protocol_enter(); q1000k_services_destroy(); q1000k_protocol_leave(token);
}

int main(void)
{
    struct omci_priority_queue_config q={.configuration=1,.maximum_size=0xffff,
        .tcont_entity_id=0x8000,.priority=4,.scheduler_entity_id=0x8000,
        .weight=17,.backpressure_occur=0xffff};
    struct omci_traffic_scheduler_config scheduler={.tcont_entity_id=0x8000,.policy=2};
    struct omci_ani_topology topology;
    struct sk_buff skb;
    struct omci_service_config s={.cookie=1,.uni_entity_id=1,.gem_ctp_entity_id=100,
        .gem_port_id=500,.tcont_entity_id=0x8000,.alloc_id=200,.vlan_id=1894,
        .vlan_valid=true,.queue=3,.direction=3};
    struct omci_service_config rules[2];
    test_deferred_gem();
    reset_model(); q1000k_services_init();
    assert(!q1000k_services_topology(NULL,&topology) && topology.tcont_count==31 && topology.queues_per_tcont==8);
    assert(q1000k_services_tcont(NULL,0x7fff,200,true)==-EINVAL);
    int untouched=physical_ops;
    assert(!q1000k_services_queue(NULL,0x8003,&q));
    assert(!q1000k_services_scheduler(NULL,0x8000,&scheduler));
    assert(physical_ops==untouched && qs_schedulers[0].weight[3]==17);
    assert(!q1000k_services_tcont(NULL,0x8000,200,true));
    assert(q1000k_services_tcont(NULL,0x8001,200,true)==-EEXIST);
    /* The adapter must see every GEM QoS pointer before programming a GEM.
     * Failed updates and profile reconciliation retain the installed intent.
     */
    {
        struct omci_gem_port_config c={.port_id=500,.tcont_entity_id=0x8000,.direction=3};
        struct omci_gem_qos rejected[]={
            {.upstream_descriptor=1}, {.downstream_descriptor=0x1234},
            {.downstream_queue=1}, {.traffic_management_option=1},
            {.traffic_management_option=255}, {.upstream_queue=0x7fff},
            {.upstream_queue=0x8008}, {.upstream_queue=0x80f8},
        };
        int ops=physical_ops;
        assert(q1000k_services_gem_config(NULL,100,NULL,true)==-EINVAL);
        for(unsigned int j=0;j<sizeof(rejected)/sizeof(rejected[0]);j++) {
            int expected=j<5 ? -EOPNOTSUPP : -EINVAL;
            c.qos=rejected[j];
            assert(q1000k_services_gem_config(NULL,100,&c,true)==expected);
            rules[0]=s; rules[0].gem_qos=c.qos;
            assert(q1000k_services_replace(NULL,rules,1)==expected);
            assert(physical_ops==ops && !hardware[500] && !qs_gems[0].valid);
        }
        c.qos=(struct omci_gem_qos){.upstream_queue=0x8003,
            .upstream_descriptor=0xffff,.downstream_queue=0xffff,
            .downstream_descriptor=0xffff,.traffic_management_option=2};
        assert(!q1000k_services_gem_config(NULL,100,&c,true));
        rules[0]=s; rules[0].gem_qos=c.qos;
        assert(!q1000k_services_replace(NULL,rules,1));
        struct qs_rules *installed=qs_current;
        ops=physical_ops;
        for(unsigned int j=0;j<sizeof(rejected)/sizeof(rejected[0]);j++) {
            int expected=j<5 ? -EOPNOTSUPP : -EINVAL;
            c.qos=rejected[j];
            assert(q1000k_services_gem_config(NULL,100,&c,true)==expected);
            rules[0].gem_qos=c.qos;
            assert(q1000k_services_replace(NULL,rules,1)==expected);
            assert(physical_ops==ops && qs_current==installed && qs_gems[0].valid);
        }
        rules[0].gem_qos=(struct omci_gem_qos){.upstream_queue=0x8004};
        assert(q1000k_services_replace(NULL,rules,1)==-EINVAL && physical_ops==ops);
        /* Even a now-unsupported candidate can be removed. */
        assert(!q1000k_services_gem_config(NULL,100,&c,false));
        assert(!hardware[500] && !qs_gems[0].valid);
        assert(!q1000k_services_replace(NULL,NULL,0));
    }
    assert(q1000k_services_gem(NULL,100,500,0x8000,3,true,2)==-EOPNOTSUPP);
    assert(!q1000k_services_gem(NULL,100,500,0x8000,3,true,false));
    assert(wan.gpon.gemPort[0].info.channel==33);
    assert(!q1000k_services_replace(NULL,&s,1));
    assert(qs_current->channels[0]==33 && queue_model[1]==255);
    struct q1000k_gwan_binding dormant;
    assert(q1000k_gwan_binding(500,true,&dormant)==-ENODATA);
    assert(!gwan_create_new_tcont(200));
    assert(!q1000k_services_uni(NULL,1,true));
    assert(!q1000k_services_replace(NULL,&s,1));
    assert(queue_model[1]==(255^BIT(3)) && !qs_changing);
    assert(qos_model[1].mode==0 && qos_model[1].weights[3]==17 && qos_model[1].weights[2]==1);
    struct airoha_pon_qos previous=qos_model[1];
    scheduler.policy=1; assert(!q1000k_services_scheduler(NULL,0x8000,&scheduler));
    assert(qos_model[1].mode==1 && !qos_model[1].weights[3]);
    assert(qos_model[1].byte_mode==previous.byte_mode && qos_model[1].scale16==previous.scale16);
    q.weight=127; assert(!q1000k_services_queue(NULL,0x8003,&q));
    scheduler.policy=2; assert(!q1000k_services_scheduler(NULL,0x8000,&scheduler));
    assert(qos_model[1].mode==0 && qos_model[1].weights[3]==127);
    untouched=physical_ops;
    q.weight=128; assert(q1000k_services_queue(NULL,0x8003,&q)==-ERANGE);
    q.weight=0; assert(q1000k_services_queue(NULL,0x8003,&q)==-ERANGE);
    q.weight=17; q.priority=3; assert(q1000k_services_queue(NULL,0x8003,&q)==-EOPNOTSUPP);
    q.priority=4; q.allocated_size=1; assert(q1000k_services_queue(NULL,0x8003,&q)==-EOPNOTSUPP);
    q.allocated_size=0; scheduler.parent_entity_id=0x8001;
    assert(q1000k_services_scheduler(NULL,0x8000,&scheduler)==-EOPNOTSUPP);
    scheduler.parent_entity_id=0; assert(physical_ops==untouched);
    physical_fail=physical_ops+1;
    assert(q1000k_services_queue(NULL,0x8003,&q)==-ETIMEDOUT);
    assert(qs_schedulers[0].weight[3]==127 && !qs_changing);
    physical_fail=0;
    make_tag(&skb,1894,0);
    assert(q1000k_services_tx(&skb)==-ENOLINK);
    q1000k_services_enable(true);
    assert(!q1000k_services_tx(&skb) && (tx_word0&7)==3 && ((tx_word0>>3)&31)==1 && (tx_word0>>14)==500);
    assert(tx_word1==(0x7f2007ff|BIT(15)));
    assert(wan.gpon.gemPort[0].stats.tx_packets==1 && wan.gpon.gemPort[0].stats.tx_bytes==60);
    assert(!q1000k_services_rx(&skb,500));
    assert(wan.gpon.gemPort[0].stats.rx_packets==1 && wan.gpon.gemPort[0].stats.rx_bytes==60);
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
    /* A provisioned UNI rewrite carries DHCP/ARP/IP frames untagged on
     * pon and tagged on the optical GEM; a literal VLAN 0 is distinct.
     */
    {
        struct omci_service_config tag=s;
        tag.vlan_valid=false; tag.vlan_treatment_valid=true;
        tag.vlan_input_tpid=tag.vlan_output_tpid=0x8100;
        tag.vlan_rule=(struct omci_extended_vlan_rule){.filter_outer_pbit=15,
            .filter_inner_pbit=15,.treat_outer_pbit=15,.treat_inner_pbit=0,
            .treat_inner_vid=123,.treat_inner_tpid_dei=4,.raw={0xf8,0,0,0,0xf8}};
        assert(!q1000k_services_replace(NULL,&tag,1));
        memset(&skb,0,sizeof(skb)); skb.len=60; skb.data[12]=8; skb.data[13]=6;
        for(unsigned int j=0;j<12;j++) skb.data[j]=j+7;
        memset(skb.data+14,0x5a,46);
        assert(!q1000k_services_tx(&skb) && skb.len==64);
        assert(skb.data[12]==0x81 && skb.data[13]==0 && skb.data[15]==123 && skb.data[17]==6);
        assert(!q1000k_services_rx(&skb,500) && skb.len==60);
        assert(skb.data[12]==8 && skb.data[13]==6);
        for(unsigned int j=0;j<12;j++) assert(skb.data[j]==j+7);
        for(unsigned int j=14;j<60;j++) assert(skb.data[j]==0x5a);
        skb.vlan=true; skb.vlan_proto=0x8100; skb.tci=0;
        assert(q1000k_services_tx(&skb)==-ENOENT);
        tag.vlan_rule.filter_inner_pbit=8; tag.vlan_rule.filter_inner_vid=0;
        tag.vlan_rule.tags_to_remove=1; tag.vlan_rule.treat_inner_pbit=8;
        tag.vlan_rule.raw[4]=0x80;
        assert(!q1000k_services_replace(NULL,&tag,1));
        skb.tci=5<<13; assert(!q1000k_services_tx(&skb) && !skb.vlan && skb.len==64);
        assert(get_unaligned_be16(skb.data+14)==((5<<13)|123));
        assert(!q1000k_services_rx(&skb,500) && skb.len==64 && get_unaligned_be16(skb.data+14)==(5<<13));
        /* Optional AT&T priority-tag fallback uses the same OLT mapping. */
        memset(&skb,0,sizeof(skb)); skb.len=60; skb.data[12]=8; skb.data[13]=6;
        assert(q1000k_services_tx(&skb)==-ENOENT);
        fixture_fix_vlans=true;
        assert(!q1000k_services_tx(&skb) && get_unaligned_be16(skb.data+14)==123);
        assert(!q1000k_services_rx(&skb,500) && skb.len==60 && skb.data[12]==8);
        make_tag(&skb,42,0); assert(q1000k_services_tx(&skb)==-ENOENT);
        fixture_fix_vlans=false;
        /* Class 84 sees the bridge-side tag, in both packet directions. */
        {
            struct omci_service_config pipeline=tag, pair[2];
            pipeline.vlan_entity_id=0x123;
            pipeline.vlan_filter[0]=(struct omci_vlan_tagging_filter){.valid=true,
                .forward_operation=0x10,.num_entries=1,.entries={{.tci=123}}};
            pipeline.vlan_filter[1]=pipeline.vlan_filter[0];
            assert(!q1000k_services_replace(NULL,&pipeline,1));
            make_tag(&skb,0,5); assert(!q1000k_services_tx(&skb));
            assert(get_unaligned_be16(skb.data+14)==((5<<13)|123));
            assert(!q1000k_services_rx(&skb,500) && get_unaligned_be16(skb.data+14)==(5<<13));
            pipeline.vlan_filter[1].entries[0].tci=124;
            assert(!q1000k_services_replace(NULL,&pipeline,1));
            make_tag(&skb,0,5); assert(q1000k_services_tx(&skb)==-ENOENT);
            make_tag(&skb,123,5); assert(q1000k_services_rx(&skb,500)==-ENOENT);
            pipeline.vlan_ani_side=true;
            pipeline.vlan_filter[0].entries[0].tci=pipeline.vlan_filter[1].entries[0].tci=0;
            assert(!q1000k_services_replace(NULL,&pipeline,1));
            make_tag(&skb,0,5); assert(!q1000k_services_tx(&skb));
            assert(!q1000k_services_rx(&skb,500) && get_unaligned_be16(skb.data+14)==(5<<13));
            /* Mapper priority is at the bridge side, before an ANI rewrite. */
            pipeline.vlan_rule.filter_inner_pbit=5; pipeline.vlan_rule.treat_inner_pbit=2;
            pipeline.pcp_valid=true; pipeline.pcp=5;
            assert(!q1000k_services_replace(NULL,&pipeline,1));
            make_tag(&skb,0,5); assert(!q1000k_services_tx(&skb));
            assert(get_unaligned_be16(skb.data+14)==((2<<13)|123));
            assert(!q1000k_services_rx(&skb,500) && get_unaligned_be16(skb.data+14)==(5<<13));
            pipeline.pcp_valid=false;
            pipeline.vlan_rule.filter_inner_pbit=8; pipeline.vlan_rule.treat_inner_pbit=8;
            /* Negative filtering accepts ingress but rejects matching egress. */
            pipeline.vlan_filter[0].valid=false; pipeline.vlan_filter[1].forward_operation=5;
            assert(!q1000k_services_replace(NULL,&pipeline,1));
            make_tag(&skb,0,5); assert(q1000k_services_tx(&skb)==-ENOENT);
            make_tag(&skb,123,5); assert(!q1000k_services_rx(&skb,500));
            pipeline.vlan_filter[1].valid=false;
            pipeline.vlan_filter[0].valid=true; pipeline.vlan_filter[0].forward_operation=5;
            assert(!q1000k_services_replace(NULL,&pipeline,1));
            make_tag(&skb,0,5); assert(!q1000k_services_tx(&skb));
            assert(q1000k_services_rx(&skb,500)==-ENOENT);
            /* A post-treatment filter rejection cannot select the default row. */
            pair[0]=tag; pair[0].vlan_entity_id=0x123; pair[0].vlan_filter[0]=(struct omci_vlan_tagging_filter){
                .valid=true,.forward_operation=0x10,.num_entries=1,.entries={{.tci=0}}};
            pair[1]=pair[0]; pair[1].cookie=2;
            pair[1].vlan_rule.filter_inner_pbit=14; pair[1].vlan_rule.tags_to_remove=0;
            pair[1].vlan_rule.treat_inner_pbit=15; pair[1].vlan_rule.raw[4]=0xe0;
            assert(!q1000k_services_replace(NULL,pair,2));
            int sent=transmit_count;
            make_tag(&skb,0,5); assert(q1000k_services_tx(&skb)==-ENOENT && transmit_count==sent);
            /* Nor can a mapper mismatch fall through the selected tag row. */
            pair[0].vlan_filter[0].valid=pair[1].vlan_filter[0].valid=false;
            pair[0].pcp_valid=pair[1].pcp_valid=true; pair[0].pcp=pair[1].pcp=5;
            pair[0].vlan_rule.treat_inner_pbit=2;
            assert(!q1000k_services_replace(NULL,pair,2));
            make_tag(&skb,0,5); assert(q1000k_services_tx(&skb)==-ENOENT && transmit_count==sent);
            /* Inverse selection also precedes ANI-side bridge filtering. */
            pair[0]=tag; pair[0].vlan_ani_side=true; pair[0].vlan_entity_id=0x123;
            pair[0].vlan_filter[0]=(struct omci_vlan_tagging_filter){
                .valid=true,.forward_operation=0x10,.num_entries=1,.entries={{.tci=1}}};
            pair[1]=pair[0]; pair[1].cookie=2; pair[1].vlan_rule.filter_inner_vid=1;
            pair[1].vlan_rule.raw[5]=1;
            assert(!q1000k_services_replace(NULL,pair,2));
            make_tag(&skb,123,5); assert(q1000k_services_rx(&skb,500)==-ENOENT);
            pair[0].vlan_filter[0].entries[0].tci=pair[1].vlan_filter[0].entries[0].tci=0;
            assert(!q1000k_services_replace(NULL,pair,2));
            make_tag(&skb,123,5); assert(!q1000k_services_rx(&skb,500));
            assert(get_unaligned_be16(skb.data+14)==(5<<13));
            /* Standalone filters keep PCP-independent VID semantics. Two
             * filters that admit the same packet but choose different queues
             * are rejected at selection, never resolved by array order.
             */
            pair[0]=s; pair[0].vlan_valid=false;
            pair[0].vlan_filter[0]=(struct omci_vlan_tagging_filter){
                .valid=true,.forward_operation=0x10,.num_entries=1,.entries={{.tci=123}}};
            pair[1]=pair[0]; pair[1].cookie=2; pair[1].queue=4;
            pair[1].vlan_filter[0].entries[0].tci=124;
            assert(!q1000k_services_replace(NULL,pair,2));
            make_tag(&skb,123,5); assert(!q1000k_services_tx(&skb) && (tx_word0&7)==3);
            make_tag(&skb,124,5); assert(!q1000k_services_tx(&skb) && (tx_word0&7)==4);
            pair[1].vlan_filter[0].entries[1].tci=123; pair[1].vlan_filter[0].num_entries=2;
            assert(!q1000k_services_replace(NULL,pair,2));
            make_tag(&skb,123,5); sent=transmit_count;
            assert(q1000k_services_tx(&skb)==-EEXIST && sent==transmit_count);
            int ops=physical_ops;
            pair[0].vlan_filter[0].forward_operation=0x16;
            assert(q1000k_services_replace(NULL,pair,2)==-EOPNOTSUPP && physical_ops==ops);
            pair[0].vlan_filter[0].forward_operation=0x10; pair[0].vlan_filter[0].num_entries=13;
            assert(q1000k_services_replace(NULL,pair,2)==-EINVAL && physical_ops==ops);
            assert(!q1000k_services_replace(NULL,&tag,1));
            make_tag(&skb,0,5);
        }
        int saved_ops=physical_ops;
        tag.vlan_downstream_mode=2;
        assert(q1000k_services_replace(NULL,&tag,1)==-EOPNOTSUPP && physical_ops==saved_ops);
        assert(!q1000k_services_tx(&skb)); /* Prior complete table remains. */
        tag.vlan_downstream_mode=0; tag.vlan_rule.tags_to_remove=3;
        assert(!q1000k_services_replace(NULL,&tag,1));
        make_tag(&skb,0,0); int sent=transmit_count;
        assert(q1000k_services_tx(&skb)==-EPERM && transmit_count==sent);
        assert(!q1000k_services_replace(NULL,rules,2));
        make_tag(&skb,1894,4);
    }
    /* Fixed implied priority carries untagged DHCP/ARP without inserting a
     * VLAN. Downstream forwarding is independent of the upstream mapping.
     */
    {
        struct omci_service_config map[2]={s,s};
        map[0].vlan_valid=map[1].vlan_valid=false;
        map[0].pcp_valid=map[1].pcp_valid=true;
        map[0].mapper_valid=map[1].mapper_valid=true;
        map[0].mapper_unmarked_pcp=map[1].mapper_unmarked_pcp=5;
        map[0].pcp=0; map[1].pcp=5; map[1].cookie=2; map[1].queue=6;
        assert(!q1000k_services_replace(NULL,map,2));
        memset(&skb,0,sizeof(skb)); skb.len=60; skb.data[12]=8; skb.data[13]=6;
        assert(!q1000k_services_tx(&skb) && (tx_word0&7)==6 && skb.len==60 && skb.data[12]==8);
        assert(!q1000k_services_rx(&skb,500));
        make_tag(&skb,0,7); assert(q1000k_services_tx(&skb)==-ENOENT);
        assert(!q1000k_services_rx(&skb,500));
        make_tag(&skb,124,0); assert(!q1000k_services_tx(&skb) && (tx_word0&7)==3);
        /* A stacked frame uses the bridge-facing outer PCP. */
        make_tag(&skb,124,7); assert(!__vlan_insert_tag(&skb,0x88a8,(5<<13)|123));
        assert(!q1000k_services_tx(&skb) && (tx_word0&7)==6);
        int ops=physical_ops;
        map[0].mapper_unmarked_pcp=8;
        assert(q1000k_services_replace(NULL,map,2)==-EINVAL && physical_ops==ops);
        assert(!q1000k_services_replace(NULL,rules,2)); make_tag(&skb,1894,4);
    }
    int before=physical_ops;
    rules[1].pcp_valid=false;
    assert(q1000k_services_replace(NULL,rules,2)==-EEXIST && physical_ops==before && !qs_changing);
    rules[1].pcp_valid=true;
    physical_fail=physical_ops+1;
    assert(q1000k_services_replace(NULL,&s,1)==-ETIMEDOUT && !qs_changing);
    physical_fail=0;
    assert(!q1000k_services_tx(&skb));
    assert(!q1000k_services_tcont(NULL,0x8000,0xffff,true));
    assert(hardware[500] && q1000k_gwan_binding(500,true,&dormant)==-ENODATA);
    assert(!q1000k_services_tcont(NULL,0x8000,200,true));
    assert(!q1000k_services_replace(NULL,rules,2));
    assert(!q1000k_services_uni(NULL,1,false));
    assert(q1000k_services_tx(&skb)==-ENOENT && q1000k_services_rx(&skb,500)==-ENOENT);
    assert(!q1000k_services_uni(NULL,1,true));
    assert(!q1000k_services_gem(NULL,100,0,0,0,false,false));
    assert(qs_changing && !hardware[500]);
    assert(!q1000k_services_replace(NULL,NULL,0) && !qs_changing && queue_model[1]==255);
    assert(q1000k_services_tx(&skb)==-ENOENT);
    /* Teardown follows stopped packet and protocol producers. */
    int token=q1000k_protocol_enter(); q1000k_services_destroy(); q1000k_protocol_leave(token);
    /* The normalized profile alone creates its GEM/T-CONT intent. Missing
     * PLOAM allocation is dormant, with no open queue or fabricated channel.
     */
    reset_model(); q1000k_services_init();
    assert(!q1000k_services_replace(NULL,&s,1));
    assert(qs_alloc[0]==200 && qs_seeded_alloc[0] && qs_gems[0].seeded);
    assert(hardware[500] && wan.gpon.gemPort[0].info.channel==33);
    q1000k_services_enable(true); make_tag(&skb,1894,0);
    assert(q1000k_services_tx(&skb)==-ENODATA && q1000k_services_rx(&skb,500)==-ENODATA);
    assert(!gwan_create_new_tcont(200));
    assert(!q1000k_services_replace(NULL,&s,1));
    assert(!q1000k_services_tx(&skb) && ((tx_word0>>3)&31)==1);
    assert(!gwan_remove_tcont(200));
    assert(q1000k_services_tx(&skb)==-ENODATA && queue_model[1]==255);
    assert(!gwan_create_new_tcont(201));
    assert(!gwan_create_new_tcont(200));
    assert(queue_model[2]==255);
    assert(!q1000k_services_replace(NULL,&s,1));
    assert(queue_model[1]==255 && queue_model[2]==(255^BIT(3)));
    assert(!q1000k_services_tx(&skb) && ((tx_word0>>3)&31)==2);
    rules[0]=s; rules[1]=s; rules[1].cookie=7; rules[1].gem_ctp_entity_id=101;
    untouched=physical_ops;
    assert(q1000k_services_replace(NULL,rules,2)==-EEXIST && physical_ops==untouched);
    assert(!q1000k_services_tx(&skb));
    assert(!q1000k_services_replace(NULL,NULL,0));
    assert(!hardware[500] && qs_alloc[0]==0xffff && queue_model[2]==255);
    token=q1000k_protocol_enter(); q1000k_services_destroy(); q1000k_protocol_leave(token);
    /* Every physical scheduler operation may fail. Software intent changes
     * only on complete success; uncertain hardware remains contained.
     */
    int operations=0;
    for(int fail_at=0; fail_at<=operations; fail_at++) {
        reset_model(); q1000k_services_init();
        assert(!q1000k_services_tcont(NULL,0x8000,200,true));
        assert(!gwan_create_new_tcont(200));
        assert(!q1000k_services_replace(NULL,NULL,0));
        int start=physical_ops;
        physical_fail=fail_at ? start+fail_at : 0;
        int result=q1000k_services_scheduler(NULL,0x8000,&scheduler);
        if(!fail_at) {
            assert(!result && qs_schedulers[0].policy==2);
            operations=physical_ops-start;
        } else {
            assert(result<0 && qs_schedulers[0].policy==1);
            if(result==-EUCLEAN) assert(qs_changing && protocol_error);
            else assert(!qs_changing);
        }
        token=q1000k_protocol_enter(); q1000k_services_destroy(); q1000k_protocol_leave(token);
    }
    for(int ring=0;ring<=3;ring++) {
        if(ring==2) continue;
        reset_model(); q1000k_services_init();
        assert(!q1000k_services_tcont(NULL,0x8000,200,true));
        assert(!gwan_create_new_tcont(200));
        assert(!q1000k_services_gem(NULL,100,500,0x8000,3,true,ring));
        s.encryption_key_ring=ring;
        assert(!q1000k_services_replace(NULL,&s,1));
        assert(qs_gems[0].key_ring==ring && encrypted_hardware[500]==(ring==1));
        assert(wan.gpon.gemPort[0].info.rxEncrypt==(ring!=0) && wan.gpon.gemPort[0].info.txEncrypt==(ring==1));
        u8 observed=0xff;
        assert(q1000k_services_gem_key_ring(100,&observed)==-EPERM && observed==0xff);
        token=q1000k_protocol_enter();
        assert(!q1000k_services_gem_key_ring(100,&observed) && observed==ring);
        q1000k_protocol_leave(token);
        s.encryption_key_ring=ring ? 0 : 1;
        untouched=physical_ops;
        assert(q1000k_services_replace(NULL,&s,1)==-ESTALE && physical_ops==untouched);
        for(int value=4;value<256;value++) assert(q1000k_services_gem(NULL,100,500,0x8000,3,true,value)==-EINVAL);
        token=q1000k_protocol_enter(); q1000k_services_destroy(); q1000k_protocol_leave(token);
    }
    return 0;
}
