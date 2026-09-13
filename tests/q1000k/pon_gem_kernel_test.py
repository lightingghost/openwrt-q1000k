#!/usr/bin/env python3
"""Generate a UML-only module with production commands, registry and callers."""
from pathlib import Path
import re
from test_pon_gem import MAC, binding_source

fixture = Path(__file__).with_name('pon_gem_kernel_fixture.c').read_text()
# The host and kernel tests share only vendor structure/constant fixtures.
# Linux provides the real spinlocks, IRQ state, atomics, stats and kthreads.
model = Path(__file__).with_name('pon_gem_binding_fixture.c').read_text()
model = model[model.index('#define Q1000K_PON_IDENTITY'):model.index('typedef atomic_int')]
model = re.sub(r'^#define BIT[^\n]*\n', '', model, flags=re.M)
model = re.sub(r'^struct net_device_stats[^\n]*\n', '', model, flags=re.M)
production = (MAC / 'src/q1000k_gem.c').read_text()
production = re.sub(r'^#include[^\n]*\n', '', production, flags=re.M)
# Declare the command value type before its implementation; duplicate header
# content in binding_source is excluded by the production include guard.
header = (MAC / 'inc/common/q1000k_gem.h').read_text()
header = re.sub(r'^#include[^\n]*\n', '', header, flags=re.M)
print(fixture.replace('/* MODEL */', model).replace(
    '/* PRODUCTION */', header + production + binding_source()))
