/* Production table transaction under an active authenticated namespace. */
static unsigned initial_installs;
static int initial_install(void *arg) { assert(physical_phase==2); initial_installs++; return 0; }
static void append_reset(unsigned mode)
{
    reset_model(); bench_live_add=mode;
    memset(queue_model,255,sizeof(queue_model)); queue_model[0]=0xfe;
    physical_phase=3; rx_channels=BIT(0);
}
int main(void)
{
    struct q1000k_gwan_table old,next;
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
