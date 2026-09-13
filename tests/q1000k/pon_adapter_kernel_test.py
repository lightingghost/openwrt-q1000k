#!/usr/bin/env python3
"""Generate a UML module exercising the production native packet adapter."""
from pathlib import Path
import re
import os

repo = Path(__file__).resolve().parents[2]
root = repo / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'
fixture = Path(__file__).with_name('pon_adapter_kernel_fixture.c').read_text()
source = (root / 'inc/common/q1000k_transport.h').read_text() + '\n' + \
    (root / 'src/q1000k_transport.c').read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
header = Path(os.environ['Q1000K_PON_HEADER']) if 'Q1000K_PON_HEADER' in os.environ else next(
    repo.glob('build_dir/target-*/linux-airoha_an7581/linux-6.18.*/include/linux/soc/airoha/airoha_pon.h'))
fixture = fixture.replace('#include <linux/soc/airoha/airoha_pon.h>', header.read_text())
print(fixture.replace('/* PRODUCTION */', source))
