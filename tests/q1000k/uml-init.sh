#!/usr/bin/busybox sh
# Boot only in the disposable UML kernel, using a read-only hostfs root.
case "$(/usr/bin/busybox uname -r)" in
    *-q1000k-uml-test) ;;
    *) echo 'This init script is only for the disposable Q1000K UML test kernel.'; exit 1;;
esac
SCRIPT_DIR=${0%/*}
/usr/bin/busybox mount -t proc proc /proc
/usr/bin/busybox mount -t sysfs sysfs /sys
/usr/bin/busybox mount -t tmpfs tmpfs /tmp
/usr/bin/busybox mount -t tmpfs tmpfs /run
/usr/bin/busybox mount -t devtmpfs devtmpfs /dev
/usr/bin/busybox ln -s /proc/self/fd /dev/fd
/usr/bin/busybox ln -s /proc/self/fd/0 /dev/stdin
/usr/bin/busybox ln -s /proc/self/fd/1 /dev/stdout
/usr/bin/busybox ln -s /proc/self/fd/2 /dev/stderr
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
python3 "$SCRIPT_DIR/test_bridge_datapath.py"
status=$?
echo "Q1000K_BRIDGE_TEST_EXIT=$status"
/usr/bin/busybox poweroff -f
