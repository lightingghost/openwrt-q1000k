// SPDX-License-Identifier: GPL-2.0-only
typedef uint32_t u32;
typedef uint16_t u16;
#define BIT(n) (1U<<(n))
#define GFP_KERNEL 0
#define Q1000K_TABLE_INSTALL 2
#define Q1000K_TABLE_ACTIVATE 3
static void *kzalloc(size_t n,int flags) { void *p=kmalloc(n,flags); if(p) memset(p,0,n); return p; }
static u32 get_unaligned_be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static u32 registers[0x6000/4];
static int phase, writes, reads, fail_write, fail_read, provider_error, polls, delays, complete_after;
static bool switch_requested;
static int q1000k_pipeline_table_context(int wanted) { return wanted==phase ? 0 : -EPERM; }
static int an7581_xpon_status(void) { return provider_error; }
static void udelay(unsigned int delay) { assert(delay==1); delays++; }
static void set_xpon_data(u32 reg,u32 value) {
    assert((phase==2 || (phase==3 && reg==0x5044)) && reg<0x6000 && !(reg&3));
    if(++writes==fail_write) { provider_error=-EIO; return; }
    if(reg>=0x5210 && reg<=0x522c) assert(!(registers[0x5200/4]&BIT(31)) && !(registers[0x5204/4]&3));
    if(reg==0x5044) { assert(value==BIT(7)); registers[reg/4]&=~value; if(phase==3) switch_requested=false; }
    else registers[reg/4]=value;
    if(reg==0x5200 && (value&BIT(31))) switch_requested=true;
}
static u32 get_xpon_data(u32 reg) {
    assert(reg<0x6000 && !(reg&3));
    if(++reads==fail_read) { provider_error=-EIO; return ~0U; }
    if(phase==3 && reg==0x5044 && switch_requested && ++polls==complete_after) registers[reg/4]|=BIT(7);
    return registers[reg/4];
}
/* PRODUCTION */
static void reset(void) {
    memset(registers,0,sizeof(registers));
    registers[0x5800/4]=0x12345678;
    registers[0x5200/4]=0x87654321;
    registers[0x5204/4]=0xa5a5fff3;
    registers[0x5044/4]=BIT(7)|BIT(0)|BIT(12);
    phase=2; writes=reads=fail_write=fail_read=provider_error=polls=delays=0;
    switch_requested=false; complete_after=3;
}
int main(void)
{
    struct q1000k_mac_data_keys keys={.rx_valid=3,.tx_index=2};
    bool pending=false;
    for(unsigned int i=0;i<32;i++) keys.key[i/16][i%16]=i*7;
    reset(); phase=0;
    assert(q1000k_mac_data_keys_install(&keys,&pending)==-EPERM && !writes);
    assert(q1000k_mac_data_keys_ready(false)==-EPERM && !reads);
    for(unsigned int valid=0;valid<256;valid++) for(unsigned int tx=0;tx<256;tx++) {
        keys.rx_valid=valid; keys.tx_index=tx;
        if(valid<=3 && tx<=2 && (!tx || (valid&BIT(tx-1)))) continue;
        reset(); assert(q1000k_mac_data_keys_install(&keys,&pending)==-EINVAL && !reads && !writes);
    }
    keys.rx_valid=3; keys.tx_index=2; reset();
    assert(q1000k_mac_data_keys_install(NULL,&pending)==-EINVAL);
    assert(q1000k_mac_data_keys_install(&keys,NULL)==-EINVAL);
    assert(!q1000k_mac_data_keys_install(&keys,&pending) && pending);
    int operations=writes, read_operations=reads;
    assert((registers[0x5200/4]&(BIT(31)|BIT(0)))==(BIT(31)|BIT(0)));
    assert(registers[0x5204/4]==0xa5a5fff3 && registers[0x5044/4]==(BIT(0)|BIT(12)));
    assert(registers[0x5800/4]==((0x12345678&~BIT(9))|BIT(12)|BIT(13)));
    for(unsigned int bank=0;bank<2;bank++) for(unsigned int word=0;word<4;word++)
        assert(registers[(0x5210+16*bank)/4+word]==get_unaligned_be32(keys.key[bank]+12-4*word));
    phase=3; assert(!q1000k_mac_data_keys_ready(pending) && polls==3 && delays==2);
    assert(registers[0x5044/4]==(BIT(0)|BIT(12)));
    for(int failure=1;failure<=operations;failure++) {
        reset(); fail_write=failure; pending=false;
        assert(q1000k_mac_data_keys_install(&keys,&pending)==-EIO && writes==failure && !pending);
    }
    for(int failure=1;failure<=read_operations;failure++) {
        reset(); fail_read=failure; pending=false;
        assert(q1000k_mac_data_keys_install(&keys,&pending)==-EIO && reads==failure && !pending);
    }
    reset(); memset(keys.key,0xff,sizeof(keys.key));
    assert(!q1000k_mac_data_keys_install(&keys,&pending)); /* All-ones key material is valid. */
    for(unsigned int valid=0;valid<4;valid++) {
        reset(); keys.rx_valid=valid; keys.tx_index=0; pending=true;
        assert(!q1000k_mac_data_keys_install(&keys,&pending) && !pending);
        for(unsigned int bank=0;bank<2;bank++) for(unsigned int word=0;word<4;word++)
            assert(registers[(0x5210+16*bank)/4+word]==(valid&BIT(bank) ? ~0U : 0));
        assert(!(registers[0x5200/4]&BIT(31)));
    }
    reset(); keys.rx_valid=1; keys.tx_index=1;
    assert(!q1000k_mac_data_keys_install(&keys,&pending)); phase=3; complete_after=0;
    assert(q1000k_mac_data_keys_ready(pending)==-ETIMEDOUT && polls==3000 && delays==3000);
    assert(registers[0x5044/4]==(BIT(0)|BIT(12)));
    reset(); registers[0x5200/4]=~0U;
    assert(q1000k_mac_data_keys_install(&keys,&pending)==-EIO && !writes);
    reset(); phase=3; registers[0x5044/4]=~0U;
    assert(q1000k_mac_data_keys_ready(true)==-EIO && !writes);
    reset(); phase=3; fail_read=1;
    assert(q1000k_mac_data_keys_ready(true)==-EIO && !writes);
    return 0;
}
