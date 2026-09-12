#!/usr/bin/env python3
"""Run real IPv4/IPv6 bridge flowtable regressions in private network namespaces.

Requires root/CAP_NET_ADMIN and the patched kernel. It never changes existing
interfaces or firewall tables. Intended for the separate UML test kernel.
It tests software forwarding; AN7581 PPE hardware requires a physical board.
"""
import json
import os
from pathlib import Path
import subprocess
import time
import unittest

PREFIX = 'q1000k-l2-' + str(os.getpid())

def run(*args, input=None):
    try:
        return subprocess.check_output(args, input=input, text=True, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"{args}: {error.output}") from error

class BridgeDatapathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.a, cls.b, cls.r = [PREFIX + '-' + name for name in ('a','b','r')]
        cls.addClassCleanup(cls.cleanup)
        for ns in (cls.a, cls.b, cls.r):
            run('ip','netns','add',ns)
            run('ip','-n',ns,'link','set','lo','up')
        for client,port in ((cls.a,'lan1'),(cls.b,'lan2')):
            run('ip','-n',cls.r,'link','add',port,'type','veth','peer','name','eth0','netns',client)
            run('ip','-n',client,'link','set','eth0','up')
        run('ip','-n',cls.r,'link','add','br-lan','type','bridge','mcast_snooping','0')
        for port in ('lan1','lan2'):
            run('ip','-n',cls.r,'link','set',port,'master','br-lan')
            run('ip','-n',cls.r,'link','set',port,'up')
        run('ip','-n',cls.r,'link','set','br-lan','up')
        for ns,idx in ((cls.a,1),(cls.b,2)):
            run('ip','-n',ns,'addr','add',f'192.0.2.{idx}/24','dev','eth0')
            run('ip','-n',ns,'-6','addr','add',f'2001:db8::{idx}/64','dev','eth0','nodad')
        cls.rules('''table bridge test {
            flowtable ft { hook ingress priority 0; devices = { lan1, lan2 }; counter; }
            chain forward { type filter hook forward priority 10; policy accept;
                ct state established meta l4proto { tcp, udp } counter flow offload @ft
            }
        }''')

    @classmethod
    def cleanup(cls):
        for ns in (cls.a,cls.b,cls.r):
            subprocess.run(['ip','netns','delete',ns],check=False)

    @classmethod
    def rules(cls,text):
        return run('ip','netns','exec',cls.r,'nft','-f','-',input=text)

    def udp_exchange(self, ipv6=False, ttl=37, size=100, routed=False):
        # Reply traffic confirms conntrack, then enough packets exercise the
        # flowtable. Ancillary metadata checks hop limit and DSCP preservation.
        address='2001:db8::2' if ipv6 else '192.0.2.2'
        if routed: address='2001:db8:1::2' if ipv6 else '198.51.100.2'
        family='AF_INET6' if ipv6 else 'AF_INET'
        def slow_packets():
            table_family='inet' if routed else 'bridge'
            data=json.loads(run('ip','netns','exec',self.r,'nft','-j','list','chain',table_family,'test','forward'))
            return sum(expr['counter']['packets'] for item in data['nftables'] for expr in item.get('rule',{}).get('expr',[]) if 'counter' in expr)
        before=slow_packets()
        receiver=r'''
import socket, json, sys
s=socket.socket(socket.FAMILY,socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_RECVHOPLIMIT,1) if socket.FAMILY==socket.AF_INET6 else s.setsockopt(socket.IPPROTO_IP,getattr(socket,'IP_RECVTTL',12),1)
s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_RECVTCLASS,1) if socket.FAMILY==socket.AF_INET6 else s.setsockopt(socket.IPPROTO_IP,socket.IP_RECVTOS,1)
s.bind(('ADDRESS',5201));s.settimeout(10)
seen=[]
for i in range(80):
 data, ancillary, flags, peer=s.recvmsg(8192,256)
 meta={t:int.from_bytes(value,sys.byteorder) for level,t,value in ancillary}
 seen.append([len(data),meta])
 s.sendto(b'ack',peer)
print(json.dumps(seen))
'''.replace('FAMILY',family).replace('ADDRESS',address)
        sender=r'''
import socket,time
s=socket.socket(socket.FAMILY,socket.SOCK_DGRAM);s.settimeout(5)
for i in range(80):
 s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_UNICAST_HOPS,TTL_VALUE) if socket.FAMILY==socket.AF_INET6 else s.setsockopt(socket.IPPROTO_IP,socket.IP_TTL,TTL_VALUE)
 tos=40 if i<40 else 80
 s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_TCLASS,tos) if socket.FAMILY==socket.AF_INET6 else s.setsockopt(socket.IPPROTO_IP,socket.IP_TOS,tos)
 s.sendto(b'x'*SIZE,('ADDRESS',5201));assert s.recv(128)==b'ack'
 time.sleep(.005)
'''.replace('FAMILY',family).replace('ADDRESS',address).replace('TTL_VALUE',str(ttl)).replace('SIZE',str(size))
        proc=subprocess.Popen(['ip','netns','exec',self.b,'python3','-c',receiver],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            time.sleep(.2)
            run('ip','netns','exec',self.a,'python3','-c',sender)
            out,err=proc.communicate(timeout=15)
            self.assertEqual(proc.returncode,0,err)
            samples=json.loads(out)
            ttl_key=52 if ipv6 else 2  # IPV6_HOPLIMIT / IP_TTL
            tos_key=67 if ipv6 else 1  # IPV6_TCLASS / IP_TOS
            self.assertEqual(len(samples),80)
            for i,(length,meta) in enumerate(samples):
                self.assertEqual(length,size)
                self.assertEqual(meta[str(ttl_key)],ttl-1 if routed else ttl)
                if not routed: self.assertEqual(meta[str(tos_key)],40 if i<40 else 80)
        finally:
            if proc.poll() is None: proc.kill();proc.wait()
        self.last_slow_delta=slow_packets()-before
        return run('ip','netns','exec',self.r,'cat','/proc/net/nf_conntrack')

    def test_ipv4_preserves_ttl_dscp_and_offloads(self):
        self.assertIn('[OFFLOAD]',self.udp_exchange())
        self.assertLess(self.last_slow_delta,30)

    def test_ipv6_preserves_hop_limit_dscp_and_offloads(self):
        self.assertIn('[OFFLOAD]',self.udp_exchange(ipv6=True))
        self.assertLess(self.last_slow_delta,30)

    def test_ttl_one_is_forwarded_by_bridge(self):
        self.udp_exchange(ttl=1)
        self.udp_exchange(ipv6=True,ttl=1)

    def test_fragmented_ipv4_uses_slow_path_without_crashing(self):
        self.udp_exchange(size=3000)

    def test_tcp_offloads_in_both_directions(self):
        for v6 in (False,True):
            addr='2001:db8::2' if v6 else '192.0.2.2'
            server=subprocess.Popen(['ip','netns','exec',self.b,'iperf3','-s','-1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            try:
                time.sleep(.2)
                client=subprocess.Popen(['ip','netns','exec',self.a,'iperf3','-c',addr,'-t','3','-P','2'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                time.sleep(1)
                ct=run('ip','netns','exec',self.r,'cat','/proc/net/nf_conntrack')
                out,err=client.communicate(timeout=10)
                self.assertEqual(client.returncode,0,err+out)
                self.assertIn('[OFFLOAD]',ct)
                server.wait(timeout=5)
            finally:
                if server.poll() is None: server.kill();server.wait()

    def test_vlan_access_ports(self):
        run('ip','-n',self.r,'link','set','br-lan','type','bridge','vlan_filtering','1')
        for port in ('lan1','lan2'):
            run('ip','netns','exec',self.r,'bridge','vlan','del','dev',port,'vid','1')
            run('ip','netns','exec',self.r,'bridge','vlan','add','dev',port,'vid','20','pvid','untagged')
        run('ip','netns','exec',self.r,'bridge','vlan','add','dev','br-lan','vid','20','self')
        try:
            self.assertIn('[OFFLOAD]',self.udp_exchange())
            self.assertLess(self.last_slow_delta,30)
        finally:
            run('ip','-n',self.r,'link','set','br-lan','type','bridge','vlan_filtering','0')

    def test_z_routed_bridge_port_regression(self):
        self.rules('delete table bridge test')
        run('ip','-n',self.r,'link','set','lan2','nomaster')
        run('ip','-n',self.b,'addr','flush','dev','eth0')
        for ns,dev,v4,v6 in ((self.r,'br-lan','192.0.2.254/24','2001:db8::ff/64'),(self.r,'lan2','198.51.100.1/24','2001:db8:1::1/64'),(self.b,'eth0','198.51.100.2/24','2001:db8:1::2/64')):
            run('ip','-n',ns,'addr','add',v4,'dev',dev)
            run('ip','-n',ns,'-6','addr','add',v6,'dev',dev,'nodad')
        for ns,via4,via6 in ((self.a,'192.0.2.254','2001:db8::ff'),(self.b,'198.51.100.1','2001:db8:1::1')):
            run('ip','-n',ns,'route','add','default','via',via4)
            run('ip','-n',ns,'-6','route','add','default','via',via6)
        run('ip','netns','exec',self.r,'sysctl','-qw','net.ipv4.ip_forward=1','net.ipv6.conf.all.forwarding=1')
        self.rules('table inet test { flowtable ft { hook ingress priority 0; devices = { lan1, lan2 }; counter; }; chain forward { type filter hook forward priority 0; policy accept; ct state established meta l4proto { tcp, udp } counter flow offload @ft; }; }')
        self.assertIn('[OFFLOAD]',self.udp_exchange(routed=True))
        self.assertLess(self.last_slow_delta,30)
        self.assertIn('[OFFLOAD]',self.udp_exchange(ipv6=True,routed=True))
        self.assertLess(self.last_slow_delta,30)

if __name__=='__main__': unittest.main(verbosity=2)
