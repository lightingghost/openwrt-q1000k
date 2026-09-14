#!/usr/bin/env bash
# Test the complete imported/adapted OMCI module in a disposable UML guest.
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$test_dir/../.." && pwd)
work=$(mktemp -d /tmp/q1000k-omci-core-uml.XXXXXX)
echo "UML OMCI core build and logs: $work"
if [ -n "${Q1000K_UML_BASE:-}" ]; then
	source_tree=$(readlink -f -- "$Q1000K_UML_BASE/source")
	[ -f "$source_tree/Makefile" ] && [ -f "$Q1000K_UML_BASE/.config" ]
	cp -a --reflink=auto "$Q1000K_UML_BASE" "$work/build"
else
	kernels=("$repo"/build_dir/target-*/linux-airoha_an7581/linux-6.18.*)
	[ "${#kernels[@]}" = 1 ] && [ -d "${kernels[0]}" ]
	cp -a --reflink=auto "${kernels[0]}" "$work/source"
	source_tree=$work/source
	make -C "$source_tree" ARCH=um mrproper > "$work/build.log" 2>&1
	make -C "$source_tree" ARCH=um O="$work/build" defconfig >> "$work/build.log" 2>&1
fi
config=("$source_tree/scripts/config" --file "$work/build/.config")
"${config[@]}" --set-str LOCALVERSION '-q1000k-omci-core-test'
for option in UML HOSTFS MODULES PROC_FS SYSFS NET INET NEW_LEDS LEDS_CLASS NVMEM \
	DEBUG_KERNEL DEBUG_LOCK_ALLOC PROVE_LOCKING PROVE_RCU DEBUG_ATOMIC_SLEEP; do
	"${config[@]}" -e "$option"
done
make -C "$source_tree" ARCH=um O="$work/build" olddefconfig >> "$work/build.log" 2>&1
for option in PROVE_LOCKING PROVE_RCU DEBUG_ATOMIC_SLEEP; do
	grep -qx "CONFIG_$option=y" "$work/build/.config"
done
make -C "$source_tree" ARCH=um O="$work/build" -j"${Q1000K_TEST_JOBS:-8}" >> "$work/build.log" 2>&1
cp -a "$repo/package/kernel/q1000k-omci/src" "$work/module"
cat "$test_dir/omci_core_kernel_fixture.c" >> "$work/module/net/xpon/omci/agent.c"
if [ -n "${Q1000K_OMCI_CLI:-}" ]; then
	cat "$test_dir/omci_cli_kernel_fixture.c" >> "$work/module/net/xpon/omci/agent.c"
fi
python3 - "$work/module/net/xpon/omci/core.c" <<'PY'
import os
import sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
s = s.replace('static int __init omci_init(void)', 'int q1000k_omci_core_test(void);\n\nstatic int __init omci_init(void)')
needle = '\tret = netlink_register_notifier(&omci_netlink_nb);\n\tif (ret)\n\t\tgoto err_genl;\n'
assert s.count(needle) == 1
s = s.replace(needle, needle + '\n\tret = q1000k_omci_core_test();\n\tif (ret) {\n\t\tnetlink_unregister_notifier(&omci_netlink_nb);\n\t\tgoto err_genl;\n\t}\n')
if os.environ.get('Q1000K_OMCI_CLI'):
    s=s.replace('int q1000k_omci_core_test(void);',
                'int q1000k_omci_core_test(void);\nint q1000k_omci_cli_setup(void);\nvoid q1000k_omci_cli_cleanup(void);')
    s=s.replace('ret = q1000k_omci_core_test();',
                'ret = q1000k_omci_core_test();\n\tif (!ret) ret = q1000k_omci_cli_setup();')
    s=s.replace('static void __exit omci_exit(void)\n{',
                'static void __exit omci_exit(void)\n{\n\tq1000k_omci_cli_cleanup();')
p.write_text(s)
PY
make -C "$source_tree" ARCH=um O="$work/build" M="$work/module/net/xpon" \
	CONFIG_XPON=m CONFIG_XPON_OMCI=m CONFIG_XPON_OAM= \
	KCFLAGS="-I$work/module/include" modules >> "$work/build.log" 2>&1
