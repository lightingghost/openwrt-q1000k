#!/usr/bin/env python3
"""Real dual-stack DHCP exchanges on a disposable veth, for the netns suite.

The parent namespace represents the ONU; a child namespace is the main router.
The upstream IPv6 /60 is a fixture address with finite lifetimes, not an ISP
lease. netifd and PON/PPE hardware are outside the scope of this test.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def run(*args, **kw):
    p = subprocess.run(list(map(str, args)), text=True, capture_output=True, **kw)
    if p.returncode: raise RuntimeError(f'{args}: {p.stdout}{p.stderr}')
    return p.stdout


def wait_for(check, label, timeout=15):
    end = time.monotonic()+timeout
    while time.monotonic() < end:
        value = check()
        if value: return value
        time.sleep(0.1)
    raise RuntimeError('Timed out: '+label)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--tools', type=Path, required=True)
    ap.add_argument('--fragment', type=Path, required=True)
    args = ap.parse_args()
    mapping = Path('/proc/self/uid_map').read_text().split()
    if len(mapping) != 3 or mapping[-1] != '1': raise RuntimeError('Requires an isolated user namespace')
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    processes = []
    logs = []
    def start(name, command):
        log = (root/(name+'.log')).open('w')
        logs.append(log)
        p = subprocess.Popen(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT)
        processes.append(p)
        return p
    def write(name, text, execute=False):
        p = root/name
        p.write_text(text)
        if execute: p.chmod(0o755)
        return p
    def read_events():
        p = root/'client6.events'
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []
    try:
        child = start('namespace', ['unshare','-n','sleep','120'])
        wait_for(lambda: os.stat(f'/proc/{child.pid}/ns/net').st_ino != os.stat('/proc/self/ns/net').st_ino,
                 'child namespace')
        ns = ['nsenter','-t',str(child.pid),'-n']
        run('ip','link','add','pt-wire','type','veth','peer','name','pt-client')
        run('ip','link','set','pt-client','netns',child.pid)
        run('ip','link','set','pt-wire','master','br-lan')
        run('ip','link','set','pt-wire','up')
        run(*ns,'ip','link','set','lo','up')
        run(*ns,'ip','link','set','pt-client','address','02:11:22:33:44:55','up')
        run('ip','-6','address','add','fe80::1/64','dev','br-lan','nodad')
        run(*ns,'ip','-6','address','add','fe80::2/64','dev','pt-client','nodad')
        # netifd uses a /60 address to describe the LAN pool, with a /64
        # connected route. This fixture supplies the same kernel input.
        run('ip','-6','address','add','2001:db8:1234:1200::1/60','dev','br-lan',
            'preferred_lft','300','valid_lft','600','noprefixroute')
        run('ip','-6','route','add','2001:db8:1234:1200::/64','dev','br-lan')
        run('ip','-6','route','add','default','dev','pon')
        config = root/'uci'
        config.mkdir()
        (config/'network').write_text("config interface 'lan'\n option device 'br-lan'\n")
        (config/'system').write_text('')
        (config/'dhcp').write_text(f"""config odhcpd 'odhcpd'
 option leasefile '{root}/leases6'
 option loglevel '7'
config dhcp 'lan'
 option interface 'lan'
 option ifname 'br-lan'
 option ra 'server'
 option dhcpv6 'server'
 option ndp 'disabled'
 option dhcpv6_pd '1'
 option dhcpv6_na '0'
 option dhcpv6_pd_min_len '61'
 option max_preferred_lifetime '30s'
 option max_valid_lifetime '60s'
 list dns '2001:db8::53'
""")
        server6 = start('odhcpd', [args.tools/'server/odhcpd','-c',config,'-f','-l','7'])
        config4 = write('dnsmasq.conf', f"""port=0
