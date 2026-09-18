// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define EXPORT_SYMBOL_GPL(...)
static bool rtnl, irq;
static unsigned int writes, polls;
static int fail_command, corrupt_command, ignore_command, ignore_mode, absent_command;
static bool retire_during, fault_during;
static void rtnl_lock(void) { assert(!rtnl && !irq); rtnl=true; }
static void rtnl_unlock(void) { assert(rtnl); rtnl=false; }
#define in_interrupt() (irq)
#define irqs_disabled() (irq)
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!*(l)); *(l)=true; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(*(l)); *(l)=false; } while(0)
#define rcu_access_pointer(p) (p)
#define atomic_read_acquire(p) (*(p))
struct airoha_qdma { u32 command, mode[4], global; u16 weights[32][8]; };
struct airoha_eth { struct airoha_qdma qdma[2]; };
struct airoha_gdm_dev { struct airoha_eth *eth; };
struct airoha_pon {
    void *netdev;
    struct airoha_gdm_dev *dma_dev;
    bool admission_lock,control_fault,paused,pause_ready,rx_closed,rx_drained;
    u32 retiring,configuring,fe_retired,tx_enabled;
    int pending;
    u8 closed[32];
    int channel_pending[32];
};
static struct airoha_eth eth;
static struct airoha_gdm_dev dev={.eth=&eth};
static struct airoha_pon pon;
static unsigned int command_count;
static u32 airoha_qdma_rr(struct airoha_qdma *q,u32 reg)
{
    assert(q==&eth.qdma[1] && rtnl && !pon.admission_lock);
    if(reg==0x1020) return q->global;
    if(reg>=0x1040 && reg<=0x104c && !(reg&3)) return q->mode[(reg-0x1040)/4];
    assert(reg==0x1024);
    if(retire_during) pon.retiring|=pon.configuring;
    if(fault_during) pon.control_fault=true;
    polls++;
    return (int)command_count==absent_command ? ~0U : q->command;
}
static void airoha_qdma_wr(struct airoha_qdma *q,u32 reg,u32 value)
{
    assert(q==&eth.qdma[1] && rtnl && !pon.admission_lock);
    writes++;
    if(reg>=0x1040 && reg<=0x104c && !(reg&3)) {
        if(!ignore_mode) q->mode[(reg-0x1040)/4]=value;
        return;
    }
    assert(reg==0x1024);
    unsigned int c=(value>>19)&31, n=(value>>16)&7;
    assert(pon.configuring==BIT(c) && pon.closed[c]==255);
    command_count++;
    if(value&BIT(31)) {
        if((int)command_count!=ignore_command) q->weights[c][n]=value&0xffff;
    }
    q->command=(value&~0xffffU)|BIT(30)|q->weights[c][n];
    if((int)command_count==fail_command) q->command&=~BIT(30);
    if((int)command_count==corrupt_command) q->command^=BIT(19);
}
#define read_poll_timeout(op,val,condition,delay,timeout,sleep,args...) ({ \
    int result=-ETIMEDOUT; \
    for(unsigned int attempt=0;attempt<10;attempt++) { \
        (val)=op(args); if(condition) { result=0; break; } \
    } result; })
