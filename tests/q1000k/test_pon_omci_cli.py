#!/usr/bin/env python3
"""Userspace command parsing and malformed netlink replies; no device access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'package/network/utils/q1000k-omci-tools/src/omci.c'

class OmciCliTests(unittest.TestCase):
    def test_arguments_json_and_malformed_replies(self):
        headers = sorted(ROOT.glob('staging_dir/target-*/usr/include/libmnl/libmnl.h'))
        if not headers:
            self.skipTest('Build libmnl first to stage its public header')
        with tempfile.TemporaryDirectory(prefix='q1000k-omci-cli-test-') as tmp:
            work = Path(tmp)
            shutil.copytree(headers[0].parent, work / 'libmnl')
            source = '#define main qomci_cli_main\n#include "' + str(SOURCE) + '"\n#undef main\n'
            source += r'''
#include <assert.h>
static struct nlmsghdr *reply(struct client *c, unsigned char *buffer)
{
    memset(buffer,0,BUFFER_SIZE); memset(c->attrs,0,sizeof(c->attrs));
    struct nlmsghdr *nlh=mnl_nlmsg_put_header(buffer);
    nlh->nlmsg_type=c->family;
    struct genlmsghdr *g=mnl_nlmsg_put_extra_header(nlh,GENL_HDRLEN);
    g->cmd=OMCI_CMD_GET; g->version=OMCI_GENL_VERSION;
    return nlh;
}
int main(void)
{
    struct client c={.family=0x31};
    unsigned char data[BUFFER_SIZE], value[64];
    struct nlmsghdr *nlh;
    uint32_t n;
    size_t length;
    assert(!number("0x8000",65535,&n) && n==32768);
    assert(!number("0010",65535,&n) && n==10);
    const char *bad[]={"", "-1", "+1", " 1", "\t1", "\n1", "1\n", "0x", "65536", "9999999999999999999999999999999"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(number(bad[i],65535,&n)==-EINVAL);
    assert(config_value(config_find("serial"),"123",value,&length)==-EOPNOTSUPP);
    assert(config_value(config_find("enabled"),"2",value,&length)==-EINVAL);
    assert(config_value(config_find("uni-count"),"0",value,&length)==-EINVAL);
    assert(config_value(config_find("version"),"too-long-version",value,&length)==-EINVAL);
    assert(config_value(config_find("version"),"bad\nvalue",value,&length)==-EINVAL);
    assert(!config_value(config_find("version"),"Q\"1000K",value,&length) && length==7);
    assert(!config_value(config_find("olt-profile"),"nokia",value,&length) && value[0]==3);
    assert(!config_value(config_find("olt-profile-force"),"none",value,&length) && value[0]==0);
    assert(config_value(config_find("olt-profile"),"none",value,&length)==-EINVAL);
    assert(!config_value(config_find("onu-type"),"sfu",value,&length) && value[0]==1);
    assert(!config_value(config_find("hardware-version"),"BGW320-500_2.1",value,&length));
    assert(!config_value(config_find("active-bank"),"1",value,&length) && value[0]==1);
    assert(config_value(config_find("committed-bank"),"2",value,&length)==-EINVAL);
    assert(!config_value(config_find("logical-password"),"",value,&length) && length==0);
    assert(config_value(config_find("logical-onu-id"),"1234567890123456789012345",value,&length)==-EINVAL);
    request_start(&c,OMCI_CMD_GET,false);
    nlh=reply(&c,data);
    mnl_attr_put_u32(nlh,OMCI_ATTR_DEV_ID,0);
    mnl_attr_put_u32(nlh,OMCI_ATTR_IFINDEX,1);
    mnl_attr_put_u8(nlh,OMCI_ATTR_STATE,5);
    mnl_attr_put_u8(nlh,OMCI_ATTR_AUTHENTICATED,0);
    mnl_attr_put_u32(nlh,OMCI_ATTR_SERVICE_ERROR,(uint32_t)-EUCLEAN);
    mnl_attr_put_u64(nlh,OMCI_ATTR_RX_PACKETS,UINT64_MAX);
    assert(reply_cb(nlh,&c)==MNL_CB_STOP);
    char *json=NULL; size_t bytes=0;
    FILE *out=open_memstream(&json,&bytes); assert(out);
    assert(!print_status(out,&c)); assert(!fclose(out));
    assert(strstr(json,"\"state\":5"));
    assert(strstr(json,"\"authenticated\":0"));
    assert(strstr(json,"\"service_error\":-117"));
    assert(strstr(json,"\"rx_packets\":\"18446744073709551615\""));
    assert(strstr(json,"\"temperature_mc\":null"));
    assert(strstr(json,"\"rx_power_dbm\":null")); free(json);
    mnl_attr_put_u32(nlh,OMCI_ATTR_BOSA_RX_POWER_NW,19900);
    memset(c.attrs,0,sizeof(c.attrs)); assert(reply_cb(nlh,&c)==MNL_CB_STOP);
    json=NULL; bytes=0; out=open_memstream(&json,&bytes); assert(out);
    assert(!print_status(out,&c)); assert(!fclose(out));
    assert(strstr(json,"\"rx_power_nw\":19900"));
    assert(strstr(json,"\"rx_power_dbm\":-17.01")); free(json);
    /* Duplicate, truncated and mis-sized attributes cannot become null. */
    mnl_attr_put_u8(nlh,OMCI_ATTR_STATE,4);
    memset(c.attrs,0,sizeof(c.attrs)); assert(reply_cb(nlh,&c)==MNL_CB_ERROR && errno==EPROTO);
    nlh=reply(&c,data); nlh->nlmsg_len++;
    assert(reply_cb(nlh,&c)==MNL_CB_ERROR && errno==EPROTO);
    nlh=reply(&c,data); mnl_attr_put_u16(nlh,OMCI_ATTR_STATE,5);
    assert(reply_cb(nlh,&c)==MNL_CB_STOP);
    uint64_t scalar_value; assert(scalar(&c,OMCI_ATTR_STATE,1,&scalar_value)==-EPROTO);
    nlh=reply(&c,data); ((struct genlmsghdr *)mnl_nlmsg_get_payload(nlh))->cmd=OMCI_CMD_MIB_GET;
    assert(reply_cb(nlh,&c)==MNL_CB_ERROR && errno==EPROTO);
    nlh=reply(&c,data); nlh->nlmsg_type++;
    assert(reply_cb(nlh,&c)==MNL_CB_ERROR && errno==EPROTO);
    nlh=reply(&c,data); mnl_attr_put_u16(nlh,CTRL_ATTR_FAMILY_ID,0x31);
    mnl_attr_put_u32(nlh,CTRL_ATTR_VERSION,OMCI_GENL_VERSION-1);
    nlh->nlmsg_type=GENL_ID_CTRL; c.resolving=true;
    assert(reply_cb(nlh,&c)==MNL_CB_ERROR && errno==EPROTONOSUPPORT);
    json=NULL; bytes=0; out=open_memstream(&json,&bytes); assert(out);
    const unsigned char text[]={34,92,10,0,255};
    json_string(out,text,sizeof(text)); assert(!fclose(out));
    assert(!strcmp(json,"\"\\\"\\\\\\u000a\\u0000\\u00ff\"")); free(json);
    return 0;
}
'''
            (work / 'test.c').write_text(source)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I' + str(work), '-I' + str(ROOT / 'package/kernel/q1000k-omci/src/include/uapi'),
                            str(work / 'test.c'), '-l:libmnl.so.0', '-lm', '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True, timeout=20)
