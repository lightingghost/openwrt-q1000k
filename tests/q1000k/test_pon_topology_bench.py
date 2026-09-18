"""Wire topology evidence and the next connected-fiber comparison suite."""
import argparse
import unittest
from test_pon_discovery import C


def events(records):
    return [dict(critical_version=1,position=i+1,seq=i+1,ns=i+1,generation=1,
                 event=32,id=r[0],result=0,a=r[1],b=r[2],c=r[3],d=r[4])
            for i,r in enumerate(records)]


def payload(kind, entity, mask, raw, tid=10):
    raw=raw.ljust(36,b'\0')
    return [(kind,(277<<16)|entity,mask,tid,26)]+[
        (3+i,(tid<<16)|277,*[int.from_bytes(raw[i*12+j:i*12+j+4],'big') for j in (0,4,8)])
        for i in range(3)]


class TopologyBenchTests(unittest.TestCase):
    def test_wire_queue_decode_and_missing_word_evidence(self):
        raw=bytes.fromhex('01ffff00000000000080000007800001000000000000ffff0000')
        wire=events([(1,1,350,0,0)]+payload(2,0x8000,0xfff0,raw))
        t=C.omci_summary(wire+wire)['topology']
        self.assertEqual(t['upload_commands'][0]['commands'],350)
        self.assertEqual(t['fully_described_queues'],[0x8000])
        row=t['wire_records'][0]
        self.assertTrue(row['complete'])
        self.assertEqual(row['data_hex'],raw.hex())
        self.assertEqual(row['values']['related_port'],0x80000007)
        self.assertEqual(row['values']['scheduler_pointer'],0x8000)
        self.assertEqual(row['values']['weight'],1)
        # A missing record cannot become fabricated zeros or a usable mapping.
        t=C.omci_summary(wire[:-1])['topology']
        self.assertFalse(t['wire_records'][0]['complete'])
        self.assertEqual(t['fully_described_queues'],[])
        wire[1]['result']=-11
        t=C.omci_summary(wire)['topology']
        self.assertTrue(t['wire_records'][0]['complete'])
        self.assertEqual(t['fully_described_queues'],[])

    def test_partial_sets_and_dead_reference_are_not_aliased(self):
        wire=events(payload(6,0xdead,0x300,b'\x80\x00\x11')+
                    payload(6,0x8003,0x200,b'\x00\x00',tid=11))
        t=C.omci_summary(wire)['topology']
        self.assertEqual(t['wire_records'][0]['values'],dict(scheduler_pointer=0x8000,weight=17))
        self.assertEqual(t['wire_records'][1]['values'],dict(scheduler_pointer=0))
        self.assertEqual(len(t['unresolved_queue_requests']),2)
        self.assertEqual(t['wire_records'][0]['entity_id'],0xdead)
        # Arbitrary classes never become exported payloads.
        bad=events([(2,(256<<16),0xffff,10,26)])
        self.assertEqual(C.omci_summary(bad)['topology']['wire_records'],[])

    def test_dot1x_action_and_mode_are_independent_of_enable(self):
        e=dict(seq=1,event=29,id=0,result=0,a=(290<<16)|257,b=(99<<16)|0xc000,
               c=8,d=0x101|(3<<9)|(1<<17)|(1<<18))
        row=C.omci_provisioning_summary([e])['operations'][0]
        self.assertEqual((row['dot1x_enable'],row['dot1x_action'],row['dot1x_oem']),(1,3,True))
        e['d']=(2<<9)|(1<<17)
        row=C.omci_provisioning_summary([e])['operations'][0]
        self.assertIsNone(row['dot1x_enable']); self.assertEqual(row['dot1x_action'],2)
        self.assertFalse(row['dot1x_oem'])

    def test_suite_keeps_live_additions_and_needs_no_physical_cycle(self):
        args=argparse.Namespace(suite='topology',physical_only=False,skip_physical=False,
                               identity='private.json',rx_only=False,cases=None)
        plan=C.discovery_plan(args)
        self.assertEqual([c['name'] for c in plan],['rx-startup']+[
            'activation-omci-topology-'+n for n in ('strict','oem','eqd','repeat')])
        self.assertEqual([c['dot1x_oem'] for c in plan[1:]],[False,True,True,True])
        self.assertEqual([c['ranging_mode'] for c in plan[1:]],[1,1,3,1])
        self.assertTrue(all(c['live_add']==3 and c['initial_key_readback'] for c in plan[1:]))
        self.assertFalse(any(c['name'] in C.PHYSICAL for c in plan))
        self.assertEqual(plan[-1]['samples'],600)
        args.identity=None
        self.assertEqual(len(C.discovery_plan(args)),1)
        args.physical_only=True
        with self.assertRaises(ValueError): C.discovery_plan(args)


    def test_service_suite_and_post_install_continuation(self):
        args=argparse.Namespace(suite='service',physical_only=False,skip_physical=False,
                               identity='private.json',rx_only=False,cases=None)
        plan=C.discovery_plan(args)
        self.assertEqual([c['name'] for c in plan[1:]],[
            'activation-omci-service-'+n for n in ('control','initial','eqd','repeat')])
        self.assertEqual([c['live_add'] for c in plan[1:]],[3,7,7,7])
        self.assertTrue(all(c['dot1x_oem'] for c in plan[1:]))
        self.assertFalse(any(c['name'] in C.PHYSICAL for c in plan))
        def row(seq,ev,ident,a=0,b=0,c=0,d=0,error=0):
            return dict(seq=seq,ns=seq*1000000,event=ev,id=ident,a=a,b=b,c=c,d=d,result=error)
        rows=[row(1,33,2,1,0,3),row(2,33,6,1,0x10002,0x30004,0x50006),
              row(3,33,8,1,0x70008),row(4,33,4,0,24,1),row(5,27,32),
              row(6,11,5),row(7,27,32)]
        s=C.service_install_summary(rows)
        self.assertEqual(s['qos'][0]['weights'],list(range(1,9)))
        self.assertTrue(s['qos'][0]['valid'])
        self.assertEqual(s['initial_live_completed'],1)
        self.assertEqual(s['installs'][0]['requests_after_install'],1)
        self.assertEqual(s['installs'][0]['reset_or_deactivation_after_ms'],2)
        s=C.service_install_summary(rows[:2])
        self.assertIsNone(s['qos'][0]['weights']); self.assertFalse(s['qos'][0]['valid'])
        rows[3]['result']=-117
        self.assertEqual(C.service_install_summary(rows)['initial_live_completed'],0)
        args.cases='activation-omci-service-initial'
        self.assertEqual(len(C.discovery_plan(args)),1)


if __name__=='__main__': unittest.main()