/* PRODUCTION */
static void reset_fixture(void)
{
    memset(&eth,0,sizeof(eth)); memset(&pon,0,sizeof(pon));
    pon.netdev=&dev; pon.dma_dev=&dev; memset(pon.closed,255,sizeof(pon.closed));
    for(unsigned int i=0;i<4;i++) eth.qdma[1].mode[i]=0x88888888;
    memset(&eth.qdma[0],0xa5,sizeof(eth.qdma[0]));
    fail_command=corrupt_command=ignore_command=ignore_mode=absent_command=0;
    command_count=writes=polls=0; retire_during=fault_during=false; irq=false;
}
static void check_unmodified_lan(void)
{
    const u8 *data=(const u8 *)&eth.qdma[0];
    for(unsigned int i=0;i<sizeof(eth.qdma[0]);i++) assert(data[i]==0xa5);
    assert(!pon.configuring && !pon.admission_lock && !rtnl);
}
int main(void)
{
    struct airoha_pon_qos cfg,actual,sentinel;
    unsigned int c,mode,n,k;
    /* First data service: OMCC stays open and has a pending descriptor.
     * Only the target bank must be empty/closed, and its commands must not
     * change channel zero's mode/weights, admission or pending accounting.
     */
    reset_fixture();
    pon.closed[0]=254; pon.channel_pending[0]=1; pon.pending=1; pon.tx_enabled=3;
    eth.qdma[1].weights[0][3]=1234;
    cfg=(struct airoha_pon_qos){.mode=1};
    assert(!airoha_pon_set_qos(&pon,1,&cfg));
    assert(!airoha_pon_get_qos(&pon,1,&actual));
    assert(actual.mode==1 && eth.qdma[1].weights[0][3]==1234);
    assert(pon.closed[0]==254 && pon.channel_pending[0]==1 && pon.pending==1 && pon.tx_enabled==3);
    assert(!(eth.qdma[1].mode[0]&7) && !pon.paused && !pon.retiring);
    check_unmodified_lan();
    for(unsigned int global=0;global<4;global++) {
        reset_fixture();
        eth.qdma[1].global=(global&1?BIT(3):0)|(global&2?BIT(31):0)|0x400;
        for(c=0;c<32;c++) for(mode=0;mode<8;mode++) {
            memset(&cfg,0,sizeof(cfg)); cfg.mode=mode;
            cfg.byte_mode=!!(global&1); cfg.scale16=!!(global&2);
            unsigned int wrr=mode==0?8:mode==1?0:9-mode;
            for(n=0;n<wrr;n++) cfg.weights[n]=n==7?65535:c*256+n+1;
            u32 old[4]; memcpy(old,eth.qdma[1].mode,sizeof(old));
            assert(!airoha_pon_set_qos(&pon,c,&cfg));
            assert(!airoha_pon_get_qos(&pon,c,&actual));
            assert(actual.mode==mode && actual.byte_mode==cfg.byte_mode && actual.scale16==cfg.scale16);
            assert(!memcmp(actual.weights,cfg.weights,sizeof(cfg.weights)));
            u32 mask=7u<<((c&7)*4);
            for(k=0;k<4;k++) assert((eth.qdma[1].mode[k]^(old[k])) & ~(k==c/8?mask:0u) ? 0 : 1);
            assert(eth.qdma[1].global==((global&1?BIT(3):0)|(global&2?BIT(31):0)|0x400));
            check_unmodified_lan();
        }
    }
    memset(&cfg,0,sizeof(cfg)); cfg.mode=0;
    for(n=0;n<8;n++) cfg.weights[n]=n+1;
    /* Every indirect write/read command can independently time out or misaddress. */
    for(k=1;k<=16;k++) for(unsigned int fault=0;fault<2;fault++) {
        reset_fixture();
        if(fault) corrupt_command=k; else fail_command=k;
        assert(airoha_pon_set_qos(&pon,31,&cfg)==(fault?-EIO:-ETIMEDOUT));
        assert(pon.control_fault);
        n=writes;
        assert(airoha_pon_set_qos(&pon,31,&cfg)==-EIO && writes==n);
        check_unmodified_lan();
    }
    for(k=1;k<=16;k+=2) {
        reset_fixture(); ignore_command=k;
        assert(airoha_pon_set_qos(&pon,8,&cfg)==-EIO && pon.control_fault);
        check_unmodified_lan();
    }
    reset_fixture(); cfg.mode=1; memset(cfg.weights,0,sizeof(cfg.weights)); ignore_mode=1;
    assert(airoha_pon_set_qos(&pon,31,&cfg)==-EIO && pon.control_fault);
    memset(&sentinel,0xa5,sizeof(sentinel));
    for(k=1;k<=8;k++) {
        reset_fixture(); actual=sentinel; fail_command=k;
        assert(airoha_pon_get_qos(&pon,23,&actual)==-ETIMEDOUT);
        assert(!memcmp(&actual,&sentinel,sizeof(actual)) && pon.control_fault);
    }
    for(k=1;k<=16;k++) {
        reset_fixture(); absent_command=k;
        assert(airoha_pon_set_qos(&pon,31,&cfg)==-EIO && pon.control_fault);
    }
    reset_fixture(); actual=sentinel; eth.qdma[1].global=~0U;
    assert(airoha_pon_get_qos(&pon,31,&actual)==-EIO && !writes && pon.control_fault);
    assert(!memcmp(&actual,&sentinel,sizeof(actual)));
    reset_fixture(); cfg.byte_mode=true;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-EOPNOTSUPP && !writes && !pon.control_fault);
    cfg.byte_mode=false; cfg.mode=8;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-EINVAL && !writes);
    cfg.mode=0; cfg.weights[0]=1;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-EINVAL && !writes);
    cfg.mode=1; cfg.weights[0]=0;
    pon.closed[0]=254;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-EBUSY && !writes);
    pon.closed[0]=255; pon.channel_pending[0]=1;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-EAGAIN && !writes);
    pon.channel_pending[0]=0; irq=true;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-EWOULDBLOCK && !writes);
    irq=false; pon.retiring=1;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-ESHUTDOWN && !writes);
    pon.retiring=0; pon.netdev=NULL;
    assert(airoha_pon_set_qos(&pon,0,&cfg)==-ENODEV && !writes);
    assert(airoha_pon_set_qos(&pon,32,&cfg)==-EINVAL && !writes);
    assert(airoha_pon_set_qos(NULL,0,&cfg)==-EINVAL);
    assert(airoha_pon_get_qos(&pon,0,NULL)==-EINVAL);
    reset_fixture(); retire_during=true;
    assert(airoha_pon_set_qos(&pon,17,&cfg)==-ESHUTDOWN);
    check_unmodified_lan();
    reset_fixture(); actual=sentinel; fault_during=true;
    assert(airoha_pon_get_qos(&pon,17,&actual)==-EIO);
    assert(!memcmp(&actual,&sentinel,sizeof(actual)));
    check_unmodified_lan();
    /* Snapshot all retired schedulers before the namespace epoch advances.
     * Every missing drain condition rejects the read without a command or
     * published result; even a complete drain cannot authorize QoS writes.
     */
    for(k=0;k<8;k++) {
        reset_fixture();
        pon.retiring=pon.fe_retired=~0U;
        pon.paused=pon.pause_ready=pon.rx_closed=pon.rx_drained=true;
        switch(k) {
        case 1: pon.paused=false; break;
        case 2: pon.pause_ready=false; break;
        case 3: pon.rx_closed=false; break;
        case 4: pon.rx_drained=false; break;
        case 5: pon.fe_retired&=~BIT(31); break;
        case 6: pon.tx_enabled=BIT(31); break;
        case 7: pon.pending=1; break;
        }
        for(c=0;c<32;c++) {
            actual=sentinel; n=writes;
            int ret=airoha_pon_get_qos(&pon,c,&actual);
            assert(ret==(k?-ESHUTDOWN:0));
            if(k) assert(writes==n && !memcmp(&actual,&sentinel,sizeof(actual)));
            else assert(actual.mode==0 && !memcmp(actual.weights,cfg.weights,sizeof(cfg.weights)));
            n=writes;
            assert(airoha_pon_set_qos(&pon,c,&cfg)==-ESHUTDOWN && writes==n);
            assert(pon.retiring==~0U);
        }
        check_unmodified_lan();
    }
    reset_fixture();
    pon.retiring=pon.fe_retired=~0U;
    pon.paused=pon.pause_ready=pon.rx_closed=pon.rx_drained=true;
    actual=sentinel; fault_during=true;
    assert(airoha_pon_get_qos(&pon,31,&actual)==-EIO);
    assert(!memcmp(&actual,&sentinel,sizeof(actual)));
    check_unmodified_lan();
    return 0;
}
