#!/usr/bin/env python3
"""Actual OMCI provider/session code with modeled core and physical boundaries."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_gem import MAC
ROOT=Path(__file__).resolve().parents[2]
class BackendTests(unittest.TestCase):
    def test_public_diagnostic_trace_byte_order_and_action_fields(self):
        header=(ROOT/'package/kernel/q1000k-omci/src/include/net/xpon/omci.h').read_text()
        fields=re.search(r'struct omci_diagnostic \{.*?\n\};',header,re.S).group(0)
        trace=(ROOT/'package/kernel/airoha-pon/src/bsp/include/q1000k_trace.h').read_text()
        events=re.search(r'enum q1000k_event \{.*?\n\};',trace,re.S).group(0)
        backend=(MAC/'src/q1000k_omci_backend.c').read_text()
        code=backend[backend.index('static void qomci_diagnostic('):backend.index('static const struct omci_device_ops qomci_ops')]
        run_c(r'''#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define BIT(x) (1U << (x))
struct omci_device;
static u32 get_unaligned_be32(const void *p) { const u8 *b=p; return (u32)b[0]<<24 | (u32)b[1]<<16 | (u32)b[2]<<8 | b[3]; }
static struct { unsigned event,id; int result; u32 a,b,c,d; } records[8];
static unsigned n;
void q1000k_trace(unsigned ev,unsigned id,int ret,u32 a,u32 b,u32 c,u32 d) {
    assert(n<8); records[n++]=(typeof(records[0])){ev,id,ret,a,b,c,d};
}
''' + fields + events + code + r'''
int main(void) {
    struct omci_diagnostic e={.class_id=2,.transaction_id=123,.opcode=14,
      .public_kind=2,.public_class=277,.public_entity=0x8003,.public_mask=0xfff0,
      .public_sequence=21,.public_len=26};
    for(unsigned i=0;i<26;i++) e.public_data[i]=i;
    qomci_diagnostic(0,&e);
    assert(n==5 && records[1].event==QT_OMCI_TOPOLOGY && records[1].id==2);
    assert(records[1].a==0x01158003 && records[1].b==0x0015fff0 && records[1].c==123 && records[1].d==26);
    assert(records[2].a==0x007b0115 && records[2].b==0x00010203 && records[2].d==0x08090a0b);
    assert(records[4].id==5 && records[4].b==0x18190000 && !records[4].c && !records[4].d);
    n=0; e=(struct omci_diagnostic){.class_id=290,.entity_id=257,.attribute_mask=0xc000,
      .opcode=8,.flags=BIT(5)|BIT(7)|BIT(8),.dot1x_enable=1,.dot1x_action=3};
    qomci_diagnostic(0,&e);
    assert(n==2 && records[1].event==QT_OMCI_OPERATION);
    assert(records[1].d==(0x101 | (3<<9) | BIT(17) | BIT(18)));
}
''')

    def test_session_order_rekey_reset_packet_ownership_and_faults(self):
        omci=(ROOT/'package/kernel/q1000k-omci/src/include/net/xpon/omci.h').read_text()
        uapi=(ROOT/'package/kernel/q1000k-omci/src/include/uapi/linux/omci.h').read_text()
        types=re.search(r'^#define OMCI_TELEMETRY_F_BOSA_RX_POWER.*$',uapi,re.M).group(0)+'\n'
        for name in ['omci_identity','omci_telemetry','omci_gem_qos','omci_gem_port_config','omci_diagnostic','omci_device_ops']:
            types+=re.search(r'struct '+name+r' \{.*?\n\};',omci,re.S).group(0)+'\n'
        code=(ROOT/'package/kernel/airoha-pon/src/bsp/include/q1000k_phy_api.h').read_text()+(MAC/'inc/common/q1000k_auth.h').read_text()+(MAC/'inc/common/q1000k_mac_keys.h').read_text()
        code+=(MAC/'inc/common/q1000k_key_exchange.h').read_text()
        code+=(MAC/'src/q1000k_key_exchange.c').read_text().split('static int qkey_fifo_status')[0]
        code+=(MAC/'inc/common/q1000k_mac_cold.h').read_text()+(MAC/'src/q1000k_omci_backend.c').read_text()
        code=(ROOT/'package/kernel/airoha-pon/src/bsp/include/q1000k_rx_diag.h').read_text()+code
        code=re.sub(r'^#include[^\n]*\n','',code,flags=re.M)
        fixture=Path(__file__).with_name('pon_omci_backend_fixture.c').read_text()
        run_c(fixture.replace('/* TYPES */',types).replace('/* PRODUCTION */',code))
