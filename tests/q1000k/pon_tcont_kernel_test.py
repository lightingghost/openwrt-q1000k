#!/usr/bin/env python3
"""Generate a UML-only module using the actual T-CONT command implementation."""
from pathlib import Path
import re

repo = Path(__file__).resolve().parents[2]
root = repo / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
source = (root / 'inc/common/q1000k_tcont.h').read_text() + '\n' + \
    (root / 'src/q1000k_tcont.c').read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
fixture = Path(__file__).with_name('pon_tcont_kernel_fixture.c').read_text()
print(fixture.replace('/* PRODUCTION */', source))
