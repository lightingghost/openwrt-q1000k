#!/usr/bin/env python3
"""Generate a UML module exercising the production native packet adapter."""
from pathlib import Path
import re

repo = Path(__file__).resolve().parents[2]
root = repo / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
fixture = Path(__file__).with_name('pon_adapter_kernel_fixture.c').read_text()
source = (root / 'inc/common/q1000k_transport.h').read_text() + '\n' + \
    (root / 'src/q1000k_transport.c').read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
print(fixture.replace('/* PRODUCTION */', source))
