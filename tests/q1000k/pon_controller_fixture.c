// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#define DEFINE_MUTEX(n) struct mutex n
#define EXPORT_SYMBOL_GPL(n)
#define IS_ENABLED(n) 0
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_err(dev,...) test_dev_err(__VA_ARGS__)
#define dev_err_ratelimited(dev,...) test_dev_err(__VA_ARGS__)
static char last_diagnostic[512];
static unsigned int health_diagnostics;
static void test_dev_err(const char *format,...)
{
    va_list args;
    va_start(args,format); vsnprintf(last_diagnostic,sizeof(last_diagnostic),format,args); va_end(args);
    if(strstr(last_diagnostic,"controller health mismatch:")) health_diagnostics++;
}
struct mutex { bool held; };
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=false; }
static int atomic_context;
static int in_interrupt(void) { return 0; }
static int in_atomic(void) { return atomic_context; }
static int irqs_disabled(void) { return 0; }
static int rcu_preempt_depth(void) { return 0; }
static int rcu_read_lock_held(void) { return 1; }
struct kref { unsigned int refs; };
static void kref_get(struct kref *r) { assert(r->refs); r->refs++; }
static void kref_put(struct kref *r,void (*free_ref)(struct kref *)) { assert(r->refs); if(!--r->refs) free_ref(r); }
static int freed;
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static void kfree(void *p) { assert(p); freed++; free(p); }
typedef uint32_t u32;
struct en7573_io { void *ctx; };
struct en7573_oem_post { bool saved; };
struct en7573_state { int md32_enabled,tx_disabled; };
struct q1000k_pon {
    struct mutex lock;
    struct kref ref;
    bool dead,leased,tx_enabled,initialized,tx_inhibited;
    int fault,mode,last_error;
    const char *stage;
    unsigned char calibration[513];
    struct en7573_io io;
    struct en7573_oem_post oem_post_original;
    int *los[2];
};
static int samples,writes,off,los_calls,sample_error,tx_error,off_error,los_value,hw_mcu,hw_disabled;
static int en7573_sample_state(struct en7573_io *io,struct en7573_state *state)
{
    struct q1000k_pon *p=io->ctx; assert(p->lock.held && !p->dead); samples++;
    if(sample_error) return sample_error; state->md32_enabled=hw_mcu; state->tx_disabled=hw_disabled; return 0;
}
static int power_error, power_reads, power_bad_health;
static int en7573_rx_power(struct en7573_io *io,u32 *value)
{
    struct q1000k_pon *p=io->ctx;
    assert(p->lock.held && p->leased && !p->dead); power_reads++;
    if(power_bad_health) hw_mcu=0;
    if(power_error) return power_error;
    *value=19900; return 0;
}
static int en7573_set_tx(struct en7573_io *io,bool enable)
{
    struct q1000k_pon *p=io->ctx; assert(p->lock.held && !p->dead && p->leased); writes++;
    if(tx_error) return tx_error; hw_disabled=!enable; return 0;
}
static int pon_off(struct q1000k_pon *p) { assert(p->lock.held); off++; p->initialized=false; p->mode=off_error ? -2 : -1; p->tx_enabled=false; return off_error; }
static int gpiod_get_value_cansleep(int *gpio) { assert(gpio); los_calls++; return los_value; }
static int post_calls;
static int en7573_oem_post_init(struct en7573_io *io,struct en7573_oem_post *original,bool restore)
{
 struct q1000k_pon *p=io->ctx;
 assert(p->lock.held && p->leased && p->tx_inhibited && !p->tx_enabled);
 post_calls++; original->saved=!restore; return 0;
}
/* PRODUCTION */
static struct q1000k_pon *create(void)
{
    struct q1000k_pon *p=calloc(1,sizeof(*p));
    p->ref.refs=1; p->mode=1; p->initialized=true; p->io.ctx=p; p->los[1]=&los_value;
    hw_mcu=1; hw_disabled=1; samples=writes=off=los_calls=0;
    sample_error=tx_error=off_error=0; pon_registered=p; return p;
}
int main(void)
{
    struct q1000k_pon *p;
    int before;
    bool tx=true;
    u32 power=123;
    assert(PTR_ERR(q1000k_pon_get())==-ENODEV);
    p=create(); atomic_context=1;
    assert(PTR_ERR(q1000k_pon_get())==-EWOULDBLOCK && !samples);
    assert(q1000k_pon_get_tx(p,&tx)==-EWOULDBLOCK && tx && !samples);
    assert(q1000k_pon_get_rx_power(p,&power)==-EWOULDBLOCK && power==123 && !power_reads);
    atomic_context=0;
    assert(q1000k_pon_get_tx(NULL,&tx)==-EINVAL && tx);
    assert(q1000k_pon_get_tx(p,NULL)==-EINVAL);
    assert(q1000k_pon_get_tx(p,&tx)==-EPERM && tx);
    assert(q1000k_pon_get_rx_power(p,&power)==-EPERM && !power_reads);
    assert(q1000k_pon_get_rx_power(p,NULL)==-EINVAL);
    p->initialized=false;
    assert(PTR_ERR(q1000k_pon_get())==-EAGAIN && p->ref.refs==1);
    p->initialized=true;
    assert(q1000k_pon_get()==p && p->leased && p->ref.refs==2 && !writes);
    assert(PTR_ERR(q1000k_pon_get())==-EBUSY && p->ref.refs==2);
    assert(!q1000k_pon_get_tx(p,&tx) && !tx);
    assert(!q1000k_pon_get_rx_power(p,&power) && power==19900 && !writes && !off);
    power=123; power_error=-ENODATA;
    assert(q1000k_pon_get_rx_power(p,&power)==-ENODATA && power==123 && !off && !p->fault);
    power_error=0;
    assert(!q1000k_pon_check(p));
    assert(!q1000k_pon_set_tx(p,true) && p->tx_enabled && !hw_disabled);
    assert(!q1000k_pon_check(p));
    assert(!q1000k_pon_get_tx(p,&tx) && tx);
    los_value=1; assert(q1000k_pon_get_los(p)==1);
    los_value=0; assert(q1000k_pon_get_los(p)==0);
    assert(!q1000k_pon_put(p) && !p->leased && !p->tx_enabled && hw_disabled && p->ref.refs==1);
    assert(q1000k_pon_put(p)==-EPERM && p->ref.refs==1);
    assert(q1000k_pon_get()==p);
    before=freed; pon_unpublish(p); pon_drop_device_ref(p);
    assert(!pon_registered && p->dead && p->ref.refs==1 && freed==before);
    samples=writes=los_calls=power_reads=0;
    assert(q1000k_pon_check(p)==-ENODEV);
    assert(q1000k_pon_get_tx(p,&tx)==-ENODEV && tx);
    assert(q1000k_pon_get_rx_power(p,&power)==-ENODEV && power==123 && !power_reads);
    assert(q1000k_pon_set_tx(p,true)==-ENODEV);
    assert(q1000k_pon_get_los(p)==-ENODEV);
    assert(q1000k_pon_put(p)==-ENODEV && freed==before+1);
    assert(!samples && !writes && !los_calls);
    p=create(); assert(q1000k_pon_get()==p); tx_error=-EREMOTEIO;
    assert(q1000k_pon_set_tx(p,true)==-EREMOTEIO && off==1 && p->fault==-EREMOTEIO);
    assert(strstr(last_diagnostic,"enable=1 phase=write-readback error=-121 prior_fault=0"));
    assert(q1000k_pon_check(p)==-EREMOTEIO && off==2);
    assert(q1000k_pon_put(p)==-EREMOTEIO);
    pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); hw_disabled=0;
    tx=true; assert(q1000k_pon_get_tx(p,&tx)==-EIO && tx && off==1 && !p->initialized);
    assert(q1000k_pon_put(p)==-EIO); pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); los_value=-ETIMEDOUT;
    assert(q1000k_pon_get_los(p)==-ETIMEDOUT && off==1);
    assert(q1000k_pon_put(p)==-ETIMEDOUT); pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); hw_mcu=0;
    before=health_diagnostics;
    assert(q1000k_pon_set_tx(p,false)==-EIO && !writes && off==1 && p->fault==-EIO);
    assert(health_diagnostics==(unsigned int)before+1);
    assert(strstr(last_diagnostic,"enable=0 phase=check error=-5 prior_fault=0"));
    assert(q1000k_pon_put(p)==-EIO); pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); off_error=-EIO; tx_error=-ETIMEDOUT;
    assert(q1000k_pon_set_tx(p,false)==-ETIMEDOUT && p->fault==-ETIMEDOUT && p->mode==-2);
    assert(q1000k_pon_put(p)==-ETIMEDOUT); pon_unpublish(p); pon_drop_device_ref(p);
    for(int kind=0;kind<3;kind++) {
        p=create(); assert(q1000k_pon_get()==p); power=123;
        power_error=kind==0 ? -EREMOTEIO : kind==1 ? -ENODATA : 0;
        power_bad_health=kind!=0;
        int error=kind==0 ? -EREMOTEIO : -EIO;
        assert(q1000k_pon_get_rx_power(p,&power)==error && power==123 && off==1 && p->fault==error);
        assert(q1000k_pon_put(p)==error); pon_unpublish(p); pon_drop_device_ref(p);
    }
    power_error=power_bad_health=0;
    p=create(); p->tx_inhibited=true;
    assert(q1000k_pon_set_tx(p,true)==-EPERM && !writes && !off);
    assert(q1000k_pon_get()==p);
    assert(!q1000k_pon_oem_post_init(p,false) && post_calls==1 && p->oem_post_original.saved);
    assert(!q1000k_pon_oem_post_init(p,true) && post_calls==2 && !p->oem_post_original.saved);
    atomic_context=1;
    assert(q1000k_pon_oem_post_init(p,false)==-EWOULDBLOCK && post_calls==2);
    atomic_context=0;
    bool inhibited=false;
    assert(!q1000k_pon_get_tx_inhibit(p,&inhibited) && inhibited);
    assert(q1000k_pon_get_tx_inhibit(p,NULL)==-EINVAL);
    assert(!q1000k_pon_set_tx(p,false) && writes==1 && hw_disabled);
    atomic_context=1;
    assert(q1000k_pon_set_tx(p,true)==-EWOULDBLOCK && writes==1 && !off);
    atomic_context=0;
    assert(q1000k_pon_set_tx(p,true)==-EPERM && writes==1 && off==1);
    assert(p->fault==-EPERM && !p->initialized && !p->tx_enabled);
    assert(q1000k_pon_set_tx(p,true)==-EPERM && writes==1);
    assert(q1000k_pon_put(p)==-EPERM);
    pon_unpublish(p); pon_drop_device_ref(p);
    assert(freed==10);
    p=create();
    assert(q1000k_pon_oem_post_init(p,false)==-EPERM && post_calls==2);
    assert(q1000k_pon_get()==p);
    assert(q1000k_pon_oem_post_init(p,false)==-EACCES && post_calls==2 && off==1);
    assert(q1000k_pon_put(p)==-EACCES); pon_unpublish(p); pon_drop_device_ref(p);
    return 0;
}
