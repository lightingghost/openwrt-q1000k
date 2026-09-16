#!/usr/bin/env python3
"""Build the production PHY lifecycle into a UML-only concurrency fixture."""
from pathlib import Path
import re
from test_pon_phy_lifecycle import BSP, production_source

source, regs = production_source()
source = '#define q1000k_trace(event,id,result,a,b,c,d) do { (void)(id); (void)(result); (void)(a); (void)(b); (void)(c); (void)(d); } while (0)\n#define q1000k_trace_generation(...) ((void)0)\n' + source
host = Path(__file__).with_name('pon_phy_lifecycle_fixture.c').read_text()
defs = host[host.index('#define TRUE'):host.index('/* REGISTERS */')]
defs = re.sub(r'^#define (?:IRQ_\w+|IRQF_\w+|GFP_KERNEL)[^\n]*\n', '', defs, flags=re.M)
types = host[host.index('struct phy_private {'):host.index('static int provider=')]
types = types.replace('event_poll_timer_value,event_handle_lock,pma_reset_lock;',
                      'event_poll_timer_value;\n    spinlock_t event_handle_lock,pma_reset_lock;')
fixture = Path(__file__).with_name('pon_phy_kernel_fixture.c').read_text()
header = (BSP / 'include/q1000k_phy_api.h').read_text()
header = header.replace('#include <q1000k_rx_diag.h>', (BSP / 'include/q1000k_rx_diag.h').read_text())
print(fixture.replace('/* API */',header).replace('/* TYPES */',defs+regs+types)
      .replace('/* PRODUCTION */',re.sub(r'^EXPORT_SYMBOL[^\n]*\n','',source,flags=re.M)))
