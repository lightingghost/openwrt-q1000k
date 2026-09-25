#!/usr/bin/env python3
"""Run the prepared odhcpd's actual allocation function, not a reimplementation.

This isolates the /60-pool -> /61 decision. It does not claim a DHCPv6 packet,
netifd integration, renumbering or hardware test.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativePdTests(unittest.TestCase):
    def test_equal_length_fails_but_61_fits_in_60_after_local_reservation(self):
        sources = list((ROOT/'build_dir/target-aarch64_cortex-a53_musl/odhcpd-ipv6only').glob('odhcpd-*/src/dhcpv6-ia.c'))
        self.assertEqual(len(sources), 1, 'Prepare the pinned odhcpd package first')
        source = sources[0].read_text()
        allocator = source[source.index('static bool assign_pd('):]
        allocator = allocator[:allocator.index('\n/* Check iid')]
        header = sources[0].with_suffix('.h').read_text()
        predicate = header[header.index('static inline bool valid_prefix_length('):]
        predicate = predicate[:predicate.index('\nstatic inline bool valid_addr')]
        with tempfile.TemporaryDirectory(prefix='q1000k-native-pd-') as directory:
            root = Path(directory)
            fixture = root/'allocator.c'
            fixture.write_text('''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <libubox/list.h>
enum { OAF_DHCPV6_NA = 1 };
struct dhcpv6_lease { struct list_head head; uint32_t assigned_subnet_id;
                     uint8_t length; unsigned flags; bool bound; };
struct interface { unsigned addr6_len; struct list_head ia_assignments; };
static void apply_lease(struct dhcpv6_lease *a, bool add) { assert(!"Unbound allocation only"); }
'''+predicate+allocator+'''
static void test(unsigned pool, unsigned length, bool expect, unsigned offset)
{
    struct interface iface = { .addr6_len = 1 };
    struct dhcpv6_lease border = { .length = 64, .assigned_subnet_id = 1U << (64-pool) };
    struct dhcpv6_lease client = { .length = length };
    INIT_LIST_HEAD(&iface.ia_assignments);
    list_add(&border.head, &iface.ia_assignments);
    bool ok = valid_prefix_length(&client, pool) && assign_pd(&iface, &client);
    assert(ok == expect);
    if (ok) assert(client.assigned_subnet_id == offset);
    printf("pool=/%u request=/%u allocated=%d offset=%u\\n", pool, length, ok, client.assigned_subnet_id);
}
int main(void)
{
    test(60, 61, true, 8);
    test(60, 60, false, 0);
    test(61, 61, false, 0);
    test(60, 62, true, 4);
    return 0;
}
''')
            subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-I'+str(ROOT/'staging_dir/host/include'), str(fixture),
                            '-o', str(root/'allocator')], check=True, capture_output=True)
            run = subprocess.run([str(root/'allocator')], text=True, capture_output=True, check=True)
            print(run.stdout, end='')


if __name__ == '__main__':
    unittest.main(verbosity=2)
