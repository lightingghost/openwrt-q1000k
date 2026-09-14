// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define DEFINE_MUTEX(n) struct mutex n
#define EXPORT_SYMBOL_GPL(n)
#define IS_ENABLED(n) 0
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_err(...) ((void)0)
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
struct en7573_io { void *ctx; };
struct en7573_state { int md32_enabled,tx_disabled; };
struct q1000k_pon {
    struct mutex lock;
    struct kref ref;
    bool dead,leased,tx_enabled,initialized,tx_inhibited;
    int fault,mode,last_error;
    const char *stage;
    unsigned char calibration[513];
    struct en7573_io io;
    int *los[2];
};
static int samples,writes,off,los_calls,sample_error,tx_error,off_error,los_value,hw_mcu,hw_disabled;
static int en7573_sample_state(struct en7573_io *io,struct en7573_state *state)
{
    struct q1000k_pon *p=io->ctx; assert(p->lock.held && !p->dead); samples++;
    if(sample_error) return sample_error; state->md32_enabled=hw_mcu; state->tx_disabled=hw_disabled; return 0;
}
static int en7573_set_tx(struct en7573_io *io,bool enable)
{
    struct q1000k_pon *p=io->ctx; assert(p->lock.held && !p->dead && p->leased); writes++;
    if(tx_error) return tx_error; hw_disabled=!enable; return 0;
}
static int pon_off(struct q1000k_pon *p) { assert(p->lock.held); off++; p->initialized=false; p->mode=off_error ? -2 : -1; p->tx_enabled=false; return off_error; }
static int gpiod_get_value_cansleep(int *gpio) { assert(gpio); los_calls++; return los_value; }
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
    assert(PTR_ERR(q1000k_pon_get())==-ENODEV);
    p=create(); atomic_context=1;
    assert(PTR_ERR(q1000k_pon_get())==-EWOULDBLOCK && !samples);
    assert(q1000k_pon_get_tx(p,&tx)==-EWOULDBLOCK && tx && !samples);
    atomic_context=0;
    assert(q1000k_pon_get_tx(NULL,&tx)==-EINVAL && tx);
    assert(q1000k_pon_get_tx(p,NULL)==-EINVAL);
    assert(q1000k_pon_get_tx(p,&tx)==-EPERM && tx);
    p->initialized=false;
    assert(PTR_ERR(q1000k_pon_get())==-EAGAIN && p->ref.refs==1);
    p->initialized=true;
    assert(q1000k_pon_get()==p && p->leased && p->ref.refs==2 && !writes);
    assert(PTR_ERR(q1000k_pon_get())==-EBUSY && p->ref.refs==2);
    assert(!q1000k_pon_get_tx(p,&tx) && !tx);
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
    samples=writes=los_calls=0;
    assert(q1000k_pon_check(p)==-ENODEV);
    assert(q1000k_pon_get_tx(p,&tx)==-ENODEV && tx);
    assert(q1000k_pon_set_tx(p,true)==-ENODEV);
    assert(q1000k_pon_get_los(p)==-ENODEV);
    assert(q1000k_pon_put(p)==-ENODEV && freed==before+1);
    assert(!samples && !writes && !los_calls);
    p=create(); assert(q1000k_pon_get()==p); tx_error=-EREMOTEIO;
    assert(q1000k_pon_set_tx(p,true)==-EREMOTEIO && off==1 && p->fault==-EREMOTEIO);
    assert(q1000k_pon_check(p)==-EREMOTEIO && off==2);
    assert(q1000k_pon_put(p)==-EREMOTEIO);
    pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); hw_disabled=0;
    tx=true; assert(q1000k_pon_get_tx(p,&tx)==-EIO && tx && off==1 && !p->initialized);
    assert(q1000k_pon_put(p)==-EIO); pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); los_value=-ETIMEDOUT;
    assert(q1000k_pon_get_los(p)==-ETIMEDOUT && off==1);
    assert(q1000k_pon_put(p)==-ETIMEDOUT); pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); assert(q1000k_pon_get()==p); off_error=-EIO; tx_error=-ETIMEDOUT;
    assert(q1000k_pon_set_tx(p,false)==-ETIMEDOUT && p->fault==-ETIMEDOUT && p->mode==-2);
    assert(q1000k_pon_put(p)==-ETIMEDOUT); pon_unpublish(p); pon_drop_device_ref(p);
    p=create(); p->tx_inhibited=true;
    assert(q1000k_pon_set_tx(p,true)==-EPERM && !writes && !off);
    assert(q1000k_pon_get()==p);
    assert(!q1000k_pon_set_tx(p,false) && writes==1 && hw_disabled);
    atomic_context=1;
    assert(q1000k_pon_set_tx(p,true)==-EWOULDBLOCK && writes==1 && !off);
    atomic_context=0;
    assert(q1000k_pon_set_tx(p,true)==-EPERM && writes==1 && off==1);
    assert(p->fault==-EPERM && !p->initialized && !p->tx_enabled);
    assert(q1000k_pon_set_tx(p,true)==-EPERM && writes==1);
    assert(q1000k_pon_put(p)==-EPERM);
    pon_unpublish(p); pon_drop_device_ref(p);
    assert(freed==6);
    return 0;
}
