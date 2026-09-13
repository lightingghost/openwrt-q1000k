#!/usr/bin/env python3
"""Run production GEM commands and binding transactions without device access."""
from pathlib import Path
import re
import unittest
from pon_test_utils import run_c
from test_pon_identity import BSP
from test_pon_lifecycle import function

REPO = Path(__file__).resolve().parents[2]
MAC = REPO / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g'


def header(name):
    return (MAC / 'inc/common' / name).read_text()


def binding_source():
    source = (BSP.parent / 'xpon-en757x/xpon_10g/src/pwan/gpon_wan.c').read_text()
    start = source.index('static atomic_t q1000k_tcont_config_busy')
    end = source.index('static int q1000k_gwan_create_tcont', start)
    source = header('q1000k_gem.h') + header('q1000k_gwan.h') + source[start:end]
    for name in ['gwan_create_new_gemport', 'gwan_config_gemport',
                 'gwan_config_gemport_encrypt', 'gwan_remove_gemport',
                 'gwan_remove_all_gemport', 'gwan_remove_all_gemport_for_disable']:
        source += function('pwan/gpon_wan.c', name)
    for name in ['xmcs_create_gem_port', 'xmcs_remove_gem_port', 'xmcs_set_gem_encrypt']:
        source += function('xmcs/xmcs_if.c', name)
    source += function('gpon/gpon_recovery.c', 'gpon_recover_create_gemport')
    return re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)


class PonGemTests(unittest.TestCase):
    def test_commands_readback_faults_and_concurrent_updates(self):
        source = header('q1000k_gem.h') + (MAC / 'src/q1000k_gem.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        for name in ['gponDevGetGemInfo', 'gponDevSetGemInfo',
                     'gponDevSetGemInfoNoCheck', 'gponDevResetGemInfo']:
            source += function('gpon/gpon_dev.c', name)
        for name in ['xgpon_register_test', 'test_gpon_mac_reg', 'gpon_dvt_sw_reset']:
            source += function('gpon/gpon_dvt.c', name)
        source += function('gpon/gpon_proc.c', 'gpon_debug_write_proc')
        fixture = Path(__file__).with_name('pon_gem_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', source), flags=['-pthread'])

    def test_bindings_publication_retirement_and_concurrent_snapshots(self):
        fixture = Path(__file__).with_name('pon_gem_binding_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', binding_source()), flags=['-pthread'])


    def test_packet_consumers_use_one_binding_across_hooks(self):
        fixture = Path(__file__).with_name('pon_gem_binding_fixture.c').read_text()
        fixture = fixture[:fixture.index('int main(void)')]
        packet = Path(__file__).with_name('pon_gem_packet_fixture.c').read_text()
        source = function('pwan/gpon_wan.c', 'gwan_prepare_tx_message') + \
            function('pwan/gpon_wan.c', 'gwan_process_rx_message')
        # Legacy locals are unused with the optional HWNAT/VLAN features off.
        run_c(fixture.replace('/* PRODUCTION */', binding_source()) +
              packet.replace('/* PACKET PRODUCTION */', source),
              flags=['-pthread', '-Wno-unused-but-set-variable'])


if __name__ == '__main__':
    unittest.main()
