#!/usr/bin/env python3
"""Generate a UML fixture using the production MAC process executor."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]/'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
source=(root/'inc/common/q1000k_protocol.h').read_text()+'\n'+(root/'src/q1000k_protocol.c').read_text()
source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
for name in ('request_irq','enable_irq','disable_irq_nosync','free_irq'):
    source=re.sub(r'\b'+name+r'\b','fixture_'+name,source)
print(Path(__file__).with_name('pon_protocol_kernel_fixture.c').read_text().replace('/* PRODUCTION */',source))
