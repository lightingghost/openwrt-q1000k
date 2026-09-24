#!/usr/bin/env python3
"""Exercise the combined runner with inert device programs and real JSON guards."""
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import unittest
from pathlib import Path
import test_pon_bench
ROOT = test_pon_bench.ROOT

SPEC = importlib.util.spec_from_file_location('activation_collect', ROOT/'scripts/q1000k/activation-collect.py')
COLLECT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COLLECT)
VALIDATE = ROOT/'package/network/utils/q1000k-xgspon-validation/files/validate'
IDENTITY = dict(serial='TEST01234567', wan_mac='02:11:22:33:44:55', registration_id='0123456789abcdef')


class ActivationTests(unittest.TestCase):
    def setUp(self):
        fixture = test_pon_bench.BenchTests(); fixture.setUp(); self.addCleanup(fixture.doCleanups)
        self.fixture = fixture
        self.root, self.env = fixture.root, fixture.env
        self.env['ACTIVATION_ROOT'] = str(self.root)
        fixture.write(fixture.dt+'quantum,xgspon-activation-bench', '')
        fixture.write('proc/uptime','0.00 0.00\n')
        fixture.write('proc/q1000k-pon-mac','{"mac_version":1}\n')
        fixture.write(str(fixture.controller.relative_to(self.root))+'/transmitter_status', '{"transmitter_version":1,"fields":{}}\n')
        fixture.write(str(fixture.controller.relative_to(self.root))+'/receiver_status', '{"oem_post_saved":true}\n')
        fixture.write(str(fixture.controller.relative_to(self.root))+'/output_status', json.dumps(dict(
            passive_output_version=1, error=0, begin_ns=1, end_ns=2, valid=(1<<30)-1,
            board_disabled_before=1, board_disabled=1, v=[0]*30))+'\n')
        self.identity = self.root/'tmp/private'
        self.identity.mkdir(); (self.identity/'identity.json').write_text(json.dumps(IDENTITY))
        mock = r'''import os,sys,json,pathlib
root=pathlib.Path(os.environ['ACTIVATION_ROOT']); action=pathlib.Path(sys.argv[0]).name; args=sys.argv[1:]
ctl=root/'sys/bus/i2c/drivers/q1000k-pon-control/0-0051'
def tx():
    p=root/'controller-params'
    return p.exists() and json.loads(p.read_text()).get('validation_tx')=='1'
def emit(v): print(json.dumps(v))
if action=='cat':
    if args==[str(ctl/'output_status')] and os.environ.get('VALIDATION_BAD')=='output-error':
        emit(dict(passive_output_version=1,error=-121))
    elif args==[str(ctl/'status')]:
        active=(ctl/'operation').read_text().strip()=='initialize'
        emit(dict(mode='xgspon' if active else 'off',tx_inhibited=not tx(),tx_disabled=not tx() or not active,
            last_error=0,md32_enabled=active,firmware_verified=active,calibration_supplied=active))
    elif args==[str(root/'sys/module/phy_10g/parameters/isolated_tx_test')]:
        emit(dict(isolated_tx_version=1,id=int(pathlib.Path(args[0]).read_text()),error=0,
            restored=os.environ.get('VALIDATION_BAD')!='restore',tx_off=True))
    elif args==[str(root/'sys/module/phy_10g/parameters/isolated_mpd')]:
        if os.environ.get('VALIDATION_BAD')=='mpd-missing': sys.exit(1)
        emit(dict(mpd_version=1,id=21,probes=[]))
    elif len(args)==1 and pathlib.Path(args[0]).name in ('output_before','output_on','output_after'):
        if os.environ.get('VALIDATION_BAD')=='output-missing': sys.exit(1)
        emit(dict(output_version=1,id=35,phase=['output_before','output_on','output_after'].index(pathlib.Path(args[0]).name),samples=[]))
    elif args==[str(root/'proc/q1000k-pon-snapshot')]:
        counter=root/'validation-count'; n=int(counter.read_text())+1 if counter.exists() else 1; counter.write_text(str(n)); (root/'proc/uptime').write_text(str(n)+'.00 0.00\n')
        bad=os.environ.get('VALIDATION_BAD','')
        emit(dict(receiver_version=5,rx_bench=not tx(),tx_inhibited=not tx(),tx_enabled=tx() or bad=='tx',
            registration_enabled=tx(),sampled_ms=n*1000,frames=n,synced=True,controller_los=False,phy_los=False,
            rx_power_valid=True,rx_power_nw=19900))
        emit(dict(diagnostics_version=5,probe=0,attempts=0,writes=0,sampled_ms=n*1000+(2000 if bad=='age' else 0),
            checker_control=0,data_route_control=0,bist_lane_control=0))
    else:
        for name in args: sys.stdout.buffer.write(pathlib.Path(name).read_bytes())
    sys.exit(0)
if action=='sleep': sys.exit(0)
if action=='uname': print('q1000k-fixture'); sys.exit(0)
if action=='bench-status': sys.exit(0)
if action=='uci':
    key=args[-1]
    if key.endswith('.q1000k_profile'): print('1')
    elif key.endswith('.auto'): print('0')
    elif key.endswith('.device'): print('pon')
    sys.exit(0)
if action=='ubus':
    interface=args[1].split('.')[-1]; active=(root/'wan-active').exists()
    active=active or os.environ.get('VALIDATION_BAD')=='wan-owned'
    if args[-1]=='renew':
        with (root/'validation-calls').open('a') as out: out.write(json.dumps([action]+args)+'\n')
        sys.exit(1 if os.environ.get('VALIDATION_BAD')=='renew-error' else 0)
    if interface.endswith('wan6'):
        prefix=[dict(address='2001:db8:1234::',mask=60,preferred=3600,valid=3600)] if os.environ.get('VALIDATION_PD') else []
        emit(dict(up=active,pending=False,proto='dhcpv6',l3_device='pon',**{'ipv6-address':[{'address':'2001:db8::2'}] if active else [],'ipv6-prefix':prefix}))
    else: emit(dict(up=active,pending=False,proto='dhcp',l3_device='pon',**{'ipv4-address':[{'address':'192.0.2.2'}] if active else []}))
    sys.exit(0)
with (root/'validation-calls').open('a') as out: out.write(json.dumps([action]+args)+'\n')
if os.environ.get('VALIDATION_FAIL')==action+':'+(args[0] if args else ''): sys.exit(1)
if action in ('modprobe','insmod'):
    module=pathlib.Path(args[0]).stem.replace('-','_') if action=='insmod' else args[0]
    if module=='q1000k_pon_control': (root/'controller-params').write_text(json.dumps(dict(a.split('=',1) for a in args[1:])))
    (root/'sys/module'/module).mkdir(parents=True)
    if module=='phy_10g' and 'isolated_tx_bench=1' in args:
        d=root/'sys/module/phy_10g/parameters'; d.mkdir(); (d/'isolated_tx_test').write_text('0')
    if module=='xpon_10g':
        (root/'proc/xgpon').mkdir(parents=True,exist_ok=True)
        (root/'proc/xgpon/status').write_text('protocol_error=0\nsecurity_keys_valid=1\ndata_rx_key_valid=1\ndata_tx_key_index=1\ndata_key_pending=0\n')
elif action=='rmmod':
    if args[0]=='q1000k_pon_control': assert (ctl/'operation').read_text()=='off\n'
    d=root/'sys/module'/args[0]
    if (d/'parameters').exists():
        (d/'parameters/isolated_tx_test').unlink(); (d/'parameters').rmdir()
    d.rmdir()
elif action=='ip':
    if args[:4]==['link','set','dev','ponraw']: pass
    elif args[:3]==['-6','address','add']:
        (root/'pd-active').touch()
    elif args[:3]==['-6','address','del']:
        if os.environ.get('VALIDATION_BAD')=='pd-cleanup': sys.exit(1)
        (root/'pd-active').unlink()
    elif args[:3]==['-6','-o','address']:
        print('1: pon inet6 fe80::1/64 scope link')
        if (root/'pd-active').exists():
            suffix=' dadfailed' if os.environ.get('VALIDATION_BAD')=='pd-dad' else ''
            print('1: pon inet6 2001:db8:1234::1/128 scope global'+suffix)
    elif '-j' in args: emit([])
    else: print('1: pon inet 192.0.2.2/24 scope global')
elif action=='pd-source':
    if os.environ.get('VALIDATION_BAD')=='pd-used': sys.exit(3)
    print('2001:db8:1234::1')
elif action=='omci':
    if args[-1]=='mib': emit([dict(class_id=268,entity_id=1,data_hex='0000')])
    else:
        active=tx(); state=5 if active else 1
        if os.environ.get('VALIDATION_BAD')=='o5': state=3
        service_error={'service-einval':-22,'service-fault':-117}.get(os.environ.get('VALIDATION_BAD'),0)
        emit(dict(state=state,authenticated=int(active and state==5),agent_operational=int(active),service_error=service_error,
            service_rules=int(active and os.environ.get('VALIDATION_BAD')!='provision'),mib_objects=3,
            schema_version=1,onu_id=1 if active else 65535,gem_port_id=1 if active else 65535))
elif action=='ifup': (root/'wan-active').touch()
elif action=='ifdown': (root/'wan-active').unlink(missing_ok=True)
elif action in ('ping','curl','iperf3','ipv6-bench','tcpdump'): pass
else: raise AssertionError((action,args))
'''
        for name in ('cat','sleep','uname','bench-status','uci','ubus','modprobe','insmod','rmmod','ip','omci','ifup','ifdown','ping','curl','iperf3','pd-source','ipv6-bench','tcpdump'):
            fixture.write('v-'+name, '#!'+sys.executable+'\n'+mock.replace("name; args=", "name.removeprefix('v-'); args=")).chmod(0o755)
        source = VALIDATE.read_text()
        source = re.sub(r'(?<![A-Za-z0-9])/(sys|proc|tmp|var/run)/', lambda m: str(self.root)+'/'+m[1]+'/', source)
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(self.root/'common.sh'))
        source = source.replace('/lib/modules/',str(self.root/'lib/modules')+'/')
        source = source.replace('f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c',hashlib.sha256(fixture.calibration.read_bytes()).hexdigest())
        source = source.replace('q1000k-pon-bench status', str(self.root/'v-bench-status'))
        source = source.replace('q1000k-omci -i',str(self.root/'v-omci')+' -i')
        source = source.replace('/usr/bin/ping',str(self.root/'v-ping'))
        source = source.replace('/usr/libexec/q1000k-pd-source',str(self.root/'v-pd-source'))
        source = source.replace('/usr/libexec/q1000k-ipv6-bench',str(self.root/'v-ipv6-bench'))
        for name in ('cat','sleep','uname','uci','ubus','modprobe','insmod','rmmod','ip','ifup','ifdown','ping','curl','iperf3','tcpdump'):
            source=re.sub(r'(?<![A-Za-z0-9_/=-])'+name+r'(?= )','"'+str(self.root/('v-'+name))+'"',source)
        self.script=fixture.write('validate',source)

    def run_case(self, mode='rx', success=True):
        p=subprocess.run(['busybox','ash',str(self.script),mode,str(self.fixture.calibration),str(self.identity),'15','none'],env=self.env,capture_output=True,text=True,timeout=90)
        self.assertEqual(p.returncode==0,success,p.stdout[-2500:]+p.stderr)
        return p

    def calls(self):
        p=self.root/'validation-calls'
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def test_wan_source_binding_renewal_and_owned_address_cleanup(self):
        self.env['VALIDATION_PD']='1'
        result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
            str(self.identity),'15','none','activation-omci-wan-renew'],env=self.env,
            capture_output=True,text=True,timeout=90)
        self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
        summary=COLLECT.ipv6_source_summary(result.stdout)
        self.assertTrue(summary['delegated_https'])
        self.assertEqual(set(summary['cycles']),{'initial','after-renew'})
        self.assertEqual(summary['cleanup_count'],2)
        self.assertEqual(len(summary['renewal_requests']),2)
        self.assertEqual(summary['cycles']['after-renew']['ipv4_wan'],dict(ping=0,mtu1500=0,https=0))
        self.assertFalse((self.root/'pd-active').exists())
        calls=self.calls()
        probes=[c for c in calls if c[0]=='curl' and any('ifhost!' in v for v in c)]
        self.assertEqual(len(probes),4)
        self.assertTrue(all(c[c.index('--interface')+1].startswith('ifhost!pon!') for c in probes))
        pings=[c for c in calls if c[0]=='ping' and c.count('-I')==2]
        self.assertEqual(len(pings),8)
        self.assertTrue(all(c[c.index('-I')+1]=='pon' for c in pings))
        add=[i for i,c in enumerate(calls) if c[:4]==['ip','-6','address','add']]
        delete=[i for i,c in enumerate(calls) if c[:4]==['ip','-6','address','del']]
        down=next(i for i,c in enumerate(calls) if c[0]=='ifdown')
        self.assertTrue(add[0]<delete[0]<add[1]<delete[1]<down)
        modules={Path(c[1]).stem:c for c in calls if c[0] in ('modprobe','insmod')}
        self.assertIn('bench_live_add=31',modules['xpon_10g'])
        self.assertIn('bench_vlan_untagged=1',modules['xpon_10g'])

    def test_pd_unavailable_dad_failure_and_delete_failure_remain_distinct(self):
        self.env['VALIDATION_PD']='1'
        for bad,success in [('pd-used',True),('pd-dad',True),('pd-cleanup',False)]:
            self.env['VALIDATION_BAD']=bad
            result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                str(self.identity),'15','none','activation-omci-wan-source'],env=self.env,
                capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode==0,success,result.stdout[-2500:]+result.stderr)
            self.assertFalse(COLLECT.ipv6_source_summary(result.stdout)['delegated_https'])
            self.assertIn('validation_stage name=cleanup status='+('passed' if success else 'failed'),result.stdout)
            self.assertFalse((self.root/'sys/module/xpon_10g').exists())
            if success: self.assertFalse((self.root/'pd-active').exists())

    def test_disconnected_phy_only_and_restore_failure(self):
        for bad in ('', 'restore'):
            self.env['VALIDATION_BAD']=bad
            result=subprocess.run(['busybox','ash',str(self.script),'isolated',str(self.fixture.calibration),
                str(self.identity),'30','none','isolated-3'],env=self.env,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode==0,not bad,result.stdout[-2500:]+result.stderr)
            self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
            self.assertFalse((self.root/'sys/module/phy_10g').exists())
        self.assertFalse(any('xpon_10g' in str(c) or 'omci' in str(c) for c in self.calls()))
        self.assertTrue(any('isolated_tx_bench=1' in c for c in self.calls()))
        self.assertFalse(any(c[0]=='ip' for c in self.calls()))

    def test_monitor_capture_and_missing_evidence(self):
        for bad in ('', 'mpd-missing'):
            self.env['VALIDATION_BAD']=bad
            result=subprocess.run(['busybox','ash',str(self.script),'isolated',str(self.fixture.calibration),
                str(self.identity),'30','none','isolated-21'],env=self.env,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode==0,not bad,result.stdout[-2500:]+result.stderr)
            if not bad: self.assertIn('"mpd_version": 1',result.stdout)
            self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
            self.assertFalse((self.root/'sys/module/phy_10g').exists())

    def test_rx_default_policy_automatic_init_and_cleanup(self):
        result=self.run_case()
        self.assertIn('rx_power_nw=19900',result.stdout)
        self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
        self.assertIn('validation_tx=0',self.calls()[0])
        self.assertFalse((self.root/'sys/module/xpon_10g').exists())

    def test_registration_variant_loads_exact_parameters_and_cleans_up(self):
        result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
            str(self.identity),'15','none','activation-reg-oem-direct'],env=self.env,
            capture_output=True,text=True,timeout=90)
        self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
        calls=[c for c in self.calls() if c[0] in ('modprobe','insmod') and Path(c[1]).stem=='xpon_10g']
        self.assertEqual(len(calls),1)
        for setting in ('bench_ranging_mode=3','bench_key_inline=1','bench_activation_diag=1',
                        'bench_profile_live=1','bench_control_coalesce=1','bench_sn_limit=40'):
            self.assertIn(setting,calls[0])
        self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
        self.assertFalse((self.root/'sys/module/xpon_10g').exists())

    def test_omci_variant_loads_readback_minimum_and_session_policy(self):
        result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
            str(self.identity),'15','none','activation-omci-min48'],env=self.env,
            capture_output=True,text=True,timeout=90)
        self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
        calls=[c for c in self.calls() if c[0] in ('modprobe','insmod') and Path(c[1]).stem=='xpon_10g']
        self.assertEqual(len(calls),1)
        for setting in ('bench_ranging_mode=1','bench_key_inline=1','bench_activation_diag=1',
                        'bench_initial_key_readback=1','bench_omci_min_len=48','bench_alloc_revoke=0'):
            self.assertIn(setting,calls[0])
        self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
        self.assertFalse((self.root/'sys/module/xpon_10g').exists())

    def test_topology_variants_keep_live_both_and_select_exact_core_parameters(self):
        for case,factory,ranging in [('strict',0,1),('oem',1,1),('eqd',1,3),('repeat',1,1)]:
            with self.subTest(case=case):
                result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                    str(self.identity),'15','none','activation-omci-topology-'+case],env=self.env,
                    capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
                modules={Path(c[1]).stem:c for c in self.calls() if c[0] in ('modprobe','insmod')}
                self.assertIn(f'bench_dot1x_oem={factory}',modules['omci'])
                for setting in ('bench_live_add=3',f'bench_ranging_mode={ranging}','bench_initial_key_readback=1',
                                'bench_key_inline=1','bench_alloc_revoke=0'):
                    self.assertIn(setting,modules['xpon_10g'])
                self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
                self.assertFalse((self.root/'sys/module/omci').exists())

    def test_service_variants_select_transport_control_and_exact_parameters(self):
        for case,live,ranging in [('control',3,1),('initial',7,1),('eqd',7,3),('repeat',7,1)]:
            with self.subTest(case=case):
                result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                    str(self.identity),'15','none','activation-omci-service-'+case],env=self.env,
                    capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
                modules={Path(c[1]).stem:c for c in self.calls() if c[0] in ('modprobe','insmod')}
                self.assertIn('bench_dot1x_oem=1',modules['omci'])
                for setting in (f'bench_live_add={live}',f'bench_ranging_mode={ranging}','bench_initial_key_readback=1',
                                'bench_key_inline=1','bench_alloc_revoke=0'):
                    self.assertIn(setting,modules['xpon_10g'])
                self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
                self.assertFalse((self.root/'sys/module/omci').exists())

    def test_classifier_variants_select_transport_control_and_exact_parameters(self):
        for case,live,ranging in [('control',7,1),('live',15,1),('eqd',15,3),('repeat',15,1)]:
            with self.subTest(case=case):
                result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                    str(self.identity),'15','none','activation-omci-filter-'+case],env=self.env,
                    capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
                modules={Path(c[1]).stem:c for c in self.calls() if c[0] in ('modprobe','insmod')}
                self.assertIn('bench_dot1x_oem=1',modules['omci'])
                for setting in (f'bench_live_add={live}',f'bench_ranging_mode={ranging}','bench_initial_key_readback=1',
                                'bench_key_inline=1','bench_alloc_revoke=0'):
                    self.assertIn(setting,modules['xpon_10g'])
                self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
                self.assertFalse((self.root/'sys/module/omci').exists())

    def test_vlan_variants_select_exact_policies_and_containment(self):
        for case,live,policy,ranging in [('control',15,0,1),('narrow',15,1,1),
                ('combined',31,1,1),('oem',31,2,1),('eqd',31,1,3),('repeat',31,1,1)]:
            with self.subTest(case=case):
                result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                    str(self.identity),'15','none','activation-omci-vlan-'+case],env=self.env,
                    capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
                modules={Path(c[1]).stem:c for c in self.calls() if c[0] in ('modprobe','insmod')}
                for setting in (f'bench_live_add={live}',f'bench_vlan_untagged={policy}',
                                f'bench_ranging_mode={ranging}','bench_initial_key_readback=1',
                                'bench_key_inline=1','bench_alloc_revoke=0','bench_omci_min_len=60'):
                    self.assertIn(setting,modules['xpon_10g'])
                self.assertIn('bench_dot1x_oem=1',modules['omci'])
                self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
        for case,bad,code in [('control','service-einval',2),('control','service-fault',1),
                              ('combined','service-einval',1)]:
            self.env['VALIDATION_BAD']=bad
            result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                str(self.identity),'15','none','activation-omci-vlan-'+case],env=self.env,
                capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,code,result.stdout[-2500:]+result.stderr)
            self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
            self.assertIn('status=functional-negative' if code==2 else 'status=containment',result.stdout)
            self.assertFalse((self.root/'sys/module/omci').exists())

    def test_output_capture_and_missing_evidence(self):
        for bad in ('', 'output-missing'):
            self.env['VALIDATION_BAD']=bad
            result=subprocess.run(['busybox','ash',str(self.script),'isolated',str(self.fixture.calibration),
                str(self.identity),'30','none','isolated-35'],env=self.env,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode==0,not bad,result.stdout[-2500:]+result.stderr)
            if not bad: self.assertEqual(result.stdout.count('"output_version": 1'),3)
            self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
            self.assertFalse((self.root/'sys/module/phy_10g').exists())

    def test_discovery_variants_and_passive_read_failure_cleanup(self):
        for case,full in [('probe',False),('full',True),('repeat',False)]:
            result=subprocess.run(['busybox','ash',str(self.script),'activate',str(self.fixture.calibration),
                str(self.identity),'15','none','activation-omci-discovery-'+case],env=self.env,
                capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stdout[-2500:]+result.stderr)
            modules={Path(c[1]).stem:c for c in self.calls() if c[0] in ('modprobe','insmod')}
            for setting in ('bench_live_add=31','bench_vlan_untagged=1','bench_ranging_mode=1',
                            'bench_initial_key_readback=1','bench_key_inline=1',
                            f'bench_profile_coalesce={int(not full)}',f'bench_profile_live={int(not full)}',
                            f'bench_control_coalesce={int(not full)}'):
                self.assertIn(setting,modules['xpon_10g'])
            self.assertIn('passive_output_observation phase=initialized-tx-off',result.stdout)
            self.assertIn('passive_output_observation phase=after-mac-stop-2s',result.stdout)
            self.assertFalse((self.root/'sys/module/phy_10g').exists())
        self.env['VALIDATION_BAD']='output-error'
        result=self.run_case(success=False)
        self.assertEqual(result.returncode,1)
        self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
        self.assertFalse((self.root/'sys/module/q1000k_pon_control').exists())

    def test_all_active_stages_and_interface_bound_probes(self):
        result=self.run_case('activate')
        report=COLLECT.summarize(result.stdout)
        self.assertEqual(report['status'],'observed')
        self.assertTrue(report['o5_authenticated'] and report['provisioned'] and report['dhcp_ipv4'] and report['dhcpv6_address'])
        self.assertTrue(report['downstream_key_installed'])
        self.assertFalse(report['encrypted_traffic_proven'])
        self.assertEqual(self.calls()[0][-1],'validation_tx=1')
        for call in self.calls():
            if call[0] in ('ping','curl'): self.assertIn('pon',call)
        self.assertFalse((self.root/'wan-active').exists())

    def test_bad_pair_fails_and_releases_every_module(self):
        self.env['VALIDATION_BAD']='age'
        p=self.run_case(success=False)
        self.assertIn('Diagnostic pair age exceeds limit',p.stderr)
        self.assertIn('validation_stage name=cleanup status=passed',p.stdout)
        self.assertFalse((self.root/'sys/module/xpon_10g').exists())

    def test_tx_observation_in_rx_mode_is_fatal(self):
        self.env['VALIDATION_BAD']='tx'
        self.assertIn('TX observed in RX-only',self.run_case(success=False).stderr)

    def test_no_factory_identity_fallback(self):
        (self.identity/'identity.json').write_text('{}')
        self.assertIn('Explicit subscriber serial',self.run_case('activate',False).stderr)
        self.assertEqual(self.calls(),[])

    def test_collector_zero_default_is_accepted_by_shipped_launcher(self):
        normalized = COLLECT.validate_identity(dict(IDENTITY, registration_id=''))
        (self.identity/'identity.json').write_text(json.dumps(normalized))
        self.run_case('activate')
        calls = [c for c in self.calls() if c[0] == 'insmod' and Path(c[1]).stem == 'xpon_10g']
        self.assertEqual(len(calls), 1)
        self.assertIn('pon_reg_id=' + '00' * 36, calls[0])

    def test_preexisting_wan_not_adopted(self):
        self.env['VALIDATION_BAD']='wan-owned'
        self.run_case('activate',False)
        self.assertEqual(self.calls(),[])

    def test_o5_timeout_blocks_wan(self):
        self.env['VALIDATION_BAD']='o5'
        p=self.run_case('activate',False)
        self.assertIn('name=activation status=timeout',p.stdout)
        self.assertNotIn('ifup',[c[0] for c in self.calls()])

    def test_provisioning_timeout_blocks_wan(self):
        self.env['VALIDATION_BAD']='provision'
        p=self.run_case('activate',False)
        self.assertIn('name=provisioning status=timeout',p.stdout)
        self.assertNotIn('ifup',[c[0] for c in self.calls()])

    def test_failed_unload_retains_dependencies(self):
        self.env['VALIDATION_FAIL']='rmmod:xpon_10g'
        p=self.run_case(success=False)
        self.assertIn('name=cleanup status=failed',p.stdout)
        self.assertTrue((self.root/'sys/module/q1000k_pon_control').exists())


