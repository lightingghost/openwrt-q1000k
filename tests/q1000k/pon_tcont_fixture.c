// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h))) & (UINT32_MAX<<(l)))
#define FIELD_PREP(m,v) (((u32)(v)<<__builtin_ctz(m)) & (m))
#define FIELD_GET(m,v) (((u32)(v)&(m))>>__builtin_ctz(m))
#define GPON_UNASSIGN_ONU_ID 0x3ff
#define TCONT_ID_CFG 0x5250
#define TCONT_ID_STS 0x5254
#define DEFINE_SPINLOCK(name) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
static _Thread_local int held;
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!held); assert(!pthread_mutex_lock(l)); held=1; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(held); held=0; assert(!pthread_mutex_unlock(l)); } while(0)
#define lockdep_assert_held(l) assert(held)
struct slot { bool valid; u16 id; };
static struct slot table[32];
static u32 pending_command;
static unsigned int commands, data_writes, polls, delays;
static unsigned int fail_command, delay_polls, poll_index;
static bool ignore_write, corrupt_read, yield_io;
static void IO_SREG(unsigned int reg,u32 value)
{
    assert(held && reg==0x5250 && !(value & ~UINT32_C(0x81f13fff)));
    unsigned int channel=(value>>20)&31;
    pending_command=value; commands++; poll_index=0;
    if(value & UINT32_C(0x80000000)) {
        assert(channel); /* Never write the ONU-ID shadow through T-CONT commands. */
        data_writes++;
        if(!ignore_write) {
            table[channel].valid=!!(value & UINT32_C(0x10000));
            table[channel].id=value & 0x3fff;
        }
    } else assert(value==(channel<<20));
    if(yield_io) sched_yield();
}
static u32 IO_GREG(unsigned int reg)
{
    assert(held && reg==0x5254);
    unsigned int channel=(pending_command>>20)&31;
    polls++;
    if(commands==fail_command || poll_index++<delay_polls) return 0;
    if(yield_io) sched_yield();
    return UINT32_C(0x80000000) | (table[channel].valid ? 0x10000 : 0) |
        ((table[channel].id ^ (corrupt_read ? 1 : 0)) & 0x3fff);
}
static void udelay(unsigned int usec) { assert(held && usec==1); delays++; }

/* PRODUCTION */

/* Simulate a fresh module and reset hardware between independent cases.
 * Production has no software-only method to clear a latched command fault.
 */
