"""OMCI bench cases, retained diagnostic accounting and reached prerequisites."""
import argparse
import unittest
from test_pon_discovery import C


class OmciBenchTests(unittest.TestCase):
    def test_typed_provisioning_metadata_and_legacy_unknowns(self):
        records=[]
        for event,ident,result,a,b,c,d in [
                (22,4,0,268,64|16|(99<<16),1,0x222<<16),
                (29,2,-61,(268<<16)|0x222,99<<16,4,0),
                (28,1,-61,(0x222<<16)|600,(0x8000<<16)|(3<<8),0xffff,1),
                (28,2,-61,(0x222<<16)|600,0x80030000,0xffff0000,1),
                (22,9,0,131,16,5,0),
                (29,1,-95,(290<<16)|0x101,(100<<16)|0x8000,8,0x101),
                (22,4,0,268,64|16|1|(99<<16),1,0x222<<16)]:
            records.append(dict(critical_version=1,position=len(records)+1,seq=len(records)+1,
                ns=len(records)+1,generation=1,event=event,id=ident,result=result,a=a,b=b,c=c,d=d))
        p=C.omci_summary(records+records)['provisioning']
        self.assertEqual(len(p['response_errors']),3)
        self.assertEqual(p['response_results'][0]['count'],2)
        self.assertEqual(p['response_results'][0]['duplicates'],1)
        self.assertEqual(p['response_errors'][0]['attribute_mask'],0)
        self.assertIsNone(p['response_errors'][1]['entity_id'])
        self.assertIsNone(p['response_errors'][1]['attribute_mask'])
        self.assertEqual(p['operations'][0]['stage'],'hardware')
        self.assertEqual(p['operations'][0]['error'],-61)
        self.assertEqual(p['operations'][1]['dot1x_enable'],1)
        self.assertEqual(p['gem_configurations'][0]['direction'],3)
        self.assertEqual(p['gem_configurations'][0]['alloc_id'],0xffff)
        self.assertEqual(p['gem_qos'][0]['upstream_queue'],0x8003)
        self.assertEqual(p['gem_qos'][0]['downstream_queue'],0xffff)
        self.assertEqual(C.omci_summary([])['provisioning']['response_results'],[])

    def test_native_packet_outcomes_and_append_boundaries(self):
        records=[]
        for event,ident,result,a,b,c,d in [
                (30,7,-116,0x1234240a,0x010cfffe,0x00110034,7),
                (30,2,0,0x1234240a,0x010cfffe,0x00110034,8),
                (30,3,0,0x1234240a,0x010cfffe,0x00110034,8),
                (30,2,0,0x1235240a,0x010cfffe,0x00110034,8),
                (30,4,-5,0x1235240a,0x010cfffe,0x00110034,8),
                (31,1,0,2,2,0,3),(31,2,0,2,2,0,3)]:
            records.append(dict(critical_version=1,position=len(records)+1,seq=len(records)+1,
                ns=len(records)+1,generation=1,event=event,id=ident,result=result,a=a,b=b,c=c,d=d))
        o=C.omci_summary(records+records); n=o['native_tx']
        self.assertEqual(n['counts'],{'admission-retry':1,'submitted':2,'dma-complete':1,'hardware-drop':1})
        self.assertFalse(n['unbalanced']); self.assertFalse(n['optical_delivery_proven'])
        self.assertEqual(n['events'][1]['tci'],0x1234)
        self.assertEqual(n['events'][1]['me_class'],268)
        self.assertEqual(n['events'][1]['entity_id'],65534)
        self.assertEqual(n['events'][1]['length'],52)
        self.assertEqual(o['live_additions']['completed'],1)
        self.assertEqual(C.omci_summary(records[:2])['native_tx']['unbalanced'][0]['submitted'],1)

    def test_matrix_has_control_comparisons_and_no_physical_actions(self):
        args=argparse.Namespace(suite='omci', physical_only=False, skip_physical=False,
                               rx_only=False, identity='private.json', cases=None)
        cases=C.discovery_plan(args)
        self.assertEqual(cases[0]['name'], 'rx-startup')
        self.assertEqual([(c['ranging_mode'],c['omci_min_len'],c['alloc_revoke']) for c in cases[1:6]],
                         [(1,60,False),(3,60,False),(1,48,False),(1,60,True),(1,60,False)])
        self.assertTrue(all(c['initial_key_readback'] and c['key_inline'] for c in cases[1:]))
        self.assertFalse(any(c['name'] in C.PHYSICAL for c in cases))
        self.assertEqual([c['live_add'] for c in cases[6:]], [1,2,3,3,3])
        args.suite='continuity'
        self.assertEqual([c['name'] for c in C.discovery_plan(args)], ['rx-startup',
            'activation-omci-fixed','activation-omci-live-gem','activation-omci-live-tcont',
            'activation-omci-live-both','activation-omci-live-oem','activation-omci-live-repeat'])
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
