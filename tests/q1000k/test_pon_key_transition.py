#!/usr/bin/env python3
"""Exercise the prepared legacy registration and security entry points."""
from pathlib import Path
import unittest
from pon_test_utils import run_c
from test_pon_lifecycle import function

class KeyTransitionTests(unittest.TestCase):
    def test_repeated_registration_and_unsupported_security_writes(self):
        source = function('gpon/gpon_ploam.c', 'ploam_recv_request_registration')
        for name in ['gpon_key_index_change_by_hw', 'gpon_ploamIk_index_change_by_OMCI_base_secure',
                     'gpon_omciIk_index_change_by_OMCI_base_secure']:
            source += function('gpon/gpon_security.c', name)
        for name in ['xmcs_set_msk', 'xmcs_set_broadcast_key', 'xmcs_set_omci_broadcast_key',
                     'xmcs_set_omci_mic_ctrl']:
            source += function('xmcs/xmcs_gpon.c', name)
        fixture = Path(__file__).with_name('pon_key_transition_fixture.c').read_text()
        run_c(fixture.replace('/* PRODUCTION */', source))
