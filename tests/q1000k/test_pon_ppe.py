#!/usr/bin/env python3
"""Actual native PPE encoding, bounded leases and synchronous retirement."""
from pathlib import Path
import os
import re
import unittest
from pon_test_utils import run_c
ROOT = Path(__file__).resolve().parents[2]
ETH = Path(os.environ['Q1000K_PON_ETH']) if 'Q1000K_PON_ETH' in os.environ else next(
    ROOT.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/drivers/net/ethernet/airoha'))

def function(source, name):
    match = re.search(r'^(?:static )?(?:void|int)\s*\n?' + name + r'\(', source, re.M)
    if not match:
        raise AssertionError(name)
    pos = source.index('{', match.start())
    end, depth = pos + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'

class PonPpeTests(unittest.TestCase):
    def test_upper_stop_and_mac_change_retire_flows(self):
        path = Path(os.environ['Q1000K_NETIF_SOURCE']) if 'Q1000K_NETIF_SOURCE' in os.environ else next(
            ROOT.glob('build_dir/target-*/linux-airoha_an7581/airoha-pon-*/airoha-pon/xpon-en757x/xpon_10g/src/pwan/xpon_netif.c'))
        source = path.read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define Q1000K_PON_IDENTITY 1
#define PWAN_IF_DATA 2
#define PON_MSG(...) ((void)0)
#define printk(...) ((void)0)
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,18,44)
typedef struct { int netIdx; } PWAN_NetPriv_T;
struct net_device { PWAN_NetPriv_T priv; bool queue,carrier; unsigned char dev_addr[6]; };
struct sockaddr { unsigned short sa_family; unsigned char sa_data[14]; };
static unsigned int invalidations;
static struct net_device *changing;
static void *netdev_priv(struct net_device *dev) { return &dev->priv; }
static bool is_valid_ether_addr(const unsigned char *p) { return p[0]==2; }
static void eth_hw_addr_set(struct net_device *dev,const unsigned char *p) { memcpy(dev->dev_addr,p,6); }
static void netif_stop_queue(struct net_device *dev) { dev->queue=false; }
static void netif_carrier_off(struct net_device *dev) { dev->carrier=false; }
static void q1000k_transport_invalidate_flows(void) {
    invalidations++;
    if(changing) assert(changing->dev_addr[5]==42);
}
''' + function(source, 'pwan_net_stop') + function(source, 'pwan_net_set_macaddr') + r'''
int main(void) {
    struct net_device data={.priv={PWAN_IF_DATA},.queue=true,.carrier=true};
    struct net_device omci={.priv={1}};
    struct sockaddr addr={.sa_data={1,0,0,0,0,42}};
    assert(pwan_net_set_macaddr(&data,&addr)==-EIO && !invalidations);
    addr.sa_data[0]=2; changing=&data;
    assert(!pwan_net_set_macaddr(&data,&addr) && invalidations==1);
    changing=NULL;
    assert(!pwan_net_stop(&data) && invalidations==2 && !data.queue && !data.carrier);
    assert(!pwan_net_stop(&omci) && invalidations==2);
    assert(!pwan_net_set_macaddr(&omci,&addr) && invalidations==2);
    return 0;
}
''')

    def test_wire_layout_leases_and_failed_retirement(self):
        header = (ETH/'airoha_eth.h').read_text()
        api = (ETH.parents[3]/'include/linux/soc/airoha/airoha_pon.h').read_text()
        types = header[header.index('enum {\n\tAIROHA_FOE_STATE_INVALID'):header.index('struct airoha_flow_data {')]
        flow = re.search(r'struct airoha_pon_flow \{.*?\n\};',api,re.S).group()
        source = (ETH/'airoha_ppe.c').read_text()
        selected = '\n'.join(function(source,name) for name in (
            'airoha_ppe_foe_set_pon', 'airoha_ppe_pon_invalidate',
            'airoha_ppe_foe_remove_flow',
            'airoha_ppe_foe_flow_commit_entry'))
        fixture = Path(__file__).with_name('pon_ppe_fixture.c').read_text()
        run_c(fixture.replace('/* TYPES */',types+'\n'+flow).replace('/* PRODUCTION */', selected))

if __name__ == '__main__':
    unittest.main()
