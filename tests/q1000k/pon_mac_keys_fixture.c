// SPDX-License-Identifier: GPL-2.0-only
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U<<(n))
#define GFP_KERNEL 0
#define Q1000K_TABLE_INSTALL 2
#define Q1000K_TABLE_ACTIVATE 3
static void udelay(unsigned int usec) { assert(usec==1); }
static void *kzalloc(size_t n,int flags) { void *p=kmalloc(n,flags); if(p) memset(p,0,n); return p; }
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static u32 registers[0x6000/4];
static int phase, writes, fail_write, provider_error;
static bool q1000k_protocol_owned(void) { return true; }
static int q1000k_pipeline_table_context(int wanted) { return wanted==phase ? 0 : -EPERM; }
static int an7581_xpon_status(void) { return provider_error; }
static void set_xpon_data(u32 reg,u32 value) { assert(reg<0x6000 && !(reg&3)); writes++; registers[reg/4]=value^(writes==fail_write); }
static u32 get_xpon_data(u32 reg) { assert(reg<0x6000 && !(reg&3)); return registers[reg/4]; }
#define module_param(...) /* module parameter */
#define MODULE_PARM_DESC(...) /* description */
#define q1000k_activation_snapshot(...) ((void)0)
/* PRODUCTION */
int main(void)
{
    struct crypto_lskcipher cipher;
    struct q1000k_mac_keys keys,before;
    struct q1000k_auth_keys expected;
    u8 reg[36]={0},sn[8]={1,2,3,4,5,6,7,8},tag[8]={9,10,11,12,13,14,15,16},msk[16];
    u8 pik=4,oik=5;
    memset(&keys,0xa5,sizeof(keys)); before=keys;
    alloc_fail=1;
    assert(q1000k_mac_keys_derive(&cipher,reg,sn,tag,&keys)==-ENOMEM);
    assert(!memcmp(&keys,&before,sizeof(keys))); alloc_fail=0;
    for(int n=1;n<=9;n++) {
        crypto_calls=0; crypto_fail=n;
        assert(q1000k_mac_keys_derive(&cipher,reg,sn,tag,&keys)==-EIO);
        assert(!memcmp(&keys,&before,sizeof(keys)) && !live_alloc);
    }
    crypto_fail=0;
    assert(!q1000k_mac_keys_derive(&cipher,reg,sn,tag,&keys));
    assert(!q1000k_auth_registration(&cipher,reg,sn,tag,&expected));
    assert(!memcmp(&expected,&keys.bank[0],sizeof(expected)));
    memset(msk,0x55,16);
    assert(!q1000k_auth_derive(&cipher,msk,sn,tag,&expected));
    memset(expected.ploam,0x55,16);
    assert(!memcmp(&expected,&keys.bank[1],sizeof(expected)));
    assert(!memcmp(keys.pon_tag,tag,8) && !live_alloc);
    assert(q1000k_mac_keys_install(&keys)==-EPERM && !writes);
    phase=2; registers[0x5800/4]=0xfffffff7;
    assert(!q1000k_mac_keys_install(&keys) && writes==27);
    assert(registers[0x5800/4]==0xffffffe7);
    assert(registers[0x53c0/4]==0x0d0e0f10 && registers[0x53c4/4]==0x090a0b0c);
    for(unsigned int bank=0;bank<2;bank++) for(unsigned int word=0;word<4;word++) {
        assert(registers[(0x5360+16*bank)/4+word]==get_unaligned_be32(keys.bank[bank].ploam+12-4*word));
        assert(registers[(0x5380+16*bank)/4+word]==get_unaligned_be32(keys.bank[bank].omci+12-4*word));
        assert(registers[(0x53a0+16*bank)/4+word]==get_unaligned_be32(keys.bank[bank].kek+12-4*word));
    }
    for(int n=1;n<=27;n++) {
        writes=0; fail_write=n; registers[0x5800/4]=0;
        assert(q1000k_mac_keys_install(&keys)==-EIO && writes==n);
    }
    fail_write=0; writes=0; registers[0x5800/4]=~0U;
    assert(q1000k_mac_keys_install(&keys)==-EIO && !writes);
    registers[0x5800/4]=0; memset(keys.bank[0].omci,0xff,16);
    assert(!q1000k_mac_keys_install(&keys));
    assert(q1000k_mac_keys_match(&keys)==1);
    for(unsigned int reg=0x5360;reg<0x53c0;reg+=4) {
        registers[reg/4]^=1;
        assert(q1000k_mac_keys_match(&keys)==0);
        registers[reg/4]^=1;
    }
    provider_error=-ENODEV; writes=0;
    assert(q1000k_mac_keys_match(&keys)==-ENODEV && !writes);
    assert(q1000k_mac_keys_install(&keys)==-ENODEV && !writes);
    assert(q1000k_mac_key_indices(&pik,&oik)==-ENODEV && pik==4 && oik==5);
    provider_error=0; registers[0x5318/4]=~0U;
    assert(q1000k_mac_key_indices(&pik,&oik)==-EIO && pik==4 && oik==5);
    for(unsigned int n=0;n<4;n++) {
        registers[0x5318/4]=(n&1)|((n>>1)<<16);
        assert(!q1000k_mac_key_indices(&pik,&oik) && pik==(n&1) && oik==(n>>1));
    }
    writes=0; phase=0;
    assert(q1000k_mac_onu_install(17)==-EPERM && !writes);
    phase=2;
    for(unsigned int id=1021;id<65535;id++)
        assert(q1000k_mac_onu_install(id)==-EINVAL && !writes);
    registers[0x5014/4]=0x55555555;
    assert(!q1000k_mac_onu_install(0) && registers[0x5014/4]==0x5555d400);
    assert(!q1000k_mac_onu_install(1020) && registers[0x5014/4]==0x5555d7fc);
    assert(!q1000k_mac_onu_install(0xffff) && registers[0x5014/4]==0x555557ff);
    writes=0; fail_write=1;
    assert(q1000k_mac_onu_install(18)==-EIO && writes==1);
    fail_write=0; registers[0x5014/4]=~0U; writes=0;
    assert(q1000k_mac_onu_install(18)==-EIO && !writes);
    return 0;
}
