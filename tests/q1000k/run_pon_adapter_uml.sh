#!/usr/bin/env bash
# Execute only extracted adapter routines in a disposable UML kernel.
set -euo pipefail

test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$test_dir/../.." && pwd)
work=$(mktemp -d /tmp/q1000k-pon-adapter-uml.XXXXXX)
echo "UML adapter build and logs: $work"
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
"${config[@]}" --set-str LOCALVERSION '-q1000k-pon-adapter-test'
for option in UML HOSTFS MODULES PROC_FS DEBUG_KERNEL DEBUG_LOCK_ALLOC \
	PROVE_LOCKING PROVE_RCU DEBUG_ATOMIC_SLEEP; do
	"${config[@]}" -e "$option"
done
make -C "$source_tree" ARCH=um O="$work/build" olddefconfig >> "$work/build.log" 2>&1
for option in PROVE_LOCKING PROVE_RCU DEBUG_ATOMIC_SLEEP; do
	grep -qx "CONFIG_$option=y" "$work/build/.config"
done
make -C "$source_tree" ARCH=um O="$work/build" -j"${Q1000K_TEST_JOBS:-8}" >> "$work/build.log" 2>&1
mkdir "$work/module"
python3 "$test_dir/pon_adapter_kernel_test.py" > "$work/module/pon_adapter_test.c"
printf '%s\n' 'obj-m := pon_adapter_test.o' > "$work/module/Makefile"
make -C "$source_tree" ARCH=um O="$work/build" M="$work/module" modules >> "$work/build.log" 2>&1
cat > "$work/init" <<'INIT'
#!/usr/bin/busybox sh
case "$(/usr/bin/busybox uname -r)" in
	*-q1000k-pon-adapter-test) ;;
	*) exit 125 ;;
esac
/usr/bin/busybox mount -t proc proc /proc
/usr/bin/busybox insmod "$PON_ADAPTER_TEST_MODULE"
result=$?
echo "Q1000K_PON_ADAPTER_TEST_EXIT=$result"
/usr/bin/busybox poweroff -f
INIT
chmod 755 "$work/init"
# Only a synthetic test netdevice is registered; no NIC or optical hardware is attached.
TMPDIR=/tmp timeout 60 "$work/build/linux" mem=256M rootfstype=hostfs rootflags=/ ro \
	con0=fd:0,fd:1 con=none init="$work/init" \
	"PON_ADAPTER_TEST_MODULE=$work/module/pon_adapter_test.ko" > "$work/run.log" 2>&1
tr -d '\r' < "$work/run.log" | grep -qx 'Q1000K_PON_ADAPTER_TEST_EXIT=0'
grep -q 'Q1000K_PON_ADAPTER_KERNEL_PASS' "$work/run.log"
if grep -Eq 'BUG:|WARNING:|Oops:|Kernel panic|INFO:.*(RCU|rcu|lock|task)' "$work/run.log"; then
	echo "Kernel diagnostics found: $work/run.log" >&2
	exit 1
fi
echo 'Kernel PON packet adapter queue and lifetime tests passed.'
