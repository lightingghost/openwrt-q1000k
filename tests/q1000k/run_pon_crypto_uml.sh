#!/usr/bin/env bash
# Execute only extracted cryptographic helpers in a disposable UML kernel.
set -euo pipefail

test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$test_dir/../.." && pwd)
work=$(mktemp -d /tmp/q1000k-pon-crypto-uml.XXXXXX)
echo "UML crypto build and logs: $work"
if [ -n "${Q1000K_UML_BASE:-}" ]; then
	# Reuse a previously prepared UML build without modifying its source/config.
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
"${config[@]}" --set-str LOCALVERSION '-q1000k-pon-crypto-test'
for option in UML HOSTFS MODULES PROC_FS CRYPTO CRYPTO_MANAGER CRYPTO_SKCIPHER \
	CRYPTO_AES CRYPTO_ECB; do
	"${config[@]}" -e "$option"
done
make -C "$source_tree" ARCH=um O="$work/build" olddefconfig >> "$work/build.log" 2>&1
make -C "$source_tree" ARCH=um O="$work/build" -j"${Q1000K_TEST_JOBS:-8}" >> "$work/build.log" 2>&1
mkdir "$work/module"
python3 "$test_dir/pon_crypto_kernel_test.py" > "$work/module/pon_crypto_test.c"
printf '%s\n' 'obj-m := pon_crypto_test.o' > "$work/module/Makefile"
make -C "$source_tree" ARCH=um O="$work/build" M="$work/module" modules >> "$work/build.log" 2>&1
cat > "$work/init" <<'INIT'
#!/usr/bin/busybox sh
# Never run guest setup or shutdown on a normal host/router kernel.
case "$(/usr/bin/busybox uname -r)" in
	*-q1000k-pon-crypto-test) ;;
	*) exit 125 ;;
esac
/usr/bin/busybox mount -t proc proc /proc
/usr/bin/busybox insmod "$PON_CRYPTO_TEST_MODULE"
result=$?
echo "Q1000K_PON_CRYPTO_TEST_EXIT=$result"
/usr/bin/busybox poweroff -f
INIT
chmod 755 "$work/init"
# No network device, optical driver or target firmware is part of this guest.
# hostfs is read-only; only the invoking shell writes local build/run logs.
TMPDIR=/tmp timeout 60 "$work/build/linux" mem=256M rootfstype=hostfs rootflags=/ ro \
	con0=fd:0,fd:1 con=none init="$work/init" \
	"PON_CRYPTO_TEST_MODULE=$work/module/pon_crypto_test.ko" > "$work/run.log" 2>&1
tr -d '\r' < "$work/run.log" | grep -qx 'Q1000K_PON_CRYPTO_TEST_EXIT=0'
grep -q 'Q1000K_PON_CRYPTO_KERNEL_PASS' "$work/run.log"
! grep -Eq 'BUG:|WARNING: CPU:|Oops:|Kernel panic' "$work/run.log"
echo 'Kernel AES/CMAC known-answer tests passed.'