cat > "$work/init" <<'INIT'
#!/usr/bin/busybox sh
case "$(/usr/bin/busybox uname -r)" in
	*-q1000k-omci-core-test) ;;
	*) exit 125 ;;
esac
/usr/bin/busybox mount -t proc proc /proc
/usr/bin/busybox insmod "$OMCI_TEST_MODULE"
result=$?
if [ "$result" = 0 ] && [ -n "$OMCI_TEST_CLI" ]; then
	set -e
	echo CLI_STATUS
	"$OMCI_TEST_CLI" -i pon-test status
	echo CLI_SERIAL
	"$OMCI_TEST_CLI" -i pon-test get serial
	echo CLI_QUEUE
	"$OMCI_TEST_CLI" -i pon-test mib 277 0x8003
	echo CLI_MIB
	"$OMCI_TEST_CLI" -i pon-test mib
	echo CLI_SET
	"$OMCI_TEST_CLI" -i pon-test set version 'Q"1000K-test'
	echo CLI_VERSION
	"$OMCI_TEST_CLI" -i pon-test get version
	echo CLI_MISSING
	if "$OMCI_TEST_CLI" -d 999 status; then exit 126; fi
	echo CLI_INVALID
	if "$OMCI_TEST_CLI" -i pon-test set enabled 2; then exit 126; fi
	echo CLI_IDENTITY
	if "$OMCI_TEST_CLI" -i pon-test set serial 123; then exit 126; fi
	/usr/bin/busybox rmmod omci
	echo Q1000K_OMCI_CLI_PASS
fi
echo "Q1000K_OMCI_CORE_TEST_EXIT=$result"
/usr/bin/busybox poweroff -f
INIT
chmod 755 "$work/init"
TMPDIR=/tmp timeout 60 "$work/build/linux" mem=384M rootfstype=hostfs rootflags=/ ro \
	con0=fd:0,fd:1 con=none init="$work/init" \
	"OMCI_TEST_MODULE=$work/module/net/xpon/omci/omci.ko" "OMCI_TEST_CLI=${Q1000K_OMCI_CLI:-}" > "$work/run.log" 2>&1
tr -d '\r' < "$work/run.log" | grep -qx 'Q1000K_OMCI_CORE_TEST_EXIT=0'
grep -q 'Q1000K_OMCI_CORE_PASS' "$work/run.log"
if grep -Eq 'BUG:|WARNING:|Oops:|Kernel panic|INFO:.*(RCU|rcu|lock|task)' "$work/run.log"; then
	echo "Kernel diagnostics found: $work/run.log" >&2
	exit 1
fi
echo 'Full OMCI core UML tests passed.'
if [ -n "${Q1000K_OMCI_CLI:-}" ]; then
	python3 - "$work/run.log" <<'PY'
import json,sys
lines=open(sys.argv[1]).read().splitlines()
values={}
for i,line in enumerate(lines):
    if line.startswith('CLI_'):
        for payload in lines[i+1:]:
            if payload.startswith(('{','[')):
                values[line]=json.loads(payload)
                break
assert values['CLI_STATUS']['authenticated']==1
assert values['CLI_STATUS']['state']==5 and values['CLI_STATUS']['service_error']==0
assert values['CLI_STATUS']['device_id']==1 and values['CLI_STATUS']['ifindex']>0
assert values['CLI_STATUS']['rx_packets']=='0' and values['CLI_STATUS']['temperature_mc'] is None
assert values['CLI_SERIAL']['value']=='5445535400000001'
assert values['CLI_QUEUE']['class_id']==277 and values['CLI_QUEUE']['entity_id']==0x8003
assert len(values['CLI_MIB'])>256
assert values['CLI_SET']=={'ok':True}
assert values['CLI_VERSION']['value']=='Q"1000K-test'
assert values['CLI_MISSING']['error']==-19
assert values['CLI_INVALID']['error']==-22
assert values['CLI_IDENTITY']['error']==-95
assert 'Q1000K_OMCI_CLI_PASS' in lines
print('OMCI userspace CLI/netlink integration passed.')
PY
fi
