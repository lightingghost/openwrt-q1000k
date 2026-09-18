/* Production table transaction under an active authenticated namespace. */
static unsigned initial_installs;
static int initial_install(void *arg) { assert(physical_phase==2); initial_installs++; return 0; }
static void append_reset(unsigned mode)
{
    reset_model(); bench_live_add=mode;
    memset(queue_model,255,sizeof(queue_model)); queue_model[0]=0xfe;
    physical_phase=3; rx_channels=BIT(0);
}
static void rx_policy_setup(struct q1000k_gwan_table *old)
{
    struct q1000k_gwan_table next;
    append_reset(31);
    assert(!q1000k_gwan_add_tcont(448));
    assert(!q1000k_gwan_snapshot(old)); next=*old;
    next.gem[1]=(struct q1000k_gwan_entry){.valid=true,.gem=600,
        .channel=1,.alloc_id=448,.ani=1};
    assert(!q1000k_gwan_apply(old,&next));
    assert(!q1000k_gwan_snapshot(old));
    queue_model[1]=0xfe; physical_ops=0; writes=0; append_calls=0;
}
static void rx_policy_tests(void)
{
    struct q1000k_gwan_table old,next,after;
    struct q1000k_gwan_binding before_binding,after_binding;
    rx_policy_setup(&old);
    assert(!q1000k_gwan_binding(600,true,&before_binding));
    next=old; next.gem[1].rx_encrypted=true;
    assert(!q1000k_gwan_apply(&old,&next));
    assert(append_calls==1 && physical_ops==1 && !writes && !physical_started);
    assert(!producers_drained && !tx_queries && optical_tx);
    assert(queue_model[0]==0xfe && queue_model[1]==0xfe && tcont_model[1]==448);
    assert(!q1000k_gwan_snapshot(&after) && after.gem[1].rx_encrypted);
    assert(!q1000k_gwan_binding(600,true,&after_binding));
    assert(before_binding.gem==after_binding.gem && before_binding.channel==after_binding.channel &&
        before_binding.ani==after_binding.ani && before_binding.alloc_id==after_binding.alloc_id &&
        before_binding.index==after_binding.index && before_binding.multicast==after_binding.multicast);
    assert(q1000k_gwan_apply(&old,&old)==-ESTALE); /* stale CAS never mutates */
    assert(!q1000k_gwan_apply(&after,&old)); /* ring 3 -> 0 also preserves hardware */
    assert(append_calls==2 && !writes && !physical_started);
    /* Control, upstream encryption and an actual rebind retain retirement. */
    for(unsigned variant=0;variant<3;variant++) {
        rx_policy_setup(&old); next=old; next.gem[1].rx_encrypted=true;
        if(variant==0) bench_live_add=15;
        if(variant==1) next.gem[1].encrypted=true;
        if(variant==2) next.gem[1].ani=2;
        assert(!q1000k_gwan_apply(&old,&next));
        assert(!append_calls && physical_started && producers_drained && writes);
    }
    /* A failed command or conflicting hardware contains the transaction;
     * no successful metadata publication and no full-rebuild fallback.
     */
    for(unsigned failure=0;failure<5;failure++) {
        rx_policy_setup(&old); next=old; next.gem[1].rx_encrypted=true;
        if(failure==0) physical_fail=1;
        if(failure==1) hardware[600]=false;
        if(failure==2) encrypted_hardware[600]=true;
        if(failure==3) faulted=true;
        if(failure==4) async_protocol_fault_step=1;
        assert(q1000k_gwan_apply(&old,&next)==-EUCLEAN);
        assert(protocol_error<0 && !writes && !physical_started && append_calls==1);
        assert(!wan.gpon.gemPort[1].info.rxEncrypt);
    }
    rx_policy_setup(&old); next=old; next.gem[1].rx_encrypted=true;
    bench_live_add=32;
    assert(q1000k_gwan_apply(&old,&next)==-EINVAL && !physical_ops);
}
int main(void)
{
    struct q1000k_gwan_table old,next;
    rx_policy_tests();
    append_reset(7);
    assert(!q1000k_gwan_snapshot(&old)); next=old;
    next.alloc_id[1]=448;
    assert(q1000k_gwan_initial_service(&old,&next,initial_install,NULL)==-EAGAIN);
    assert(!initial_installs && !append_calls && !physical_started && !physical_ops);
    next=old; next.alloc_id[0]=99;
    assert(q1000k_gwan_initial_service(&next,&old,initial_install,NULL)==-ESTALE);
    assert(!initial_installs && !physical_started);
    assert(!q1000k_gwan_initial_service(&old,&old,initial_install,NULL));
    assert(initial_installs==1 && append_calls==1 && !physical_started && queue_model[0]==0xfe);
    append_reset(15);
    assert(!q1000k_gwan_snapshot(&old)); next=old; next.alloc_id[1]=448;
    unsigned installed=initial_installs;
    assert(q1000k_gwan_classifier(&old,&next,initial_install,NULL)==-EAGAIN);
    assert(!physical_ops && initial_installs==installed && !append_calls);
    next=old; next.alloc_id[0]=99;
    assert(q1000k_gwan_classifier(&next,&old,initial_install,NULL)==-ESTALE);
    assert(!q1000k_gwan_classifier(&old,&old,initial_install,NULL));
    assert(initial_installs==installed+1 && append_calls==1 && !physical_started);
    bench_live_add=7;
    assert(q1000k_gwan_classifier(&old,&old,initial_install,NULL)==-EAGAIN);
    for(unsigned mode=0;mode<4;mode++) {
        append_reset(mode);
        assert(!q1000k_gwan_snapshot(&old)); next=old;
        next.gem[1]=(struct q1000k_gwan_entry){.valid=true,.gem=65534,
            .channel=33,.alloc_id=0xffff,.ani=256};
        assert(!q1000k_gwan_apply(&old,&next));
        assert(append_calls==!!(mode&1) && physical_started==!(mode&1));
        assert(hardware[65534] && optical_tx && queue_model[0]==0xfe);
        if(mode&1) assert(!producers_drained && !tx_queries);
        append_reset(mode);
        assert(!q1000k_gwan_add_tcont(448));
        assert(append_calls==!!(mode&2) && physical_started==!(mode&2));
        assert(tcont_model[1]==448 && wan.gpon.allocId[1]==448);
        assert(queue_model[1]==255 && queue_model[0]==0xfe && optical_tx);
    }
    /* Old bindings, deletions and install callbacks cannot enter append. */
    append_reset(3);
    assert(!q1000k_gwan_add_tcont(448));
    assert(!q1000k_gwan_snapshot(&old)); next=old;
    next.gem[1]=(struct q1000k_gwan_entry){.valid=true,.gem=600,
        .channel=1,.alloc_id=448,.ani=1};
    assert(!q1000k_gwan_apply(&old,&next) && append_calls==2 && !physical_started);
    assert(!q1000k_gwan_snapshot(&old)); next=old; next.gem[1].ani=2;
    assert(!q1000k_gwan_apply(&old,&next) && append_calls==2 && physical_started);
    assert(!q1000k_gwan_delete_gem(600,false) && append_calls==2 && !hardware[600]);
    /* Existing or conflicting hardware, partial writes, and protocol faults
     * never publish a successful software table or silently fall back.
     */
    for(int failure=1;failure<=2;failure++) {
        append_reset(3); physical_fail=failure;
        assert(q1000k_gwan_add_tcont(448)==-EUCLEAN);
        assert(wan.gpon.allocId[1]==0xffff && protocol_error<0 && append_calls==1);
    }
    append_reset(3); queue_model[1]=0;
    assert(q1000k_gwan_add_tcont(448)==-EUCLEAN && tcont_model[1]==0xffff);
    append_reset(3); async_protocol_fault_step=2;
    assert(q1000k_gwan_add_tcont(448)==-EUCLEAN && wan.gpon.allocId[1]==0xffff);
    append_reset(3);
    assert(!q1000k_gwan_snapshot(&old)); next=old;
    next.gem[1]=(struct q1000k_gwan_entry){.valid=true,.gem=65534,
        .channel=33,.alloc_id=0xffff,.ani=256};
    hardware[65534]=true;
    assert(q1000k_gwan_apply(&old,&next)==-EUCLEAN && !wan.gpon.gemPort[1].info.valid);
    return 0;
}
