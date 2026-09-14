// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* PRODUCTION */
static struct omci_service_config service(void)
{
    struct omci_service_config s={.vlan_treatment_valid=true,
        .vlan_input_tpid=0x88a8,.vlan_output_tpid=0x88a8,
        .vlan_rule={.filter_outer_pbit=15,.filter_inner_pbit=15,
            .treat_outer_pbit=15,.treat_inner_pbit=0,.treat_inner_vid=123,
            .treat_inner_tpid_dei=4}};
    return s;
}
static void filter_test(void)
{
    struct omci_vlan_tagging_filter f={.valid=true,.num_entries=1,.entries={{.tci=0xb07b}}};
    struct q1000k_vlan_frame frame={.count=1,.tag={{0x8100,0xb07b}}};
    const u8 vid_modes[]={3,4,15,16,28,29};
    const u8 pcp_modes[]={7,8,17,18,30,31};
    const u8 tci_modes[]={11,12,19,20,32,33};
    for(unsigned int m=0;m<6;m++) for(unsigned int tci=0;tci<65536;tci++) {
        frame.tag[0].tci=tci;
        f.forward_operation=vid_modes[m];
        assert(q1000k_vlan_filter_apply(&f,true,&frame)==((tci&4095)==123?0:-ENOENT));
        assert(q1000k_vlan_filter_apply(&f,false,&frame)==((tci&4095)==123?0:-ENOENT));
        f.forward_operation=pcp_modes[m];
        assert(q1000k_vlan_filter_apply(&f,true,&frame)==((tci>>13)==5?0:-ENOENT));
        f.forward_operation=tci_modes[m];
        assert(q1000k_vlan_filter_apply(&f,false,&frame)==(tci==0xb07b?0:-ENOENT));
    }
    f.forward_operation=5; frame.tag[0].tci=123;
    assert(!q1000k_vlan_filter_apply(&f,true,&frame));
    assert(q1000k_vlan_filter_apply(&f,false,&frame)==-ENOENT);
    frame.tag[0].tci=124;
    assert(!q1000k_vlan_filter_apply(&f,true,&frame) && !q1000k_vlan_filter_apply(&f,false,&frame));
    f.forward_operation=0x10; f.entries[0].tci=0; frame.tag[0].tci=0xf000;
    assert(!q1000k_vlan_filter_apply(&f,true,&frame)); /* VID zero, independent of PCP and DEI. */
    frame.count=0; assert(q1000k_vlan_filter_apply(&f,true,&frame)==-ENOENT);
    f.forward_operation=0xf; assert(!q1000k_vlan_filter_apply(&f,false,&frame));
    f.forward_operation=0; assert(!q1000k_vlan_filter_apply(&f,true,&frame));
    frame.count=1; assert(!q1000k_vlan_filter_apply(&f,true,&frame));
    f.forward_operation=1; assert(q1000k_vlan_filter_apply(&f,true,&frame)==-ENOENT);
    f.forward_operation=0x10; f.num_entries=0;
    assert(q1000k_vlan_filter_apply(&f,true,&frame)==-ENOENT);
    f.num_entries=12; f.entries[11].tci=123; frame.tag[0].tci=123;
    assert(!q1000k_vlan_filter_apply(&f,true,&frame));
    frame.count=2; frame.tag[1].tci=124;
    assert(!q1000k_vlan_filter_apply(&f,true,&frame)); /* Outer bridge VLAN, not inner VID. */
    frame.tag[0].tci=124; frame.tag[1].tci=123;
    assert(q1000k_vlan_filter_apply(&f,true,&frame)==-ENOENT);
    f.num_entries=13; assert(q1000k_vlan_filter_validate(&f)==-EINVAL);
    f.num_entries=1;
    for(unsigned int mode=0x16;mode<=0x1b;mode++) {
        f.forward_operation=mode; assert(q1000k_vlan_filter_validate(&f)==-EOPNOTSUPP);
    }
    f.forward_operation=0x22; assert(q1000k_vlan_filter_validate(&f)==-EINVAL);
    f.valid=false; assert(!q1000k_vlan_filter_apply(&f,false,&frame));
}
int main(void)
{
    filter_test();
    struct omci_service_config s=service();
    struct q1000k_vlan_program p, old;
    struct q1000k_vlan_frame in={.ethertype=0x0800}, out, back;
    assert(!q1000k_vlan_compile(&s,&p));
    assert(!q1000k_vlan_apply(&p,true,&in,&out));
    assert(out.count==1 && out.tag[0].tpid==0x8100 && out.tag[0].tci==123 && out.ethertype==in.ethertype);
    assert(!q1000k_vlan_apply(&p,false,&out,&back) && !back.count);
    out.tag[0].tci=124; assert(q1000k_vlan_apply(&p,false,&out,&back)==-ENOENT);
    in.count=1; in.tag[0]=(struct q1000k_vlan_tag){0x8100,0};
    assert(q1000k_vlan_apply(&p,true,&in,&out)==-ENOENT); /* Priority-tagged != untagged. */
    s.vlan_rule.filter_inner_pbit=8; s.vlan_rule.filter_inner_vid=0;
    s.vlan_rule.tags_to_remove=1; s.vlan_rule.treat_inner_pbit=8;
    assert(!q1000k_vlan_compile(&s,&p));
    for(unsigned int tci=0;tci<65536;tci++) {
        in.tag[0].tci=tci;
        int ret=q1000k_vlan_apply(&p,true,&in,&out);
        if(tci&4095) assert(ret==-ENOENT);
        else {
            assert(!ret && out.tag[0].tci==((tci&0xe000)|123));
            assert(!q1000k_vlan_apply(&p,false,&out,&back));
            assert(back.count==1 && back.tag[0].tci==(tci&0xe000));
        }
    }
    /* A wildcard VID translates to a provisioned optical VID; inverse sets
     * the lost VID to zero while preserving PCP and copied DEI.
     */
    s.vlan_rule.filter_inner_vid=4096; s.vlan_rule.treat_inner_tpid_dei=0;
    assert(!q1000k_vlan_compile(&s,&p));
    for(unsigned int tci=0;tci<65535;tci++) {
        in.tag[0].tci=tci;
        assert(!q1000k_vlan_apply(&p,true,&in,&out));
        assert(!q1000k_vlan_apply(&p,false,&out,&back));
        assert(back.tag[0].tci==(tci&0xf000));
    }
    /* Push one tag above a retained inner tag, including configured TPID. */
    s.vlan_rule.tags_to_remove=0; s.vlan_rule.treat_inner_tpid_dei=6;
    assert(!q1000k_vlan_compile(&s,&p));
    in.tag[0].tci=0xb321;
    assert(!q1000k_vlan_apply(&p,true,&in,&out));
    assert(out.count==2 && out.tag[0].tpid==0x88a8 && out.tag[0].tci==0xa07b && out.tag[1].tci==0xb321);
    assert(!q1000k_vlan_apply(&p,false,&out,&back) && back.count==1 && back.tag[0].tci==in.tag[0].tci);
    out.tag[0].tci ^= 0x2000; /* Inconsistent duplicate PCP copy. */
    assert(q1000k_vlan_apply(&p,false,&out,&back)==-ENOENT);
    /* Replace two tags, swapping the copied inner/outer fields. */
    s.vlan_rule.filter_outer_pbit=8; s.vlan_rule.filter_outer_vid=4096;
    s.vlan_rule.tags_to_remove=2; s.vlan_rule.treat_outer_pbit=8;
    s.vlan_rule.treat_outer_vid=4096; s.vlan_rule.treat_outer_tpid_dei=0;
    s.vlan_rule.treat_inner_pbit=9; s.vlan_rule.treat_inner_vid=4097; s.vlan_rule.treat_inner_tpid_dei=1;
    assert(!q1000k_vlan_compile(&s,&p));
    in.count=2; in.tag[0]=(struct q1000k_vlan_tag){0x88a8,0xd456}; in.tag[1]=(struct q1000k_vlan_tag){0x8100,0x7123};
    assert(!q1000k_vlan_apply(&p,true,&in,&out));
    assert(!memcmp(&out.tag[0],&in.tag[1],sizeof(in.tag[0])) && !memcmp(&out.tag[1],&in.tag[0],sizeof(in.tag[0])));
    assert(!q1000k_vlan_apply(&p,false,&out,&back) && !memcmp(in.tag,back.tag,sizeof(in.tag)));
    for(unsigned int eth=0;eth<65536;eth++) for(unsigned int filter=0;filter<=5;filter++) {
        p.ethertype=filter; in.ethertype=eth;
        bool match=filter==0 || (filter==1 && eth==0x0800) || (filter==2 && (eth==0x8863||eth==0x8864)) ||
            (filter==3 && eth==0x0806) || (filter==4 && eth==0x86dd) || (filter==5 && eth==0x888e);
        assert(q1000k_vlan_apply(&p,true,&in,&out)==(match ? 0 : -ENOENT));
    }
    s=service(); s.vlan_rule.tags_to_remove=3;
    assert(!q1000k_vlan_compile(&s,&p)); in.count=0;
    assert(q1000k_vlan_apply(&p,true,&in,&out)==-EPERM);
    assert(q1000k_vlan_apply(&p,false,&in,&out)==-ENOENT);
    s.vlan_downstream_mode=1; assert(!q1000k_vlan_compile(&s,&p));
    assert(!q1000k_vlan_apply(&p,false,&in,&out) && out.ethertype==in.ethertype);
    s=service(); s.vlan_rule.filter_inner_pbit=14;
    s.vlan_rule.treat_inner_pbit=15;
    assert(!q1000k_vlan_compile(&s,&p)); in.count=1;
    assert(!q1000k_vlan_apply(&p,true,&in,&out));
    assert(q1000k_vlan_apply(&p,false,&out,&back)==-ENOENT); /* No upstream default in inverse. */
    old=p;
    s.vlan_rule.treat_inner_pbit=10;
    assert(q1000k_vlan_compile(&s,&p)==-EOPNOTSUPP && !memcmp(&old,&p,sizeof(p)));
    s=service(); s.vlan_downstream_mode=2;
    assert(q1000k_vlan_compile(&s,&p)==-EOPNOTSUPP);
    s=service(); s.vlan_rule.tags_to_remove=1;
    assert(q1000k_vlan_compile(&s,&p)==-EINVAL); /* Cannot pop an absent tag. */
    s=service(); s.vlan_rule.treat_inner_pbit=8;
    assert(q1000k_vlan_compile(&s,&p)==-EINVAL); /* Cannot copy an absent tag. */
    s=service(); s.vlan_rule.treat_inner_vid=4095;
    assert(q1000k_vlan_compile(&s,&p)==-EINVAL);
    s=service(); s.vlan_rule.filter_inner_pbit=9;
    assert(q1000k_vlan_compile(&s,&p)==-EINVAL);
    s=service(); s.vlan_rule.treat_inner_tpid_dei=5;
    assert(q1000k_vlan_compile(&s,&p)==-EINVAL);
    return 0;
}
