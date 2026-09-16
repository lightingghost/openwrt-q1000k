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
        fixture.write(str(fixture.controller.relative_to(self.root))+'/receiver_status', '{"oem_post_saved":true}\n')
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
    if args==[str(ctl/'status')]:
        active=(ctl/'operation').read_text().strip()=='initialize'
        emit(dict(mode='xgspon' if active else 'off',tx_inhibited=not tx(),tx_disabled=not tx() or not active,
            last_error=0,md32_enabled=active,firmware_verified=active,calibration_supplied=active))
    elif args==[str(root/'proc/q1000k-pon-snapshot')]:
        counter=root/'validation-count'; n=int(counter.read_text())+1 if counter.exists() else 1; counter.write_text(str(n))
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
    if interface.endswith('wan6'):
        emit(dict(up=active,pending=False,proto='dhcpv6',l3_device='pon',**{'ipv6-address':[{'address':'2001:db8::2'}] if active else [],'ipv6-prefix':[]}))
    else: emit(dict(up=active,pending=False,proto='dhcp',l3_device='pon',**{'ipv4-address':[{'address':'192.0.2.2'}] if active else []}))
    sys.exit(0)
with (root/'validation-calls').open('a') as out: out.write(json.dumps([action]+args)+'\n')
if os.environ.get('VALIDATION_FAIL')==action+':'+(args[0] if args else ''): sys.exit(1)
if action in ('modprobe','insmod'):
    module=pathlib.Path(args[0]).stem.replace('-','_') if action=='insmod' else args[0]
    if module=='q1000k_pon_control': (root/'controller-params').write_text(json.dumps(dict(a.split('=',1) for a in args[1:])))
    (root/'sys/module'/module).mkdir(parents=True)
    if module=='xpon_10g':
        (root/'proc/xgpon').mkdir(parents=True,exist_ok=True)
        (root/'proc/xgpon/status').write_text('protocol_error=0\nsecurity_keys_valid=1\ndata_rx_key_valid=1\ndata_tx_key_index=1\ndata_key_pending=0\n')
elif action=='rmmod':
    if args[0]=='q1000k_pon_control': assert (ctl/'operation').read_text()=='off\n'
    (root/'sys/module'/args[0]).rmdir()
elif action=='ip':
    if args[:4]==['link','set','dev','ponraw']: pass
    elif '-j' in args: emit([])
    else: print('1: pon inet 192.0.2.2/24 scope global')
elif action=='omci':
    if args[-1]=='mib': emit([dict(class_id=268,entity_id=1,data_hex='0000')])
    else:
        active=tx(); state=5 if active else 1
        if os.environ.get('VALIDATION_BAD')=='o5': state=3
        emit(dict(state=state,authenticated=int(active and state==5),agent_operational=int(active),service_error=0,
            service_rules=int(active and os.environ.get('VALIDATION_BAD')!='provision'),mib_objects=3,
            schema_version=1,onu_id=1 if active else 65535,gem_port_id=1 if active else 65535))
elif action=='ifup': (root/'wan-active').touch()
elif action=='ifdown': (root/'wan-active').unlink(missing_ok=True)
elif action in ('ping','curl','iperf3'): pass
else: raise AssertionError((action,args))
'''
        for name in ('cat','sleep','uname','bench-status','uci','ubus','modprobe','insmod','rmmod','ip','omci','ifup','ifdown','ping','curl','iperf3'):
            fixture.write('v-'+name, '#!'+sys.executable+'\n'+mock.replace("name; args=", "name.removeprefix('v-'); args=")).chmod(0o755)
        source = VALIDATE.read_text()
        source = re.sub(r'(?<![A-Za-z0-9])/(sys|proc|tmp|var/run)/', lambda m: str(self.root)+'/'+m[1]+'/', source)
        source = source.replace('/lib/q1000k-xgspon/common.sh', str(self.root/'common.sh'))
        source = source.replace('/lib/modules/',str(self.root/'lib/modules')+'/')
        source = source.replace('f2ec3b0de9683d113755d5d4df4fcafe8a4b47a43153ad0de45cbe9cd34c6e1c',hashlib.sha256(fixture.calibration.read_bytes()).hexdigest())
        source = source.replace('q1000k-pon-bench status', str(self.root/'v-bench-status'))
        source = source.replace('q1000k-omci -i',str(self.root/'v-omci')+' -i')
        source = source.replace('/usr/bin/ping',str(self.root/'v-ping'))
        for name in ('cat','sleep','uname','uci','ubus','modprobe','insmod','rmmod','ip','ifup','ifdown','ping','curl','iperf3'):
            source=re.sub(r'(?<![A-Za-z0-9_/-])'+name+r'(?= )','"'+str(self.root/('v-'+name))+'"',source)
        self.script=fixture.write('validate',source)

    def run_case(self, mode='rx', success=True):
        p=subprocess.run(['busybox','ash',str(self.script),mode,str(self.fixture.calibration),str(self.identity),'15','none'],env=self.env,capture_output=True,text=True,timeout=90)
        self.assertEqual(p.returncode==0,success,p.stdout[-2500:]+p.stderr)
        return p

    def calls(self):
        p=self.root/'validation-calls'
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def test_rx_default_policy_automatic_init_and_cleanup(self):
        result=self.run_case()
        self.assertIn('rx_power_nw=19900',result.stdout)
        self.assertIn('validation_stage name=cleanup status=passed',result.stdout)
        self.assertIn('validation_tx=0',self.calls()[0])
        self.assertFalse((self.root/'sys/module/xpon_10g').exists())

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
