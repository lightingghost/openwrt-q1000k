// SPDX-License-Identifier: GPL-2.0-only
/* Exact public class-171 rows from bea6898ee9, parsed by the production agent. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
static u32 get_unaligned_be32(const void *p) {
    const u8 *b=p; return (u32)b[0]<<24 | (u32)b[1]<<16 | (u32)b[2]<<8 | b[3];
}
/* PRODUCTION */
static const u8 rows[][16] = {
    {0xf8,0,0,0,0xe8,0,0x50,0,0xc0,0x0f,0,0,0,0x0f,0x80,2},
    {0xe8,0,0x50,0,0xe8,0,0,0,0xc0,0x0f,0,0,0,0x0f,0x80,3},
    {0xf8,0,0,0,0xf0,0x3d,0x50,0,0xc0,0x0f,0,0,0,0x0f,3,0xd2},
    {0xf8,0,0,0,0x80,0,0x50,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    {0x80,0,0x50,0,0x88,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},
    {0xf8,0,0,0,0x80,0x3d,0x50,0,0x40,0x0f,0,0,0,8,3,0xd2},
    {0x80,0x3d,0x50,0,0x88,0,0,0,0x40,0x0f,0,0,0,9,3,0xd3},
    {0xf8,0,0,0,0xf0,0x3d,0x50,0,0,0x0f,0,0,0,0,3,0xd2},
    {0xf8,0,0,0,0x80,0,0x50,0,0x40,0x0f,0,0,0,8,3,0xd2},
    {0x80,0,0x50,0,0x88,0,0,0,0x40,0x0f,0,0,0,9,3,0xd3},
};
int main(void)
{
    struct omci_service_config s={.vlan_treatment_valid=true,
        .vlan_input_tpid=0x8100,.vlan_output_tpid=0x8100};
    struct q1000k_vlan_program p, before;
    struct q1000k_vlan_frame in={.ethertype=0x0800},out,back;
    for(unsigned int policy=0;policy<3;policy++) for(unsigned int i=0;i<10;i++) {
        omci_ext_vlan_parse_rule(&s.vlan_rule,rows[i]);
        memset(&p,0xa5,sizeof(p)); before=p;
        int ret=q1000k_vlan_compile_policy(&s,&p,policy);
        if(i==3 || i==4 || (i==7 && !policy)) {
            assert(ret==-EINVAL && !memcmp(&p,&before,sizeof(p))); continue;
        }
        assert(!ret && !memcmp(s.vlan_rule.raw,rows[i],16));
        if(i==7) {
            in.count=0;
            assert(!q1000k_vlan_apply(&p,true,&in,&out));
            assert(out.count==1 && out.tag[0].tpid==0x8100 && out.tag[0].tci==122);
            assert(!q1000k_vlan_apply(&p,false,&out,&back) && !back.count);
            out.tag[0].tci|=0x1000;
            assert(q1000k_vlan_apply(&p,false,&out,&back)==-ENOENT);
        } else if(i==5 || i==6 || i==8 || i==9) {
            /* Preserve every PCP/DEI combination. Double-tag treatment
             * changes the outer tag and retains the complete inner tag. */
            for(unsigned int bits=0;bits<16;bits++) {
                in.count=(i==6 || i==9)?2:1;
                in.tag[0]=(struct q1000k_vlan_tag){0x8100,(bits<<12)|((i==5||i==6)?122:0)};
                in.tag[1]=(struct q1000k_vlan_tag){0x88a8,0xb456};
                assert(!q1000k_vlan_apply(&p,true,&in,&out));
                assert(out.count==in.count && out.tag[0].tpid==0x8100 && out.tag[0].tci==((bits<<12)|122));
                if(in.count==2) assert(!memcmp(&out.tag[1],&in.tag[1],sizeof(in.tag[1])));
                assert(!q1000k_vlan_apply(&p,false,&out,&back));
                assert(back.count==in.count && !memcmp(back.tag,in.tag,in.count*sizeof(in.tag[0])));
            }
        }
    }
    omci_ext_vlan_parse_rule(&s.vlan_rule,rows[7]);
    s.vlan_output_tpid=0x88a8;
    for(unsigned int mode=0;mode<8;mode++) {
        s.vlan_rule.treat_inner_tpid_dei=mode;
        int ret=q1000k_vlan_compile_policy(&s,&p,2);
        if(mode==5) { assert(ret==-EINVAL); continue; }
        assert(!ret); in.count=0;
        assert(!q1000k_vlan_apply(&p,true,&in,&out));
        assert(out.tag[0].tpid==(mode==4?0x8100:0x88a8) && out.tag[0].tci==122);
        ret=q1000k_vlan_compile_policy(&s,&p,1);
        if(mode<2) { assert(ret==-EINVAL); continue; }
        assert(!ret && !q1000k_vlan_apply(&p,true,&in,&out));
        assert(out.tag[0].tci==((mode==7?0x1000:0)|122));
    }
    s.vlan_rule.treat_inner_tpid_dei=2;
    s.vlan_rule.treat_inner_pbit=8;
    assert(q1000k_vlan_compile_policy(&s,&p,1)==-EINVAL);
    assert(q1000k_vlan_compile_policy(&s,&p,2)==-EINVAL);
    s.vlan_rule.treat_inner_pbit=0; s.vlan_rule.treat_inner_vid=4096;
    assert(q1000k_vlan_compile_policy(&s,&p,1)==-EINVAL);
    assert(q1000k_vlan_compile_policy(&s,&p,2)==-EINVAL);
    assert(q1000k_vlan_compile_policy(&s,&p,3)==-EINVAL);
    return 0;
}
