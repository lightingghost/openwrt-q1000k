#!/usr/bin/env bash
# Build and boot an independent UML kernel; never modify the target build tree.
set -euo pipefail

test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$test_dir/../.." && pwd)
kernels=("$repo"/build_dir/target-*/linux-airoha_an7581/linux-6.18.*)
[ "${#kernels[@]}" = 1 ] && [ -d "${kernels[0]}" ] || {
	echo 'Prepare one AN7581 kernel with make target/linux/prepare first.' >&2
	exit 1
}
work=$(mktemp -d /tmp/q1000k-bridge-uml.XXXXXX)
echo "UML build and logs: $work"
cp -a --reflink=auto "${kernels[0]}" "$work/source"
make -C "$work/source" ARCH=um mrproper > "$work/build.log" 2>&1
make -C "$work/source" ARCH=um O="$work/build" defconfig >> "$work/build.log" 2>&1
config=("$work/source/scripts/config" --file "$work/build/.config")
"${config[@]}" --set-str LOCALVERSION '-q1000k-uml-test'
for symbol in HOSTFS DEVTMPFS DEVTMPFS_MOUNT PROC_FS SYSFS TMPFS NET INET IPV6 \
	NET_NS USER_NS VETH BRIDGE BRIDGE_VLAN_FILTERING VLAN_8021Q NETFILTER \
	NETFILTER_ADVANCED NETFILTER_INGRESS NF_CONNTRACK NF_CONNTRACK_EVENTS \
	NF_CONNTRACK_PROCFS NF_CONNTRACK_MARK NF_CONNTRACK_BRIDGE NF_TABLES \
	NF_TABLES_INET NF_TABLES_BRIDGE NF_TABLES_NETDEV NFT_CT NFT_COUNTER \
	NFT_FLOW_OFFLOAD NF_FLOW_TABLE_INET NF_FLOW_TABLE NF_FLOW_TABLE_PROCFS \
	NFT_BRIDGE_META BLK_DEV_INITRD RD_GZIP WERROR; do
	"${config[@]}" -e "$symbol"
done
make -C "$work/source" ARCH=um O="$work/build" olddefconfig >> "$work/build.log" 2>&1
make -C "$work/source" ARCH=um O="$work/build" -j"${Q1000K_TEST_JOBS:-8}" >> "$work/build.log" 2>&1
# Run as the current user. UML needs ptrace of its own child processes.
# The init script mounts guest-only tmpfs for all writable runtime state.
TMPDIR=/tmp "$work/build/linux" mem=256M rootfstype=hostfs rootflags=/ ro \
	con0=fd:0,fd:1 con=none init="$test_dir/uml-init.sh" > "$work/run.log" 2>&1
cat "$work/run.log"
tr -d '\r' < "$work/run.log" | grep -qx 'Q1000K_BRIDGE_TEST_EXIT=0'
! grep -Eq 'BUG:|WARNING: CPU:|Oops:|Kernel panic' "$work/run.log"
