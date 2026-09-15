// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32;
#include <an7581_xpon.h>
#include <an7581_xpon_map.h>
#define __iomem
#define EXPORT_SYMBOL(x)
#define DEFINE_RWLOCK(x) int x
#define pr_err_ratelimited(...) ((void)0)
static int held, ticks, reads, writes, ack_after, fifo_after, bad_after;
static bool ignore_write;
static u32 stop, fifo, last_write;
static unsigned char memory[0x1000];
#define read_lock_irqsave(l,f) do { (void)(l); assert(!held); held=1; (f)=0; } while (0)
#define read_unlock_irqrestore(l,f) do { (void)(l); (void)(f); assert(held); held=0; } while (0)
#define write_lock_irqsave read_lock_irqsave
#define write_unlock_irqrestore read_unlock_irqrestore
static void udelay(unsigned int n) { assert(held && n==1); ticks++; }
static u32 readl(void *p)
{
    assert(held); reads++;
    if(bad_after>=0 && ticks>=bad_after) return ~0U;
    if(p==memory+4) {
        if(ack_after>=0 && ticks>=ack_after) {
            if(stop & (1U<<0)) stop|=1U<<14;
            if(stop & (1U<<8)) stop|=1U<<15;
            if(stop & (1U<<16)) stop|=1U<<30;
            if(stop & (1U<<24)) stop|=1U<<31;
        }
        return stop;
    }
    assert(p==memory+0x814);
    if(fifo_after>=0 && ticks>=fifo_after) fifo=0xabcd0000;
    return fifo;
}
static void writel(u32 v,void *p)
{
    assert(held && p==memory+4);
    writes++; last_write=v;
    if(!ignore_write) stop=v;
}
/* PRODUCTION */
static struct an7581_xpon provider;
static void reset(void)
{
    memset(&provider,0,sizeof(provider)); provider.base[1]=memory; xpon=&provider;
    held=ticks=reads=writes=0; stop=0; fifo=1; ack_after=fifo_after=bad_after=-1;
    ignore_write=false;
}
int main(void)
{
    u32 controls[]={1U,1U<<8,1U<<16,1U<<24};
    unsigned int subset,b;
    reset(); xpon=NULL;
    assert(an7581_xpon_mac_request_rx_stop()==-ENODEV);
    assert(an7581_xpon_mac_stop(1,true)==-ENODEV);
    assert(an7581_xpon_mac_wait_tx_empty()==-ENODEV && !reads && !writes);
    reset();
    assert(an7581_xpon_mac_stop(0,true)==-EINVAL);
    for(b=0;b<32;b++) if(!((1U<<b)&0x01010101))
        assert(an7581_xpon_mac_stop(1U<<b,true)==-EINVAL);
    assert(!reads && !writes);
    /* Cold request succeeds without clocks/ack, but cannot hide a later
     * drain timeout or a dropped control write. No status bits are replayed.
     */
    reset(); stop=0xc000;
    assert(!an7581_xpon_mac_request_rx_stop() && !ticks && !provider.mac_fault);
    assert(last_write==(1U<<16) && !(stop&(1U<<30)));
    assert(an7581_xpon_mac_stop(1U<<16,true)==-ETIMEDOUT && provider.mac_fault);
    reset(); ignore_write=true;
    assert(an7581_xpon_mac_request_rx_stop()==-EIO && provider.mac_fault);
    reset(); provider.resetting=true;
    assert(an7581_xpon_mac_request_rx_stop()==-EBUSY && !writes);
    for(subset=1;subset<16;subset++) {
        u32 mask=0,done=0;
        reset();
        for(b=0;b<4;b++) if(subset&(1U<<b)) mask|=controls[b];
        stop=0x10|0x1000; ack_after=7;
        assert(!an7581_xpon_mac_stop(mask,true));
        assert(ticks==7 && !provider.mac_fault);
        assert(last_write==(mask|0x1010));
        if(mask&1) done|=1U<<14;
        if(mask&(1U<<8)) done|=1U<<15;
        if(mask&(1U<<16)) done|=1U<<30;
        if(mask&(1U<<24)) done|=1U<<31;
        assert((stop&done)==done);
        assert(!an7581_xpon_mac_stop(mask,false));
        assert(last_write==0x1010 && (stop&0x01011111)==0x1010);
    }
    reset(); ack_after=0; stop=0x01000000;
    assert(!an7581_xpon_mac_stop(1,true));
    assert((last_write&0x01000001)==0x01000001); /* Preserve another request. */
    reset(); ignore_write=true; ack_after=0;
    assert(an7581_xpon_mac_stop(1,true)==-EIO && provider.mac_fault);
    ignore_write=false; ack_after=0;
    assert(an7581_xpon_mac_stop(1,true)==-EIO); /* Containment hold is attempted. */
    b=writes;
    assert(an7581_xpon_mac_stop(1,false)==-EIO && writes==(int)b);
    reset(); bad_after=0;
    assert(an7581_xpon_mac_stop(1,true)==-EIO && !writes && provider.mac_fault);
    reset(); bad_after=17;
    assert(an7581_xpon_mac_stop(1,true)==-EIO && ticks==17 && provider.mac_fault);
    reset();
    assert(an7581_xpon_mac_stop(1,true)==-ETIMEDOUT && ticks==3000 && provider.mac_fault);
    /* Release verifies control readback but never waits for unspecified done deassertion. */
    reset(); stop=1; ignore_write=true;
    assert(an7581_xpon_mac_stop(1,false)==-EIO && provider.mac_fault);
    reset();
    assert(an7581_xpon_mac_wait_tx_empty()==-EBUSY && !provider.mac_fault);
    stop=(1U<<8); assert(an7581_xpon_mac_wait_tx_empty()==-EBUSY);
    stop|=(1U<<15)|(1U<<24); assert(an7581_xpon_mac_wait_tx_empty()==-EBUSY);
    stop&=~(1U<<24); fifo_after=9;
    assert(!an7581_xpon_mac_wait_tx_empty() && ticks==9 && !writes);
    reset(); stop=(1U<<8)|(1U<<15)|(1U<<24)|(1U<<31); fifo_after=0;
    assert(!an7581_xpon_mac_wait_tx_empty() && !ticks && !writes);
    reset(); stop=(1U<<8)|(1U<<15); bad_after=5;
    assert(an7581_xpon_mac_wait_tx_empty()==-EIO && ticks==5 && provider.mac_fault);
    reset(); stop=(1U<<8)|(1U<<15);
    assert(an7581_xpon_mac_wait_tx_empty()==-ETIMEDOUT && ticks==3000);
    fifo_after=0;
    assert(an7581_xpon_mac_wait_tx_empty()==-EIO);
    reset();
    set_xpon_data(0x5004,0); assert(!reads && !writes); /* No raw bypass. */
    return 0;
}