static void reset_model(void)
{
    memset(table,0,sizeof(table));
    q1000k_tcont_fault=false; q1000k_tcont_quarantined=0;
    commands=data_writes=polls=delays=fail_command=delay_polls=poll_index=0;
    ignore_write=corrupt_read=yield_io=false;
    assert(!held);
}
static void failed_outputs(void)
{
    unsigned int before=commands;
    bool valid=true;
    u16 id=0x5555;
    u8 channel=0xa5;
    assert(q1000k_tcont_read(1,&valid,&id)==-EIO && valid && id==0x5555);
    assert(q1000k_tcont_find(100,10,&channel)==-EIO && channel==0xa5);
    assert(q1000k_tcont_enable(100,10)==-EIO);
    assert(q1000k_tcont_disable(100,10)==-EIO && commands==before);
}
struct request { u16 id; int result; bool disable; };
static void *allocate(void *arg)
{
    struct request *r=arg;
    r->result=r->disable ? q1000k_tcont_disable(r->id,10) : q1000k_tcont_enable(r->id,10);
    return NULL;
}
int main(void)
{
    bool valid=true;
    u16 id=0xaaaa;
    u8 channel=0xa5;
    reset_model();
    assert(q1000k_tcont_read(32,&valid,&id)==-EINVAL && valid && id==0xaaaa);
    assert(q1000k_tcont_read(UINT32_MAX,&valid,&id)==-EINVAL);
    assert(q1000k_tcont_read(1,NULL,&id)==-EINVAL);
    assert(q1000k_tcont_read(1,&valid,NULL)==-EINVAL);
    assert(q1000k_tcont_enable(0x4000,10)==-EINVAL);
    assert(q1000k_tcont_disable(0xffff,10)==-EINVAL);
    assert(q1000k_tcont_find(0x4000,10,&channel)==-EINVAL && channel==0xa5);
    assert(q1000k_tcont_find(100,10,NULL)==-EINVAL);
    assert(q1000k_tcont_enable(10,10)==-EOPNOTSUPP);
    assert(q1000k_tcont_disable(10,10)==-EOPNOTSUPP);
    assert(!q1000k_tcont_find(10,10,&channel) && !channel && !commands);
    channel=0xa5;
    assert(q1000k_tcont_find(0x3ff,0x3ff,&channel)==-ENOENT && channel==0xa5);
    for(unsigned int ch=0;ch<32;ch++) {
        table[ch].valid=ch&1; table[ch].id=(ch*521)&0x3fff;
        assert(!q1000k_tcont_read(ch,&valid,&id) && valid==table[ch].valid && id==table[ch].id);
    }
    reset_model();
    table[1].id=100; /* Invalid stale record must not match or be deleted. */
    table[17]=(struct slot){true,100};
    assert(!q1000k_tcont_find(100,10,&channel) && channel==17);
    assert(q1000k_tcont_enable(100,10)==-EEXIST && !data_writes);
    assert(q1000k_tcont_disable(100,10)==17 && !table[17].valid && !table[1].valid);
    assert(data_writes==1 && (q1000k_tcont_quarantined & (1u<<17)));
    assert(q1000k_tcont_disable(100,10)==-ENOENT && data_writes==1);
    for(unsigned int ch=1;ch<32;ch++) table[ch]=(struct slot){true,(u16)(200+ch)};
    table[17].valid=false;
    assert(q1000k_tcont_enable(100,10)==-ENOSPC && data_writes==1); /* quarantine survives invalidation */
    reset_model();
    delay_polls=2;
    assert(q1000k_tcont_enable(0x3fff,10)==1);
    assert(commands==33 && data_writes==1 && polls==99 && delays==66);
    assert(table[1].valid && table[1].id==0x3fff && !table[0].valid);
    for(unsigned int ch=2;ch<32;ch++) assert(q1000k_tcont_enable(ch+1000,10)==(int)ch);
    unsigned int writes=data_writes;
    assert(q1000k_tcont_enable(999,10)==-ENOSPC && writes==data_writes);
    assert(!q1000k_tcont_find(1031,10,&channel) && channel==31);
    for(unsigned int fail=1;fail<=33;fail++) {
        reset_model(); fail_command=fail;
        assert(q1000k_tcont_enable(100,10)==-ETIMEDOUT);
        assert(commands==fail && delays==3000);
        failed_outputs();
    }
    reset_model(); ignore_write=true;
    assert(q1000k_tcont_enable(100,10)==-EIO); failed_outputs();
    reset_model(); corrupt_read=true;
    assert(q1000k_tcont_enable(100,10)==-EIO); failed_outputs();
    reset_model(); table[31]=(struct slot){true,100}; ignore_write=true;
    assert(q1000k_tcont_disable(100,10)==-EIO && table[31].valid);
    assert(q1000k_tcont_quarantined==(1u<<31)); failed_outputs();

    pthread_t threads[32];
    struct request requests[32];
    for(unsigned int different=0;different<2;different++) {
        reset_model(); yield_io=true;
        for(unsigned int i=0;i<32;i++) {
            requests[i]=(struct request){.id=(u16)(1000+(different ? i : 0))};
            assert(!pthread_create(&threads[i],NULL,allocate,&requests[i]));
        }
        unsigned int successes=0,failures=0;
        for(unsigned int i=0;i<32;i++) {
            assert(!pthread_join(threads[i],NULL));
            int result=requests[i].result;
            if(result>0) {
                successes++; assert(table[result].valid && table[result].id==requests[i].id);
            } else {
                failures++; assert(result==(different ? -ENOSPC : -EEXIST));
            }
        }
        assert(successes==(different ? 31u : 1u) && failures==(different ? 1u : 31u));
        assert(data_writes==successes);
        for(unsigned int ch=1;ch<32;ch++) for(unsigned int other=ch+1;other<32;other++)
            assert(!table[ch].valid || !table[other].valid || table[ch].id!=table[other].id);
    }
    return 0;
}
