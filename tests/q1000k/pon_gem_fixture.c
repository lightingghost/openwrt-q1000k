// SPDX-License-Identifier: GPL-2.0-only
/* Execute production indirect commands against two modeled MAC registers. */
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
typedef u16 ushort;
typedef u8 unchar;
typedef unsigned int uint;
struct file;
#define Q1000K_PON_IDENTITY
#define pr_err(...) ((void)0)
#define BIT(n) (UINT32_C(1)<<(n))
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_RELAXED)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELAXED)
#define GEM_PORT_CFG 0x5274
#define GEM_PORT_STS 0x5278
#define DEFINE_SPINLOCK(n) pthread_mutex_t n=PTHREAD_MUTEX_INITIALIZER
static _Thread_local bool held;
#define spin_lock_irqsave(l,f) do { (f)=0; assert(!held); assert(!pthread_mutex_lock(l)); held=true; } while(0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); assert(held); held=false; assert(!pthread_mutex_unlock(l)); } while(0)
#define lockdep_assert_held(l) assert(held)
static u8 table[65536];
static u32 command;
static unsigned int commands,writes,polls,delays,fail_command,delay_polls,poll_index;
static bool ignore_write,corrupt_read,yield_io;
static void IO_SREG(unsigned int reg,u32 value)
{
    assert(held && reg==0x5274 && !(value & ~UINT32_C(0x8007ffff)));
    assert((value&0xffff)!=0xffff);
    command=value; commands++; poll_index=0;
    if(value & BIT(31)) {
        writes++;
        if(!ignore_write) table[value&0xffff]=(value>>16)&7;
    } else assert(!(value&0xffff0000));
    if(yield_io) sched_yield();
}
static u32 IO_GREG(unsigned int reg)
{
    assert(held && reg==0x5278); polls++;
    if(commands==fail_command || poll_index++<delay_polls) return 0;
    if(yield_io) sched_yield();
    return BIT(31) | (table[command&0xffff] ^ (corrupt_read && writes ? 1 : 0));
}
static void udelay(unsigned int us) { assert(held && us==1); delays++; }
/* PRODUCTION */
/* A new independent case models reset hardware plus a fresh module instance. */
static void reset_model(void)
{
    assert(!held); memset(table,0,sizeof(table)); q1000k_gem_fault=false;
    commands=writes=polls=delays=fail_command=delay_polls=poll_index=0;
    ignore_write=corrupt_read=yield_io=false;
}
static const struct q1000k_gem_value empty={0}, unicast={1,0,0}, multicast={1,1,1};
static void latched(void)
{
    unsigned int before=commands;
    struct q1000k_gem_value value={9,8,7};
    assert(q1000k_gem_faulted());
    assert(q1000k_gem_read(7,&value)==-EIO);
    assert(value.valid==9 && value.multicast==8 && value.encrypted==7);
    assert(q1000k_gem_replace(7,&empty,&unicast)==-EIO && commands==before);
}
struct request { u16 id; int result; };
static void *create(void *arg)
{
    struct request *r=arg; r->result=q1000k_gem_replace(r->id,&empty,&unicast); return NULL;
}
int main(void)
{
    struct q1000k_gem_value value={9,8,7},bad={2,0,0};
    reset_model();
    u8 valid=9,type=8,encryption=7;
    assert(gponDevGetGemInfo(65535,&valid,&type,&encryption)==-EINVAL && valid==9 && type==8 && encryption==7);
    assert(gponDevGetGemInfo(1,NULL,&type,&encryption)==-EINVAL);
    assert(gponDevSetGemInfo(500,1,0,0)==-EOPNOTSUPP);
    assert(gponDevSetGemInfoNoCheck(500,1,0,0)==-EOPNOTSUPP);
    gponDevResetGemInfo();
    assert(xgpon_register_test(100)==-EOPNOTSUPP);
    assert(test_gpon_mac_reg()==-EOPNOTSUPP && gpon_dvt_sw_reset()==-EOPNOTSUPP);
    assert(gpon_debug_write_proc(NULL,"writereg 5274 80040005",23,NULL)==-EOPNOTSUPP && !commands);
    table[65534]=5;
    assert(!gponDevGetGemInfo(65534,&valid,&type,&encryption) && valid==1 && type==1 && encryption==1);
    reset_model();
    assert(q1000k_gem_read(0xffff,&value)==-EINVAL && value.valid==9);
    assert(q1000k_gem_read(1,NULL)==-EINVAL);
    assert(q1000k_gem_replace(0xffff,&empty,&unicast)==-EINVAL);
    assert(q1000k_gem_replace(1,NULL,&unicast)==-EINVAL);
    assert(q1000k_gem_replace(1,&empty,NULL)==-EINVAL);
    assert(q1000k_gem_replace(1,&bad,&unicast)==-EINVAL);
    bad=(struct q1000k_gem_value){0,2,0};
    assert(q1000k_gem_replace(1,&empty,&bad)==-EINVAL);
    bad=(struct q1000k_gem_value){0,0,2};
    assert(q1000k_gem_replace(1,&empty,&bad)==-EINVAL && !commands);
    for(unsigned int id=0;id<65535;id++) {
        table[id]=id&7;
        assert(!q1000k_gem_read(id,&value));
        assert(value.valid==!!(id&4) && value.multicast==!(id&2) && value.encrypted==!!(id&1));
    }
    for(unsigned int bits=0;bits<4;bits++) {
        reset_model(); table[500]=bits; /* Stale invalid type/encryption bits are immaterial. */
        assert(!q1000k_gem_replace(500,&empty,&unicast) && table[500]==6 && writes==1);
        assert(q1000k_gem_replace(500,&empty,&multicast)==-ESTALE && table[500]==6 && writes==1);
        assert(!q1000k_gem_replace(500,&unicast,&unicast) && writes==1);
        assert(!q1000k_gem_replace(500,&unicast,&multicast) && table[500]==5 && writes==2);
        /* Regression: matching type/encryption must not skip valid -> invalid. */
        assert(!q1000k_gem_replace(500,&multicast,&empty) && !(table[500]&4) && writes==3);
    }
    reset_model(); delay_polls=2;
    assert(!q1000k_gem_replace(65534,&empty,&multicast));
    assert(commands==3 && polls==9 && delays==6 && table[65534]==5);
    for(unsigned int failure=1;failure<=3;failure++) {
        reset_model(); fail_command=failure;
        assert(q1000k_gem_replace(65534,&empty,&unicast)==-ETIMEDOUT);
        assert(commands==failure && delays==3000); latched();
    }
    reset_model(); ignore_write=true;
    assert(q1000k_gem_replace(7,&empty,&unicast)==-EIO); latched();
    reset_model(); corrupt_read=true;
    assert(q1000k_gem_replace(7,&empty,&unicast)==-EIO); latched();
    reset_model(); table[7]=6; ignore_write=true;
    assert(q1000k_gem_replace(7,&unicast,&empty)==-EIO && table[7]==6); latched();
    pthread_t threads[32]; struct request requests[32];
    for(unsigned int different=0;different<2;different++) {
        reset_model(); yield_io=true;
        for(unsigned int i=0;i<32;i++) {
            requests[i]=(struct request){.id=1000+(different ? i : 0)};
            assert(!pthread_create(&threads[i],NULL,create,&requests[i]));
        }
        unsigned int successes=0;
        for(unsigned int i=0;i<32;i++) {
            assert(!pthread_join(threads[i],NULL));
            if(!requests[i].result) successes++;
            else assert(!different && requests[i].result==-ESTALE);
            assert(table[requests[i].id]==6);
        }
        assert(successes==(different ? 32u : 1u) && writes==successes);
    }
    return 0;
}
