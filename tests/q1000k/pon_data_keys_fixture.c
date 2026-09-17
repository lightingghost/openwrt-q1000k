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
static bool switch_requested, owner=true;
static bool q1000k_protocol_owned(void) { return owner; }
static int q1000k_pipeline_table_context(int wanted) { return wanted==phase ? 0 : -EPERM; }
static int an7581_xpon_status(void) { return provider_error; }
static void udelay(unsigned int delay) { assert(delay==1); delays++; }
static void set_xpon_data(u32 reg,u32 value) {
    assert((phase==2 || phase==4 || (phase==3 && reg==0x5044)) && reg<0x6000 && !(reg&3));
    if(++writes==fail_write) { provider_error=-EIO; return; }
    if(reg>=0x5210 && reg<=0x522c) {
        unsigned int bank=(reg-0x5210)/16;
        assert(!(registers[0x5204/4]&BIT(bank)));
        assert(!(registers[0x5200/4]&BIT(31)) || (phase==4 && (registers[0x5200/4]&1)!=bank));
    }
    if(reg==0x5044) { assert(value==BIT(7)); registers[reg/4]&=~value; if(phase>=3) switch_requested=false; }
    else registers[reg/4]=value;
    if(reg==0x5200 && (value&BIT(31))) switch_requested=true;
}
static u32 get_xpon_data(u32 reg) {
    assert(reg<0x6000 && !(reg&3));
    if(++reads==fail_read) { provider_error=-EIO; return ~0U; }
    if(phase>=3 && reg==0x5044 && switch_requested && ++polls==complete_after) registers[reg/4]|=BIT(7);
    return registers[reg/4];
}
#define module_param(...) /* module parameter */
#define MODULE_PARM_DESC(...) /* description */
#define q1000k_activation_snapshot(...) ((void)0)
/* PRODUCTION */
static void reset(void) {
    memset(registers,0,sizeof(registers));
    registers[0x5800/4]=0x12345678;
    registers[0x5200/4]=0x87654321;
    registers[0x5204/4]=0xa5a5fff3;
    registers[0x5044/4]=BIT(7)|BIT(0)|BIT(12);
    phase=2; writes=reads=fail_write=fail_read=provider_error=polls=delays=0;
    switch_requested=false; complete_after=3; owner=true;
}
static void live_start(const struct q1000k_mac_data_keys *old) {
    reset(); phase=4;
    registers[0x5104/4]=5; registers[0x5004/4]=0;
    registers[0x5200/4]=0x1200|(old->tx_index ? BIT(31)|(old->tx_index-1) : 0);
    registers[0x5204/4]=0xa500|old->rx_valid;
    for(unsigned int b=0;b<2;b++) for(unsigned int w=0;w<4;w++)
        registers[(0x5210+16*b)/4+w]=get_unaligned_be32(old->key[b]+12-4*w);
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
    /* A live Generate may touch only an inactive bank. Confirm switches
     * atomically at the hardware completion, then retires the old RX key. */
    struct q1000k_mac_data_keys old={.rx_valid=1,.tx_index=1}, next;
    memset(old.key[0],0x12,16); next=old;
    next.rx_valid=3; memset(next.key[1],0x34,16);
    live_start(&old); owner=false;
    assert(q1000k_mac_data_keys_live(&old,&next)==-EPERM && !reads && !writes);
    live_start(&old); registers[0x5104/4]=4;
    assert(q1000k_mac_data_keys_live(&old,&next)==-EOPNOTSUPP && !writes);
    live_start(&old); registers[0x5204/4]^=1;
    assert(q1000k_mac_data_keys_live(&old,&next)==-EUCLEAN && !writes);
    live_start(&old); next.key[0][0]^=1;
    assert(q1000k_mac_data_keys_live(&old,&next)==-EOPNOTSUPP && !reads && !writes);
    next.key[0][0]^=1;
    live_start(&old); assert(!q1000k_mac_data_keys_live(&old,&next));
    operations=writes; read_operations=reads;
    assert(registers[0x5200/4]==(0x1200|BIT(31)) && registers[0x5204/4]==0xa503);
    assert(!polls && registers[0x5044/4]==(BIT(7)|BIT(0)|BIT(12)));
    for(unsigned int w=0;w<4;w++) {
        assert(registers[0x5210/4+w]==0x12121212);
        assert(registers[0x5220/4+w]==0x34343434);
    }
    for(int f=1;f<=operations;f++) {
        live_start(&old); fail_write=f;
        assert(q1000k_mac_data_keys_live(&old,&next)==-EIO && writes==f);
    }
    for(int f=1;f<=read_operations;f++) {
        live_start(&old); fail_read=f;
        assert(q1000k_mac_data_keys_live(&old,&next)==-EIO && reads==f);
    }
    old=next; next.tx_index=2; next.rx_valid=2; memset(next.key[0],0,16);
    live_start(&old); assert(!q1000k_mac_data_keys_live(&old,&next));
    operations=writes; read_operations=reads;
    assert(polls==3 && registers[0x5200/4]==(0x1200|BIT(31)|1));
    assert(registers[0x5204/4]==0xa502 && registers[0x5044/4]==(BIT(0)|BIT(12)));
    assert(registers[0x5210/4]==0x12121212); /* Disabled, not rewritten live. */
    for(int f=1;f<=operations;f++) {
        live_start(&old); fail_write=f;
        assert(q1000k_mac_data_keys_live(&old,&next)==-EIO && writes==f);
    }
    for(int f=1;f<=read_operations;f++) {
        live_start(&old); fail_read=f;
        assert(q1000k_mac_data_keys_live(&old,&next)==-EIO && reads==f);
    }
    live_start(&old); complete_after=0;
    assert(q1000k_mac_data_keys_live(&old,&next)==-ETIMEDOUT && polls==3000);
    assert((registers[0x5204/4]&3)==3); /* Do not retire old key without switch ACK. */
    /* Missing switch IRQ may be accepted only for the first enable, after
     * all control/material readbacks agree. Later switches still time out. */
    bench_initial_key_readback=true;
    live_start(&old); complete_after=0;
    assert(q1000k_mac_data_keys_live(&old,&next)==-ETIMEDOUT);
    memset(&old,0,sizeof(old)); old.rx_valid=1; memset(old.key[0],0x39,16);
    next=old; next.tx_index=1;
    live_start(&old); complete_after=0;
    assert(!q1000k_mac_data_keys_live(&old,&next) && polls==3000);
    live_start(&old); complete_after=0; bench_initial_key_readback=false;
    assert(q1000k_mac_data_keys_live(&old,&next)==-ETIMEDOUT);
    bench_initial_key_readback=true;
    for(int bankword=0;bankword<4;bankword++) {
        live_start(&old); complete_after=0;
        registers[0x5210/4+bankword]^=1;
        assert(q1000k_mac_data_keys_live(&old,&next)==-EKEYREJECTED);
    }
    live_start(&old); bench_key_wait_us=2999;
    assert(q1000k_mac_data_keys_live(&old,&next)==-EINVAL && !reads);
    bench_key_wait_us=30001;
    assert(q1000k_mac_data_keys_live(&old,&next)==-EINVAL && !reads);
    return 0;
}
