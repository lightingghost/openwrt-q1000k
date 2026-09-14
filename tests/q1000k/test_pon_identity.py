#!/usr/bin/env python3
"""Exercise production MAC identity and its legacy callers without a device."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / 'package/kernel/airoha-pon/src/xpon-en757x/xpon_10g/src/q1000k_identity.c'
BSP = Path(os.environ['Q1000K_PON_BSP']) if 'Q1000K_PON_BSP' in os.environ else next(
    REPO.glob('build_dir/target-*/linux-airoha_an7581/airoha-pon-v2/airoha-pon/bsp'))


def extract(source, name):
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    start = re.search(r'^int ' + name + r'\(', source, re.M).start()
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos] + '\n'


class PonIdentityTests(unittest.TestCase):
    def test_validation_cached_identity_and_callers(self):
        source = re.sub(r'^#include[^\n]*\n', '', SRC.read_text(), flags=re.M)
        header = (REPO / 'package/kernel/q1000k-omci/src/include/net/xpon/omci.h').read_text()
        source = re.search(r'struct omci_identity \{.*?\n\};', header, re.S).group(0) + '\n' + source
        mac = BSP.parent / 'xpon-en757x/xpon_10g/src'
        for filename, name in [('pwan/xpon_netif.c', 'get_interface_mac_addr'),
                               ('epon/epon_dev.c', 'get_onu_mac_address'),
                               ('xmcs/xmcs_if.c', 'getPonMacfromflash')]:
            source += extract((mac / filename).read_text(), name)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ETH_ALEN 6
#define ETH_ADDR_LEN 6
#define Q1000K_PON_IDENTITY
#define module_param(...)
#define MODULE_PARM_DESC(...)
#define XMCS_IF_WAN_DETECT_MODE_XGSPON 7
#define OMCI_OLT_VENDOR_ID_LEN 4
#define OMCI_OLT_VERSION_LEN 14
#define OMCI_OLT_EQUIPMENT_ID_LEN 20
#define OMCI_IDENTITY_F_VERSION 8
#define OMCI_IDENTITY_F_EQUIPMENT_ID 16
#define OMCI_CONFIG_SOURCE_DRIVER 1
typedef uint32_t u32;
typedef uint8_t u8;
typedef unsigned char unchar;
typedef unsigned char __u8;
static void memzero_explicit(void *p,size_t n) { memset(p,0,n); }
static bool board=true;
static bool of_machine_is_compatible(const char *name) {
    assert(!strcmp(name,"quantum,q1000k-ubi")); return board;
}
static int hexval(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static int hex2bin(unsigned char *out, const char *in, size_t n) {
    size_t i;
    for(i=0;i<n;i++) {
        int a=hexval(in[i*2]),b=hexval(in[i*2+1]);
        if(a<0 || b<0) return -EINVAL;
        out[i]=(a<<4)|b;
    }
    return 0;
}
static bool mac_pton(const char *s, unsigned char *mac) {
    unsigned int i;
    /* Standard kernel helpers are fixtures; production validation is below. */
    for(i=0;i<6;i++) {
        if(hex2bin(mac+i,s+3*i,1)) return false;
        if(i<5 && s[3*i+2]!=':') return false;
    }
    return true;
}
static bool is_valid_ether_addr(const unsigned char *mac) {
    static const unsigned char zero[6];
    return !(mac[0]&1) && memcmp(mac,zero,6);
}
''' + source + r'''
static void unavailable(void) {
    struct omci_identity id, saved;
    memset(&id,0x5a,sizeof(id)); saved=id;
    assert(q1000k_pon_get_omci_overrides(&id)==-ENODATA && !memcmp(&id,&saved,sizeof(id)));
    unsigned char out[10],before[10];
    memset(out,0xaa,sizeof(out)); memcpy(before,out,sizeof(out));
    assert(get_ethaddr(out+1,6)==-ENODATA);
    assert(q1000k_pon_get_serial(out+1,8)==-ENODATA);
    assert(get_interface_mac_addr(out+1)==-ENODATA);
    assert(get_onu_mac_address(out+1)==-ENODATA);
    assert(getPonMacfromflash(out+1)==-ENODATA);
    assert(!memcmp(out,before,sizeof(out)));
}
int main(void) {
    static char valid_mac[]="02:12:aB:34:cd:56",valid_sn[]="ABc11234ABcd";
    const unsigned char mac[]={2,0x12,0xab,0x34,0xcd,0x56};
    const unsigned char sn[]={'A','B','c','1',0x12,0x34,0xab,0xcd};
    char *bad_mac[]={"","02:12:ab:34:cd:5","02:12:ab:34:cd:56x",
       "02-12-ab-34-cd-56","02:12:ag:34:cd:56","01:12:ab:34:cd:56",
       "ff:ff:ff:ff:ff:ff","00:00:00:00:00:00"};
    char *bad_sn[]={"","ABC11234ABC","ABC11234ABCDx","ABC_1234ABCD",
       "ABC\x80" "1234ABCD","ABCD1234ABCG","ABCD 234ABCD"};
    unsigned char out[10];
    unsigned int i;
    unavailable();
    assert(q1000k_pon_identity_init()==-ENODATA);
    wan_mac=valid_mac;
    assert(q1000k_pon_identity_init()==-ENODATA);
    pon_serial=valid_sn;
    assert(q1000k_pon_identity_init()==-ENODATA);
    char reg_id[73]; memset(reg_id,'0',72); reg_id[72]=0; pon_reg_id=reg_id;
    board=false; assert(q1000k_pon_identity_init()==-ENODEV); unavailable(); board=true;
    for(i=0;i<sizeof(bad_mac)/sizeof(bad_mac[0]);i++) {
        wan_mac=bad_mac[i]; assert(q1000k_pon_identity_init()==-EINVAL); unavailable();
    }
    wan_mac=valid_mac;
    for(i=0;i<sizeof(bad_sn)/sizeof(bad_sn[0]);i++) {
        pon_serial=bad_sn[i]; assert(q1000k_pon_identity_init()==-EINVAL); unavailable();
    }
    pon_serial=valid_sn;
    assert(!q1000k_pon_identity_init());
    assert(get_onutype()==0x71);
    unsigned char registration[36],saved_reg[36];
    assert(!q1000k_pon_get_registration(registration,36));
    for(i=0;i<36;i++) assert(!registration[i]);
    assert(q1000k_pon_get_registration(NULL,36)==-EINVAL);
    assert(q1000k_pon_get_registration(registration,10)==-EINVAL);
    assert(q1000k_pon_get_registration(registration,35)==-EINVAL);
    assert(q1000k_pon_get_registration(registration,37)==-EINVAL);
    for(i=0;i<72;i++) {
        reg_id[i]='x'; assert(q1000k_pon_identity_init()==-EINVAL); reg_id[i]='0';
    }
    for(i=0;i<72;i++) {
        reg_id[i]=0; assert(q1000k_pon_identity_init()==-EINVAL); reg_id[i]='0';
    }
    reg_id[71]='f'; assert(!q1000k_pon_identity_init());
    assert(!q1000k_pon_get_registration(registration,36) && registration[35]==15);
    memcpy(saved_reg,registration,36); reg_id[71]='e';
    assert(!q1000k_pon_get_registration(registration,36) && !memcmp(saved_reg,registration,36));

    struct omci_identity id={.valid=3,.equipment_id="Q1000K",.version="OpenWrt"}, saved=id;
    assert(!q1000k_pon_get_omci_overrides(&id) && !memcmp(&id,&saved,sizeof(id)));
    assert(q1000k_pon_get_omci_overrides(NULL)==-EINVAL);
    char text[43];
    for(int version=0;version<2;version++) {
        unsigned int limit=version?28:40;
        for(unsigned int n=0;n<=limit+2;n++) {
            memset(text,'4',n); text[n]=0;
            pon_equipment_id_hex=version?NULL:text; pon_omci_version_hex=version?text:NULL;
            int expected=n>limit || (n&1) ? -EINVAL : 0;
            assert(q1000k_pon_identity_init()==expected);
            if(expected) { unavailable(); continue; }
            id=saved; assert(!q1000k_pon_get_omci_overrides(&id));
            if(n) {
                const unsigned char *value=version?id.version:id.equipment_id;
                for(unsigned int j=0;j<limit/2;j++) assert(value[j]==(j<n/2?'D':0));
                assert(id.valid==(3|(version?OMCI_IDENTITY_F_VERSION:OMCI_IDENTITY_F_EQUIPMENT_ID)));
                assert((version?id.version_source:id.equipment_source)==OMCI_CONFIG_SOURCE_DRIVER);
                text[0]='5';
                struct omci_identity cached=saved;
                assert(!q1000k_pon_get_omci_overrides(&cached) && !memcmp(&cached,&id,sizeof(id)));
            } else assert(!memcmp(&id,&saved,sizeof(id)));
        }
        for(unsigned int byte=0;byte<256;byte++) {
            snprintf(text,sizeof(text),"%02x",byte);
            int ret=q1000k_pon_identity_init();
            assert(ret==((byte>=0x20 && byte<=0x7e)?0:-EINVAL));
            if(ret) unavailable();
        }
        strcpy(text,"g0"); assert(q1000k_pon_identity_init()==-EINVAL); unavailable();
    }
    pon_equipment_id_hex=pon_omci_version_hex=NULL;
    assert(!q1000k_pon_identity_init());

    assert(get_ethaddr(NULL,6)==-EINVAL);
    assert(get_ethaddr(out,-1)==-EINVAL && q1000k_pon_get_serial(out,-1)==-EINVAL);
    assert(q1000k_pon_get_serial(NULL,8)==-EINVAL);
    for(i=0;i<10;i++) {
        if(i!=6) assert(get_ethaddr(out,i)==-EINVAL);
        if(i!=8) assert(q1000k_pon_get_serial(out,i)==-EINVAL);
    }
    memset(out,0xaa,sizeof(out));
    assert(!get_ethaddr(out+1,6) && !memcmp(out+1,mac,6));
    assert(out[0]==0xaa && out[7]==0xaa);
    assert(!q1000k_pon_get_serial(out+1,8) && !memcmp(out+1,sn,8));
    assert(out[0]==0xaa && out[9]==0xaa);
    assert(!get_interface_mac_addr(out) && !memcmp(out,mac,6));
    assert(!get_onu_mac_address(out) && !memcmp(out,mac,6));
    assert(!getPonMacfromflash(out) && !memcmp(out,mac,6));
    /* Cached bytes must not alias the module parameter buffers. */
    valid_mac[0]='4'; valid_sn[0]='Z';
    assert(!get_ethaddr(out,6) && !memcmp(out,mac,6));
    assert(!q1000k_pon_get_serial(out,8) && !memcmp(out,sn,8));
    pon_serial=NULL;
    assert(q1000k_pon_identity_init()==-ENODATA); unavailable();
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='q1000k-pon-identity-') as tmp:
            c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            c.write_text(code)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-sign-compare', '-fsanitize=undefined',
                            '-fno-sanitize-recover=all', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