class CollectorTests(unittest.TestCase):
    def test_extended_settings_match_native_validation(self):
        extra = dict(omcc_version='0xBF', pon_slot='3', olt_profile='nokia',
                     iphost_mac='02:11:22:33:44:55', iphost_hostname='H' * 25, iphost_domain='D' * 25)
        self.assertEqual(COLLECT.validate_identity(dict(IDENTITY, **extra)), dict(IDENTITY, **extra))
        for key, value in [('omcc_version','0xC0'), ('pon_slot','128'), ('pon_slot','255'),
                           ('pon_slot','-1'), ('iphost_mac','00:00:00:00:00:00'),
                           ('iphost_hostname','H'*26), ('olt_profile','18')]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                COLLECT.validate_identity(dict(IDENTITY, **{key:value}))

    def test_optional_registration_default_reaches_existing_launcher(self):
        for supplied in (dict(serial=IDENTITY['serial'], wan_mac=IDENTITY['wan_mac']),
                         dict(IDENTITY, registration_id='')):
            original = dict(supplied)
            normalized = COLLECT.validate_identity(supplied)
            self.assertEqual(normalized['registration_id'], '00' * 36)
            self.assertEqual(supplied, original)
        self.assertEqual(COLLECT.validate_identity(IDENTITY)['registration_id'], IDENTITY['registration_id'])
        for malformed in ('0', 'gg', '00' * 37):
            with self.assertRaises(ValueError):
                COLLECT.validate_identity(dict(IDENTITY, registration_id=malformed))

    def test_identity_validation_and_redaction(self):
        self.assertEqual(COLLECT.validate_identity(IDENTITY),IDENTITY)
        for key,value in [('serial',''),('wan_mac','01:11:22:33:44:55'),('registration_id','x'),('equipment_id','bad\n')]:
            with self.assertRaises(ValueError): COLLECT.validate_identity(dict(IDENTITY,**{key:value}))
        text=COLLECT.redactor(IDENTITY)('serial='+IDENTITY['serial']+' pon_reg_id='+IDENTITY['registration_id'])
        self.assertNotIn(IDENTITY['serial'],text); self.assertNotIn(IDENTITY['registration_id'],text)

    def test_no_false_success_for_empty_or_torn_capture(self):
        self.assertEqual(COLLECT.summarize('')['status'],'failed')
        data=json.dumps(dict(receiver_version=5,sampled_ms=1))+ '\n'+json.dumps(dict(diagnostics_version=5,sampled_ms=1502))+'\nvalidation_stage name=cleanup status=passed\n'
        self.assertEqual(COLLECT.summarize(data)['status'],'failed')
        self.assertFalse(COLLECT.summarize(data)['encrypted_traffic_proven'])

    def test_plan_includes_all_lifecycles_and_activation(self):
        self.assertEqual([x['name'] for x in COLLECT.plan()],['rx-startup','rx-repeat-1','rx-repeat-2','rx-soak','rx-reconnect','activation'])
        self.assertEqual(len(COLLECT.plan(True,True)),4)
        self.assertEqual(COLLECT.plan(physical_only=True), [dict(name='rx-reconnect', mode='rx', samples=300)])
        with self.assertRaises(ValueError): COLLECT.plan(skip_physical=True, physical_only=True)

    def test_management_lease_cannot_pass_wan(self):
        report=COLLECT.summarize(json.dumps(dict(l3_device='br-lan',up=True,proto='dhcp',**{'ipv4-address':['192.168.255.1']})))
        self.assertFalse(report['dhcp_ipv4'])

if __name__=='__main__': unittest.main()
