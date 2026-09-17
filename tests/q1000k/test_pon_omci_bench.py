"""OMCI bench cases, retained diagnostic accounting and reached prerequisites."""
import argparse
import unittest
from test_pon_discovery import C


class OmciBenchTests(unittest.TestCase):
    def test_matrix_has_control_comparisons_and_no_physical_actions(self):
        args=argparse.Namespace(suite='omci', physical_only=False, skip_physical=False,
                               rx_only=False, identity='private.json', cases=None)
        cases=C.discovery_plan(args)
        self.assertEqual(cases[0]['name'], 'rx-startup')
        self.assertEqual([(c['ranging_mode'],c['omci_min_len'],c['alloc_revoke']) for c in cases[1:]],
                         [(1,60,False),(3,60,False),(1,48,False),(1,60,True),(1,60,False)])
        self.assertTrue(all(c['initial_key_readback'] and c['key_inline'] for c in cases[1:]))
        self.assertFalse(any(c['name'] in C.PHYSICAL for c in cases))
        args.cases='activation-omci-fixed'
        self.assertEqual(len(C.discovery_plan(args)),1)
        args.cases=None; args.identity=None
        self.assertEqual([c['name'] for c in C.discovery_plan(args)], ['rx-startup'])

    def test_duplicate_polling_and_incomplete_evidence_are_not_success(self):
        records=[]
        for ident,result,a,b,c,d in [(22,0,0,0,0,0), (30,0,0x40001100,48,0,0),
                (32,0,64,64,0,48), (37,-129,9,9,30,0), (40,0,9,3,64,0),
                (41,-108,9,64,1,0), (42,0,9,7,64,1), (45,0,9,3,0,0),
                (43,-116,9,9,64,1), (36,0,60,60,2000,2000)]:
            records.append(dict(critical_version=1,position=len(records)+1,seq=len(records)+1,
                                ns=len(records)+1,generation=1,event=27,id=ident,
                                result=result,a=a,b=b,c=c,d=d))
        o=C.omci_summary(records+records)
        self.assertEqual(o['ethernet_runt_omci_delivered'],1)
        self.assertEqual(o['authenticated_rx'],1)
        self.assertEqual(o['tx_auth_rejected'],1)
        self.assertEqual(o['deferred_native_consumed'],1)
        self.assertEqual(o['reply_expired'],1)
        self.assertEqual(o['minimums'],[60])
        self.assertTrue(o['critical_evidence_complete'])
        self.assertFalse(o['upstream_delivery_proven'])
        self.assertFalse(C.omci_summary(records[1:])['critical_evidence_complete'])
        case={'ids':['O1','O2','O3','O4','O5','O6']}
        outcomes=C.test_outcomes(case,dict(stages={},omci_experiment=o),'')
        self.assertEqual(outcomes['O3'],'allocation-session-preserved')
        self.assertEqual(outcomes['O6'],'service-not-verified')
        empty=C.test_outcomes(case,dict(stages={},omci_experiment=C.omci_summary([])),'')
        self.assertIn('not-observed',empty['O4'])
        self.assertIn('incomplete',empty['O4'])

if __name__=='__main__': unittest.main()
