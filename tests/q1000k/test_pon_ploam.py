#!/usr/bin/env python3
"""Authenticate before dispatch; recovery cannot use unowned legacy TX writes."""
from pathlib import Path
import re
import unittest

from test_pon_lifecycle import MAC, function
from pon_test_utils import run_c


class PloamTests(unittest.TestCase):
    def test_dispatch_authentication_allowlist_and_recovery(self):
        constants = '\n'.join((MAC.parent / ('inc/gpon/' + name)).read_text(errors='replace')
                              for name in ('gpon_ploam_raw.h', 'gpon_ploam.h'))
        defines = '\n'.join(line for line in constants.splitlines()
                            if re.match(r'#define (?:PLOAM_DOWN_|PLOAM_DISABLE_|PLAOM_DISABLE_|PLOAM_.*BROADCAST_ADDR)', line))
        code = defines + '\n'
        for name in ('ploam_parser_down_message', 'ploam_recv_deactivate_onu',
                     'ploam_recv_disable_serial_number', 'ploam_recv_key_control'):
            code += function('gpon/gpon_ploam.c', name)
        code += function('gpon/gpon_dev.c', 'gponDevUnicastKeyExchange')
        fixture = Path(__file__).with_name('pon_ploam_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', code))


if __name__ == '__main__':
    unittest.main()
