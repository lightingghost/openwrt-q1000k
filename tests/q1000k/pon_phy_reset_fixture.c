// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32;
#define PHY_TX_DIS_ON_HW_ONLY 2
#define PHY_TX_DIS_RESTORE_BY_SW 3
#define PHY_PMA_RESET_FUNC 0
/* REGISTERS */
static int context, status, step, fail, reset_count, tx_disabled, drop;
static u32 mode=10, digital, analog=0x12345778, force=0x76553210, pcs;
static int next(void) { return ++step==fail ? -ETIMEDOUT : 0; }
static int q1000k_phy_callback_context(void) { return context; }
static int an7581_pon_phy_status(void) { return status; }
static int an7581_pon_wan_get(u32 *v) { int r=next(); if(!r) *v=mode; return r; }
static int an7581_pon_wan_set(u32 v) { int r=next(); if(!r) mode=v; return r; }
static u32 *address(u32 reg)
{
    if(reg==EN7581_XPON_PMA_PON_CK_SET) return &digital;
    if(reg==EN7581_XPON_ANA_RG_PXP_TXPLL_PHY_CK1_EN) return &analog;
    if(reg==EN7581_XPON_PMA_rg_force_da_pxp_txpll_ckout_en) return &force;
    assert(reg==EN7581_XGPON_PHY_XG_PHY_RST_N); return &pcs;
}
static int an7581_pon_phy_write(u32 reg,u32 v) { int r=next(); if(!r && step!=drop) *address(reg)=v; return r; }
static int an7581_pon_phy_read(u32 reg,u32 *v) { int r=next(); if(!r) *v=*address(reg); return r; }
static int an7581_pon_phy_update(u32 reg,u32 end,u32 start,u32 v)
{
    int r=next(); u32 *p=address(reg); assert(end==start && v<=1);
    if(!r && step!=drop) *p=(*p&~(1U<<end))|(v<<end);
    return r;
}
static int an7581_pon_phy_reset(void)
{
    assert(mode==17 && !digital && !(analog&(1<<8)));
    assert((force&0x1010000)==0x1000000); reset_count++; return next();
}
static void udelay(int n) { assert(n==1); }
static int phy_trans_power_switch(int value)
{
    int r=next(); assert(value==2 || value==3); if(!r) tx_disabled=value==2; return r;
}
static int pma_reset(char *arg) { assert(!arg && tx_disabled); return next(); }
static int (*ponPhyFunc[])(char *)={pma_reset};
/* PRODUCTION */
static void init(void)
{
    step=0; fail=0; drop=0; reset_count=0; mode=10; digital=1;
    analog=0x12345778; force=0x76553210; pcs=EN7581_XGPON_PHY_XG_PHY_RST_N_OFF;
}
int main(void)
{
    int n, total;
    init(); context=-EPERM;
    assert(q1000k_phy_top_reset()==-EPERM && !step);
    assert(q1000k_phy_pma_reset()==-EPERM && !step);
    context=0; mode=1;
    assert(q1000k_phy_top_reset()==-EINVAL && step==1);
    init(); assert(!q1000k_phy_top_reset()); total=step;
    assert(mode==10 && reset_count==1 && !digital);
    assert(analog==(0x12345778&~0x100U));
    assert(force==((0x76553210|0x1000000)&~0x10000U));
    for(n=1;n<=total;n++) {
        init(); fail=n; assert(q1000k_phy_top_reset()==-ETIMEDOUT && step==n);
        if(n>4 && n<=12) assert(mode==17);
    }
    for(n=5;n<=9;n+=2) {
        init(); drop=n; assert(q1000k_phy_top_reset()==-EIO && !reset_count && mode==17);
    }
    init(); tx_disabled=0; assert(!q1000k_phy_pma_reset() && step==3 && !tx_disabled);
    for(n=1;n<=3;n++) {
        init(); fail=n; tx_disabled=0;
        assert(q1000k_phy_pma_reset()==-ETIMEDOUT && step==n);
        if(n>1) assert(tx_disabled);
    }
    return 0;
}
