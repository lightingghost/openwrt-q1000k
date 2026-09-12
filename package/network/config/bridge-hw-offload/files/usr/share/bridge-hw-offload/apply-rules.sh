#!/bin/sh
# Keep the runtime bridge fragment and the live fw4 ruleset in sync.
# The packaged ruleset-post include removes the old table even when disabled.

BRIDGE="${1:-br-lan}"
RULES_DIR=/var/run/bridge-hw-offload
RULES_FILE="$RULES_DIR/bridge.nft"

log_error() {
	logger -t bridge-hw-offload "$*"
	echo "bridge-hw-offload: $*" >&2
}

case "$BRIDGE" in ''|*[!a-zA-Z0-9_.:-]*) log_error 'Invalid bridge name'; exit 1;; esac
mkdir -p "$RULES_DIR" /var/lock || exit 1
exec 9>/var/lock/bridge-hw-offload.lock
flock -x 9 || exit 1

candidate=$(mktemp "$RULES_DIR/.candidate.XXXXXX") || exit 1
previous=$(mktemp "$RULES_DIR/.previous.XXXXXX") || exit 1
check=$(mktemp "$RULES_DIR/.check.XXXXXX") || exit 1
trap 'rm -f "$candidate" "$previous" "$check"' EXIT
trap 'exit 1' HUP INT TERM

if [ "$(uci -q get firewall.@defaults[0].flow_offloading)" = 1 ] &&
   [ "$(uci -q get firewall.@defaults[0].flow_offloading_hw)" = 1 ]; then
	devices=""
	count=0
	for path in "/sys/class/net/$BRIDGE/brif/"*; do
		[ -d "$path" ] || continue
		port=${path##*/}
		case "$port" in ''|*[!a-zA-Z0-9_.:-]*) log_error 'Invalid bridge port name'; exit 1;; esac
		devices="${devices:+$devices, }\"$port\""
		count=$((count + 1))
	done
	if [ "$count" -ge 2 ]; then
		cat > "$candidate" <<RULES
table bridge fw4 {
	flowtable br_offload {
		hook ingress priority 0; devices = { $devices };
		counter; flags offload;
	}
	chain forward {
		type filter hook forward priority 10; policy accept;
		iifname { $devices } oifname { $devices } ct state established meta l4proto { tcp, udp } counter flow offload @br_offload
	}
}
RULES
	fi
fi

# Check only our table before changing the fragment. A check or fw4 failure
# leaves the previous live ruleset intact and reports the actual error.
{
	echo 'destroy table bridge fw4'
	cat "$candidate"
} > "$check"
if ! nft -c -f "$check"; then
	log_error 'Bridge flowtable validation failed; check kernel/module support'
	exit 1
fi

had_previous=0
if [ -f "$RULES_FILE" ]; then
	cp "$RULES_FILE" "$previous" || exit 1
	had_previous=1
fi
mv "$candidate" "$RULES_FILE" || exit 1
if ! /etc/init.d/firewall reload; then
	if [ "$had_previous" = 1 ]; then
		mv "$previous" "$RULES_FILE"
	else
		rm -f "$RULES_FILE"
	fi
	log_error 'Firewall reload failed; restored the previous bridge fragment'
	exit 1
fi
if [ -s "$RULES_FILE" ]; then
	logger -t bridge-hw-offload "Hardware flow offload configured on $BRIDGE: $devices"
else
	logger -t bridge-hw-offload 'Bridge flow offload disabled or fewer than two ports available'
fi
