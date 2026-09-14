// SPDX-License-Identifier: GPL-2.0-only
static struct q1000k_gwan_table before_table, desired_table, stale_table;
static void two_services(void)
{
    reset_model(); create_ready(500,4,200,7); create_ready(600,6,201,8);
    queue_model[0]=0xfe; queue_model[4]=0xf0; queue_model[6]=0x00;
    assert(!q1000k_gwan_snapshot(&before_table));
}
int main(void)
{
    struct q1000k_gwan_binding b;
    unsigned int steps;
    reset_model();
    assert(!gwan_create_new_tcont(200));
    assert(wan.gpon.allocId[1]==200 && wan.activeChannelNum==2 && tcont_model[0]==17);
    assert(rx_channels==(BIT(0)|BIT(1)) && queue_model[1]==255);
    assert(gwan_create_new_tcont(200)==-EEXIST);
    assert(gwan_remove_tcont(17)==-ENOENT && wan.gpon.allocId[0]==17);
    assert(!gwan_create_new_gemport(500,1,0,200));
    assert(!gwan_config_gemport(500,ENUM_CFG_NETIDX,7));
    assert(!gwan_remove_tcont(200));
    assert(wan.gpon.allocId[1]==0xffff && wan.activeChannelNum==1 && rx_channels==BIT(0));
    assert(hardware[500] && wan.gpon.gemPort[0].info.channel==33);
    assert(q1000k_gwan_binding(500,true,&b)==-ENODATA);
    assert(!gwan_create_new_tcont(200));
    assert(!q1000k_gwan_binding(500,true,&b) && b.channel==1);
    assert(queue_model[1]==255 && tcont_model[1]==200);
    assert(!gwan_remove_all_tcont());
    assert(wan.activeChannelNum==1 && wan.gpon.allocId[0]==17 && hardware[500]);

    two_services(); desired_table=before_table; desired_table.gem[0].gem=501;
    assert(!q1000k_gwan_apply(&before_table,&desired_table));
    assert(!hardware[500] && hardware[501] && hardware[600]);
    assert(q1000k_gwan_binding(500,true,&b)==-ENOENT);
    assert(!q1000k_gwan_binding(501,true,&b) && b.channel==4 && b.ani==7);
    assert(queue_model[0]==0xfe && queue_model[4]==0xf0 && queue_model[6]==0);
    assert(rx_channels==(BIT(0)|BIT(4)|BIT(6)) && tcont_model[0]==17);
    steps=physical_ops;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-ESTALE && physical_ops==(int)steps);
    assert(!q1000k_gwan_apply(&desired_table,&desired_table) && physical_ops==(int)steps);

    two_services();
    wan.gpon.gemPort[250].info=(GWAN_GemInfo_T){.valid=1,.portId=17,.allocId=17,.ani=0x1ff,.channel=0,.rxEncrypt=1};
    wan.gpon.gemIdToIndex[17]=250|0x8000; wan.gpon.gemNumbers++; hardware[17]=true;
    assert(!gwan_remove_gemport(500));
    assert(wan.gpon.gemIdToIndex[17]==(250|0x8000) && wan.gpon.gemPort[250].info.rxEncrypt==1);
    assert(!wan.gpon.gemPort[250].info.txEncrypt && hardware[17] && wan.gpon.gemNumbers==2);
    assert(!q1000k_gwan_snapshot(&before_table)); desired_table=before_table;
    desired_table.gem[250].valid=false;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-EPERM);

    two_services(); desired_table=before_table; desired_table.alloc_id[4]=202;
    desired_table.gem[0].alloc_id=202;
    assert(!q1000k_gwan_apply(&before_table,&desired_table));
    assert(queue_model[4]==255 && queue_model[6]==0 && tcont_model[4]==202);
    two_services(); desired_table=before_table; desired_table.alloc_id[0]=18;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-EPERM && !physical_ops);
    desired_table=before_table; desired_table.alloc_id[5]=200;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-EEXIST && !physical_ops);
    desired_table=before_table; desired_table.gem[1].gem=500;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-EEXIST && !physical_ops);
    desired_table=before_table; desired_table.gem[0].alloc_id=202;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-ESTALE && !physical_ops);
    desired_table=before_table; desired_table.gem[0].encrypted=true;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-EOPNOTSUPP && !physical_ops);
    desired_table=before_table; desired_table.gem[0].ani=257;
    assert(q1000k_gwan_apply(&before_table,&desired_table)==-EINVAL && !physical_ops);
    assert(q1000k_gwan_apply(NULL,&desired_table)==-EINVAL);

    two_services(); assert(!gwan_remove_gemport(500)); steps=physical_ops;
    assert(steps>70 && !hardware[500] && hardware[600]);
    for(unsigned int n=1;n<=steps;n++) {
        two_services(); physical_fail=n;
        int ret=gwan_remove_gemport(500);
        assert(ret==(n<=32 ? -ETIMEDOUT : -EUCLEAN));
        assert(physical_ops==(int)n && !q1000k_gwan_changing && !protocol_owned);
        assert(!atomic_load(&q1000k_tcont_config_busy));
        if(n<=32) {
            assert(!q1000k_gwan_snapshot(&desired_table));
            assert(q1000k_gwan_table_equal(&desired_table,&before_table));
            assert(!protocol_error && hardware[500] && hardware[600]);
        } else {
            assert(protocol_error==-ETIMEDOUT && q1000k_gwan_error==-ETIMEDOUT);
            assert(q1000k_gwan_binding(600,true,&b)==-ETIMEDOUT);
            assert(gwan_create_new_gemport(700,6,0,201)==-ETIMEDOUT);
            for(unsigned int i=0;i<32;i++) assert(queue_model[i]==255);
        }
    }
    two_services(); async_protocol_fault_step=34;
    assert(gwan_remove_gemport(500)==-EUCLEAN && protocol_error==-ENOSPC);
    assert(physical_phase==2); /* No activation after a lost protocol event. */
    return 0;
}
