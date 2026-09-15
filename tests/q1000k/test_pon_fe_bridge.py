#!/usr/bin/env python3
"""Exercise the imported callers' checked native port configuration bridge."""
import unittest
from pon_test_utils import run_c
from test_pon_lifecycle import function

class FeBridgeTests(unittest.TestCase):
    def test_global_weight_callers_preserve_errors_and_frame_limits(self):
        source = function('xmcs/xmcs_if.c', 'xmcs_set_qos_weight_config')
        source += function('xmcs/xmcs_if.c', 'xmcs_get_qos_weight_config')
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#define Q1000K_PON_IDENTITY
#define XMCS_IF_WEIGHT_TYPE_PACKET 0
#define XMCS_IF_WEIGHT_TYPE_BYTE 1
#define XMCS_IF_WEIGHT_SCALE_1B 0
#define XMCS_IF_WEIGHT_SCALE_16B 1
struct airoha_pon_port_config { unsigned short min_len,max_len; bool byte_mode,scale16; };
struct XMCS_QoSWeightConfig_S { int weightType,weightScale; };
static struct airoha_pon_port_config current;
static int get_error,set_error,gets,sets;
static int q1000k_transport_get_port_config(struct airoha_pon_port_config *out)
{
    gets++; if(get_error) return get_error; *out=current; return 0;
}
static int q1000k_transport_configure_port(const struct airoha_pon_port_config *old,
    const struct airoha_pon_port_config *config)
{
    sets++; assert(!memcmp(old,&current,sizeof(current)));
    if(set_error) return set_error;
    current=*config; return 0;
}
''' + source + r'''
int main(void)
{
    struct XMCS_QoSWeightConfig_S weight,out,sentinel={17,23};
    for(int type=0;type<2;type++) for(int scale=0;scale<2;scale++) {
        current=(struct airoha_pon_port_config){60,16128,type,scale};
        weight=(struct XMCS_QoSWeightConfig_S){!type,!scale};
        assert(!xmcs_set_qos_weight_config(&weight));
        assert(current.min_len==60 && current.max_len==16128 && current.byte_mode==!type && current.scale16==!scale);
        assert(!xmcs_get_qos_weight_config(&out));
        assert(out.weightType==!type && out.weightScale==!scale);
    }
    weight=(struct XMCS_QoSWeightConfig_S){1,1};
    for(int which=0;which<2;which++) {
        get_error=which?-ENODEV:0; set_error=which?0:-EBUSY;
        struct airoha_pon_port_config old=current;
        assert(xmcs_set_qos_weight_config(&weight)==(which?-ENODEV:-EBUSY));
        assert(!memcmp(&old,&current,sizeof(current)));
    }
    out=sentinel;
    assert(xmcs_get_qos_weight_config(&out)==-ENODEV);
    assert(!memcmp(&out,&sentinel,sizeof(out)));
    get_error=set_error=0; gets=sets=0;
    weight.weightType=2;
    assert(xmcs_set_qos_weight_config(&weight)==-EINVAL && !gets && !sets);
    weight.weightType=1; weight.weightScale=2;
    assert(xmcs_set_qos_weight_config(&weight)==-EINVAL && !gets && !sets);
    assert(xmcs_set_qos_weight_config(NULL)==-EINVAL);
    assert(xmcs_get_qos_weight_config(NULL)==-EINVAL);
    return 0;
}
''')

if __name__ == '__main__':
    unittest.main()
