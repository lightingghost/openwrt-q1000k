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
python3 - "$work/module/net/xpon/omci/core.c" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
s = s.replace('static int __init omci_init(void)', 'int q1000k_omci_core_test(void);\n\nstatic int __init omci_init(void)')
needle = '\tret = netlink_register_notifier(&omci_netlink_nb);\n\tif (ret)\n\t\tgoto err_genl;\n'
assert s.count(needle) == 1
s = s.replace(needle, needle + '\n\tret = q1000k_omci_core_test();\n\tif (ret) {\n\t\tnetlink_unregister_notifier(&omci_netlink_nb);\n\t\tgoto err_genl;\n\t}\n')
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
echo "Q1000K_OMCI_CORE_TEST_EXIT=$result"
/usr/bin/busybox poweroff -f
INIT
chmod 755 "$work/init"
TMPDIR=/tmp timeout 60 "$work/build/linux" mem=384M rootfstype=hostfs rootflags=/ ro \
	con0=fd:0,fd:1 con=none init="$work/init" \
	"OMCI_TEST_MODULE=$work/module/net/xpon/omci/omci.ko" > "$work/run.log" 2>&1
tr -d '\r' < "$work/run.log" | grep -qx 'Q1000K_OMCI_CORE_TEST_EXIT=0'
grep -q 'Q1000K_OMCI_CORE_PASS' "$work/run.log"
if grep -Eq 'BUG:|WARNING:|Oops:|Kernel panic|INFO:.*(RCU|rcu|lock|task)' "$work/run.log"; then
	echo "Kernel diagnostics found: $work/run.log" >&2
	exit 1
fi
echo 'Full OMCI core UML tests passed.'
