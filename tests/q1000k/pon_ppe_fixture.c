// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define DECLARE_FLEX_ARRAY(t,n) t n[0]
#define PPE_ENTRY_SIZE 80
#define AIROHA_GDM2_IDX 2
/* TYPES */
struct list_head { struct list_head *next,*prev; };
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_entry(p,t,m) container_of(p,t,m)
#define list_for_each_entry_safe(p,n,h,m) \
    for(p=list_entry((h)->next,typeof(*p),m),n=list_entry(p->m.next,typeof(*p),m); \
        &p->m!=(h);p=n,n=list_entry(n->m.next,typeof(*n),m))
static void INIT_LIST_HEAD(struct list_head *h) { h->next=h->prev=h; }
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_add_tail(struct list_head *n,struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
static void list_del_init(struct list_head *n) {
    n->prev->next=n->next; n->next->prev=n->prev; INIT_LIST_HEAD(n);
}
struct hlist_node { bool linked; };
struct hlist_head { unsigned int inserts; };
static void hlist_del_init(struct hlist_node *n) { n->linked=false; }
static void hlist_add_head(struct hlist_node *n,struct hlist_head *h) { assert(!n->linked); n->linked=true; h->inserts++; }
struct airoha_eth { struct airoha_ppe *ppe; };
struct airoha_gdm_dev { struct airoha_eth *eth; u64 pon_flow_epoch; bool pon_flow_fault; };
struct airoha_flow_table_entry {
    struct list_head pon_list; struct hlist_node list, l2_subflow_node;
    struct airoha_gdm_dev *pon_dev; u64 pon_epoch;
    int type; u32 hash; struct airoha_foe_entry data;
};
struct airoha_ppe { struct list_head pon_flows; unsigned int pon_flow_count; struct hlist_head foe_flow[1]; };
#define FLOW_TYPE_L4 0
#define FLOW_TYPE_L2_SUBFLOW 1
#define kfree free
static bool ppe_lock;
#define lockdep_assert_held(l) assert(*(l))
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=true; (f)=1; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(*(l) && (f)==1); *(l)=false; } while(0)
static unsigned int commits,gate_closes;
static int gate_error;
static int airoha_ppe_pon_ifc_close(struct airoha_gdm_dev *d) { assert(ppe_lock); gate_closes++; return gate_error; }
static u32 last_hash;
static int commit_error;
static int airoha_ppe_foe_commit_entry(struct airoha_ppe *ppe,struct airoha_foe_entry *e,u32 hash,bool wlan) {
    assert(ppe_lock && !wlan && !(e->ib1 & 0x30000000));
    commits++; last_hash=hash; return commit_error;
}
static int airoha_ppe_foe_l2_flow_commit_entry(struct airoha_ppe *p,struct airoha_flow_table_entry *e) { assert(0); return 0; }
static u32 airoha_ppe_foe_get_entry_hash(struct airoha_ppe *p,struct airoha_foe_entry *e) { return 0; }
static void airoha_ppe_pon_fault(struct airoha_gdm_dev *d) { d->pon_flow_fault=true; }
/* PRODUCTION */
static u32 word(struct airoha_foe_entry *e,unsigned int i) { u32 w; memcpy(&w,e->data+4*i,4); return w; }
int main(void)
{
    /* Expectations use OEM byte offsets and literal bit positions, not the
     * macros used to encode them. Ordinary tuple/MAC/VLAN bytes stay intact.
     */
    assert(sizeof(struct airoha_foe_entry)==80);
    for(unsigned int ipv6=0;ipv6<2;ipv6++)
        for(unsigned int ch=1;ch<32;ch++) for(unsigned int q=0;q<8;q++) {
            struct airoha_pon_flow f={.gem=0xabcd,.channel=ch,.queue=q};
            struct airoha_foe_entry e,before;
            memset(&e,0xa5,sizeof(e)); before=e;
            airoha_ppe_foe_set_pon(&e,&f,ipv6);
            unsigned int ib=ipv6 ? 11 : 4, tag=ipv6 ? 50 : 46;
            assert(((word(&e,ib)>>5)&15)==2);
            assert((word(&e,ib)&31)==ch && (word(&e,ib)&0x200) && !(word(&e,ib)&0x400));
            assert(((word(&e,10)>>11)&31)==ch && ((word(&e,10)>>8)&7)==q);
            assert(e.data[tag]==0xcd && e.data[tag+1]==0xab);
            if(ipv6) assert(((word(&e,15)>>16)&31)==2);
            for(unsigned int i=0;i<80;i++) {
                if(i/4==ib || i/4==10 || i==tag || i==tag+1 || (ipv6 && i/4==15)) continue;
                assert(e.data[i]==before.data[i]);
            }
        }
    struct airoha_ppe p={}; INIT_LIST_HEAD(&p.pon_flows);
    struct airoha_eth eth={.ppe=&p};
    struct airoha_gdm_dev dev={.eth=&eth,.pon_flow_epoch=9},peer={.eth=&eth,.pon_flow_epoch=3};
    struct airoha_flow_table_entry entries[65]={},other={};
    for(unsigned int i=0;i<65;i++) {
        struct airoha_flow_table_entry *e=&entries[i];
        e->pon_dev=&dev; e->pon_epoch=9; e->data.ib1=0x20000000;
        INIT_LIST_HEAD(&e->pon_list);
        int ret=airoha_ppe_foe_flow_commit_entry(&p,e);
        assert(ret==(i==64 ? -ENOSPC : 0));
        if(i<64) { assert(e->hash==0xffff && e->list.linked); e->hash=i; }
    }
    assert(p.pon_flow_count==64);
    /* A second attachment's entry must survive another owner's invalidation. */
    other.pon_dev=&peer; other.hash=0xffff; other.list.linked=true;
    INIT_LIST_HEAD(&other.pon_list); list_add_tail(&other.pon_list,&p.pon_flows); p.pon_flow_count++;
    commit_error=-ETIMEDOUT;
    assert(airoha_ppe_pon_invalidate(&dev)==-ETIMEDOUT);
    assert(dev.pon_flow_fault);
    assert(commits==64 && last_hash==63 && dev.pon_flow_epoch==10 && !ppe_lock);
    assert(p.pon_flow_count==1 && other.list.linked && !list_empty(&other.pon_list));
    for(unsigned int i=0;i<64;i++) assert(entries[i].hash==0xffff && !entries[i].list.linked && list_empty(&entries[i].pon_list));
    /* Resolver-before-retire / publish-after-retire must never reinstall. */
    assert(airoha_ppe_foe_flow_commit_entry(&p,&entries[64])==-ESTALE);
    assert(!entries[64].list.linked && p.pon_flow_count==1);
    commit_error=0; assert(!airoha_ppe_pon_invalidate(&peer));
    assert(!p.pon_flow_count && list_empty(&p.pon_flows));
    gate_error=-EIO;
    assert(airoha_ppe_pon_invalidate(&peer)==-EIO && peer.pon_flow_fault);
    gate_error=0;
    dev.pon_flow_epoch=UINT64_MAX;
    assert(airoha_ppe_pon_invalidate(&dev)==-EOVERFLOW);
    /* Firewall removal must keep PPE admission until the final PON lease
     * is retired, then close it even without a PON service restart.
     */
    dev.pon_flow_epoch=1; dev.pon_flow_fault=false;
    unsigned int closed_before=gate_closes;
    for(unsigned int i=0;i<2;i++) {
        entries[i].pon_dev=&dev; entries[i].pon_epoch=1;
        entries[i].hash=i; entries[i].type=FLOW_TYPE_L4;
        entries[i].data.ib1=0x20000000;
        list_add_tail(&entries[i].pon_list,&p.pon_flows); p.pon_flow_count++;
    }
    ppe_lock=true;
    airoha_ppe_foe_remove_flow(&p,&entries[0]);
    assert(p.pon_flow_count==1 && gate_closes==closed_before);
    airoha_ppe_foe_remove_flow(&p,&entries[1]);
    assert(!p.pon_flow_count && gate_closes==closed_before+1);
    assert(entries[0].hash==0xffff && entries[1].hash==0xffff);
    ppe_lock=false;
    return 0;
}