user=root
group=root
interface=br-lan
bind-interfaces
no-hosts
no-resolv
dhcp-authoritative
dhcp-leasefile={root}/leases4
pid-file={root}/dnsmasq.pid
dhcp-range=192.168.0.100,192.168.0.150,255.255.255.0,2m
conf-file={args.fragment}
log-dhcp
""")
        server4 = start('dnsmasq', [args.tools/'dnsmasq/src/dnsmasq','--no-daemon',
                                   '--conf-file='+str(config4),'--log-facility=-'])
        time.sleep(1.5)
        if server6.poll() is not None or server4.poll() is not None:
            raise RuntimeError('DHCP server exited; inspect retained logs')
        hook4 = write('hook4.py', f'''#!/usr/bin/python3
import os,json,sys
if sys.argv[1] in ('bound','renew'):
    with open({str(root/'client4.json')!r},'w') as f:
        json.dump({{key:os.getenv(key) for key in ('ip','subnet','router','dns','lease')}},f)
''', True)
        client4 = subprocess.run([*ns,'busybox','udhcpc','-f','-n','-q','-i','pt-client',
                                 '-s',str(hook4),'-t','4','-T','1'], capture_output=True, text=True, timeout=12)
        write('udhcpc.log', client4.stdout+client4.stderr)
        if client4.returncode: raise RuntimeError('DHCPv4 exchange failed')
        v4 = json.loads((root/'client4.json').read_text())
        assert v4['ip']=='198.51.100.10' and v4['subnet']=='255.255.255.0', v4
        assert v4['router']=='198.51.100.1' and v4['dns']=='203.0.113.53', v4
        hook6 = write('hook6.py', f'''#!/usr/bin/python3
import os,json,sys
with open({str(root/'client6.events')!r},'a') as f:
    f.write(json.dumps({{'event':sys.argv[2], 'prefixes':os.getenv('PREFIXES',''), 'dns':os.getenv('RDNSS','')}})+'\\n')
''', True)
        client6 = start('odhcp6c', [*ns,args.tools/'client/odhcp6c','-e','-l','7','-N','none',
                                   '-P','61','-F','-s',hook6,'-p',root/'client6.pid','pt-client'])
        bound = wait_for(lambda: next((e for e in read_events() if e['event']=='bound' and '/61,' in e['prefixes']), None),
                         'native /61 DHCPv6 binding')
        prefix = ipaddress.ip_network(bound['prefixes'].split(',')[0])
        assert prefix == ipaddress.ip_network('2001:db8:1234:1208::/61'), bound
        route = run('ip','-6','route','show',str(prefix))
        assert 'via fe80:' in route and 'dev br-lan' in route, route
        # Exercise both extreme /64s from the delegated /61 across that route.
        for address in ('2001:db8:1234:1208::2','2001:db8:1234:120f::2'):
            run(*ns,'ip','-6','address','add',address+'/128','dev','lo')
            run('ping','-6','-c','1','-W','2',address)
        before = len(read_events())
        client6.send_signal(signal.SIGUSR1)
        renewed = wait_for(lambda: next((e for e in read_events()[before:] if '/61,' in e['prefixes'] and
                                         e['event'] in ('updated','rebound','bound')), None), 'native renewal', 20)
        client6.terminate()
        client6.wait(timeout=10)
        wait_for(lambda: not run('ip','-6','route','show',str(prefix)).strip(), 'native route removal on release')
        result = {'passed':True,'ipv4':v4,'ipv6_bound':bound,'ipv6_renewed':renewed,
                  'delegation_route':route.strip(),'first_last_64_reachable':True,'release_route_removed':True,
                  'scope':'isolated real dnsmasq/odhcpd/odhcp6c; upstream PD and netifd kernel input are fixtures'}
        write('summary.json', json.dumps(result,indent=2)+'\n')
        print(json.dumps(result))
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
        for log in logs: log.close()
        subprocess.run(['ip','link','del','pt-wire'], capture_output=True)
        subprocess.run(['ip','-6','address','del','2001:db8:1234:1200::1/60','dev','br-lan'], capture_output=True)
        subprocess.run(['ip','-6','address','del','fe80::1/64','dev','br-lan'], capture_output=True)
        subprocess.run(['ip','-6','route','del','2001:db8:1234:1200::/64','dev','br-lan'], capture_output=True)
        subprocess.run(['ip','-6','route','del','default','dev','pon'], capture_output=True)


if __name__ == '__main__':
    main()
