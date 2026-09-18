#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1u<<(n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define IS_ENABLED(x) 0
static int atomic_context,locked,contained,install_calls,enable_calls,fail_enable;
static int in_interrupt(void) { return 0; }
static int in_atomic(void) { return atomic_context; }
static int irqs_disabled(void) { return 0; }
static int rcu_preempt_depth(void) { return 0; }
static int rcu_read_lock_held(void) { return 0; }
struct task_struct { int id; };
static struct task_struct task1,task2,*current=&task1;
static void mutex_lock(int *lock) { assert(!locked); locked=1; }
static void mutex_unlock(int *lock) { assert(locked); locked=0; }
/* HEADER */
static int q1000k_pipeline_lock;
static struct q1000k_pipeline_status q1000k_pipeline;
static struct task_struct *q1000k_table_owner;
static enum q1000k_table_phase q1000k_table_phase;
static void q1000k_pipeline_contain(void) { assert(locked && q1000k_pipeline.error); contained++; }
static int q1000k_transport_set_tx_channel(unsigned ch,bool on) {
    assert(locked && !q1000k_table_owner && on && ch==7);
    enable_calls++; return fail_enable ? -EIO : 0;
}
/* PRODUCTION */
static int install_error;
static int install(void *arg) {
    assert(locked && arg==&task1 && !q1000k_pipeline_table_context(Q1000K_TABLE_APPEND));
    assert(q1000k_pipeline_table_context(Q1000K_TABLE_INSTALL)==-EPERM);
    current=&task2; assert(q1000k_pipeline_table_context(Q1000K_TABLE_APPEND)==-EPERM); current=&task1;
    install_calls++; return install_error;
}
static void reset(void) {
    memset(&q1000k_pipeline,0,sizeof(q1000k_pipeline)); q1000k_pipeline.channels=1;
    contained=install_calls=enable_calls=fail_enable=install_error=0;
}
int main(void) {
    reset();
    assert(q1000k_pipeline_append(NULL,&task1,1)==-EINVAL);
    assert(q1000k_pipeline_append(install,&task1,2)==-EINVAL);
    atomic_context=1; assert(q1000k_pipeline_append(install,&task1,1)==-EWOULDBLOCK); atomic_context=0;
    q1000k_pipeline.stage=Q1000K_PIPELINE_PREPARED;
    assert(q1000k_pipeline_append(install,&task1,1)==-EBUSY && !install_calls);
    reset(); q1000k_pipeline.channels=3;
    assert(q1000k_pipeline_append(install,&task1,1)==-EBUSY && !install_calls);
    reset(); assert(!q1000k_pipeline_append(install,&task1,1));
    assert(install_calls==1 && !enable_calls && !contained && q1000k_pipeline.channels==1);
    reset(); assert(!q1000k_pipeline_append(install,&task1,0x81));
    assert(install_calls==1 && enable_calls==1 && !contained && q1000k_pipeline.channels==0x81);
    for(int fail=0;fail<2;fail++) {
        reset(); if(fail) fail_enable=1; else install_error=-EIO;
        assert(q1000k_pipeline_append(install,&task1,0x81)==-EIO);
        assert(contained==1 && q1000k_pipeline.error==-EIO && !q1000k_table_owner);
        assert(q1000k_pipeline.channels==1 && enable_calls==fail);
        assert(q1000k_pipeline_append(install,&task1,0x81)==-EIO && install_calls==1);
    }
    assert(!locked);
    return 0;
}
