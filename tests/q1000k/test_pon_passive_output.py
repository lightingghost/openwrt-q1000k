"""Connected-fiber passive readings and prerequisite-aware collection."""
from argparse import Namespace
from pathlib import Path
import copy
import json
import unittest
from pon_test_utils import run_c
from test_pon_discovery import C

ROOT = Path(__file__).resolve().parents[2]


class PassiveOutputTests(unittest.TestCase):
    def test_production_sysfs_lock_gpio_and_partial_error(self):
        source = (ROOT/'package/kernel/q1000k-pon-control/src/driver.c').read_text()
        body = source[source.index('static ssize_t output_status_show'):source.index('static DEVICE_ATTR_RO(output_status)')]
        run_c(r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef unsigned long long u64;
#include "en7573_output.h"
struct device { int unused; }; struct device_attribute { int unused; };
struct q1000k_pon { int lock, io, *board_tx_disable; };
static int gpio=0, calls, checks, fail_read, fail_gpio;
static struct q1000k_pon p = { .board_tx_disable=&gpio };
static void *dev_get_drvdata(struct device *d) { (void)d; return &p; }
static void mutex_lock(int *m) { assert(!*m); *m=1; }
static void mutex_unlock(int *m) { assert(*m); *m=0; }
static int pon_check_locked(struct q1000k_pon *p) { assert(p->lock); return checks; }
static u64 ktime_get_boottime_ns(void) { static u64 t=100; assert(p.lock); return ++t; }
static int gpiod_get_value_cansleep(int *g) { assert(p.lock && g==&gpio); return fail_gpio ? -EIO : *g; }
static int en7573_output_sample(int *io, struct en7573_output_sample *s) {
 assert(p.lock && io==&p.io); calls++;
 for(int i=0;i<EN7573_OUTPUT_FIELDS;i++) s->values[i]=~0U;
 s->valid=fail_read ? 3 : (1U<<30)-1; return fail_read ? -EREMOTEIO : 0;
}
static int sysfs_emit_at(char *b, int offset, const char *fmt, ...) {
 va_list a; va_start(a,fmt); int n=vsnprintf(b+offset,4096-offset,fmt,a); va_end(a); return n;
}
#define sysfs_emit(b,...) sysfs_emit_at(b,0,__VA_ARGS__)
/* PRODUCTION */
int main(void) {
 char out[4096]; int n=output_status_show(NULL,NULL,out);
 assert(n>0 && n<4096 && !p.lock && calls==1);
 assert(strstr(out,"\"error\":0") && strstr(out,"\"conversion_ready_verified\":false"));
 fail_read=1; n=output_status_show(NULL,NULL,out); assert(n>0 && !p.lock);
 assert(strstr(out,"\"valid\":3") && strstr(out,"\"error\":-121"));
 fail_read=0; fail_gpio=1; n=output_status_show(NULL,NULL,out); assert(n>0 && !p.lock);
 assert(strstr(out,"\"board_disabled\":-5") && strstr(out,"\"error\":-5"));
 checks=-ENODEV; int previous=calls; assert(output_status_show(NULL,NULL,out)==-ENODEV && !p.lock && calls==previous);
 return 0;
}
'''.replace('/* PRODUCTION */',body), flags=['-Wno-unused-parameter','-I'+str(ROOT/'package/kernel/q1000k-pon-control/src')])
    @staticmethod
    def row():
        v=[0]*30
        v[1]=30000<<7; v[4]=31000<<7; v[2]=30500; v[3]=3
        v[6]=12600; v[7]=1; v[8]=6000; v[28]=34364+(46885<<16); v[29]=29491
        return dict(passive_output_version=1,begin_ns=1,end_ns=2,error=0,
                    board_disabled_before=0,board_disabled=0,valid=(1<<30)-1,v=v)

    def summarize(self, row):
        return C.passive_output_summary('passive_output_observation phase=live\n'+json.dumps(row)+'\n')

    def test_no_monitor_current_without_valid_selection(self):
        row=self.row(); result=self.summarize(row); d=result['decoded'][0]
        self.assertEqual(d['hardware_tssi'],[30000,31000])
        self.assertEqual(d['mailbox_tssi'],30500)
        self.assertEqual(d['tx_power_nW'],100)
        self.assertEqual(d['bias_uA'],12000)
        self.assertEqual(d['reporting_status'],3)
        self.assertIsNone(d['monitor_current_uA_oem'])
        self.assertFalse(result['connector_emission_verified'])
        row['v'][17]=1<<26; row['v'][20]=0x2400; row['v'][22]=0x40
        self.assertEqual(self.summarize(row)['decoded'][0]['monitor_current_uA_oem'],220)
        row['valid'] &= ~(1<<20)
        self.assertIsNone(self.summarize(row)['decoded'][0]['monitor_current_uA_oem'])
        row['valid'] &= ~(1<<1)
        self.assertIsNone(self.summarize(row)['decoded'][0]['hardware_tssi'][0])
        for key,value in [('error',-121),('end_ns',1),('begin_ns','bad'),('v',[0]*29),('board_disabled',-1)]:
            bad=copy.deepcopy(row); bad[key]=value
            self.assertEqual(self.summarize(bad)['samples'],0)
            self.assertEqual(self.summarize(bad)['invalid_records'],1)

    def test_discovery_gate_uses_authenticated_requests_not_o5_label(self):
        args=Namespace(suite='discovery',physical_only=False,skip_physical=False,identity='private',rx_only=False,cases=None)
        cases=C.discovery_plan(args)
        self.assertEqual([c['name'] for c in cases[:4]],['rx-startup','activation-omci-discovery-probe','activation-omci-discovery-full','activation-omci-discovery-repeat'])
        self.assertTrue(all(c['requires_authenticated_discovery'] for c in cases[4:]))
        self.assertEqual({c['vlan_untagged'] for c in cases[4:]},{0,1,2})
        fake=dict(status='functional-negative',stages={'cleanup':'passed'},o5_authenticated=True,omci_experiment={'authenticated_rx':0})
        self.assertIsNone(C.discovery_skip(cases[2],[fake]))
        self.assertIsNotNone(C.discovery_skip(cases[4],[fake]))
        fake['omci_experiment']['authenticated_rx']=1
        self.assertIsNotNone(C.discovery_skip(cases[2],[fake]))
        self.assertIsNone(C.discovery_skip(cases[4],[fake]))
        fake['stages']['cleanup']='failed'
        self.assertIsNotNone(C.discovery_skip(cases[4],[fake]))
        args.identity=None
        self.assertEqual([c['name'] for c in C.discovery_plan(args)],['rx-startup'])


if __name__=='__main__': unittest.main()
