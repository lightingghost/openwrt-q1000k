# SPDX-License-Identifier: GPL-2.0-only
export LC_ALL=C
. /usr/share/libubox/jshn.sh

valid_serial() {
	[ "${#1}" -eq 12 ] || return 1
	printf '%s\n' "$1" | grep -Eq '^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$'
}

valid_mac() {
	[ "${#1}" -eq 17 ] || return 1
	printf '%s\n' "$1" | grep -Eq '^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$' || return 1
	[ "$1" != '00:00:00:00:00:00' ]
}

read_identity() {
	factory_available=0 factory_serial= factory_mac= factory_source=
	factory_json=$(/usr/sbin/q1000k-pon-factory inspect 2>/dev/null)
	if json_load "$factory_json"; then
		json_get_var factory_available available
		json_get_var factory_serial serial
		json_get_var factory_mac wan_mac
		json_get_var factory_source source
	fi
	serial=$(uci -q get q1000k-xgspon.identity.serial)
	wan_mac=$(uci -q get q1000k-xgspon.identity.wan_mac)
	serial_source=override mac_source=override
	[ -n "$serial" ] || { serial=$factory_serial; serial_source=factory; }
	[ -n "$wan_mac" ] || { wan_mac=$factory_mac; mac_source=factory; }
	identity_valid=1
	valid_serial "$serial" && valid_mac "$wan_mac" || identity_valid=0
}

firmware_check() {
	local path="$1" expected="$2" actual
	[ -f "$path" ] && [ ! -L "$path" ] || return 1
	actual=$(sha256sum "$path" 2>/dev/null)
	[ "${actual%% *}" = "$expected" ]
}

read_firmware() {
	pm_ready=0 dm_ready=0
	firmware_check /lib/firmware/airoha/q1000k/A60993.elf.pm \
		5a8a4bbae5f70c1e615ba0aa1c2a1dce654611d3205d2fa983bf41e6cdadb4a1 && pm_ready=1
	firmware_check /lib/firmware/airoha/q1000k/A60993.elf.dm \
		21618dc3694a1e6f6b28c7da7141964dea1d6e57f2d2956bbe72a780ca6166a4 && dm_ready=1
}

find_controller() {
	local path found=
	for path in /sys/bus/i2c/drivers/q1000k-pon-control/*-0051; do
		[ -r "$path/status" ] && [ -w "$path/operation" ] || continue
		[ -z "$found" ] || return 1
		found=$path
	done
	[ -n "$found" ] || return 1
	printf '%s\n' "$found"
}

read_controller() {
	local path data previous version
	controller_available=0
	json_set_namespace q1000k_controller previous
	json_init
	path=$(find_controller)
	if [ -n "$path" ]; then
		data=$(cat "$path/status" 2>/dev/null)
		if json_load "$data"; then
			json_get_var version schema_version
			[ "$version" = 1 ] && controller_available=1
		fi
	fi
	json_set_namespace "$previous"
}

controller_field() {
	local name="$1" expected="$2" previous type value
	json_set_namespace q1000k_controller previous
	json_get_type type "$name"
	json_get_var value "$name"
	json_set_namespace "$previous"
	if [ "$controller_available" = 1 ] && [ "$type" = "$expected" ]; then
		case "$type" in
			boolean) json_add_boolean "$name" "$value" ;;
			int) json_add_int "$name" "$value" ;;
			string) json_add_string "$name" "$value" ;;
		esac
	else
		json_add_null "$name"
	fi
}

# Use only the command's fixed, read-only status operation. Keep its parser
# namespace separate from factory/controller data and never reuse a stale sample.
read_omci() {
	local data previous version type
	omci_available=0
	json_set_namespace q1000k_omci previous
	json_init
	if [ -x /usr/sbin/q1000k-omci ] && \
	   data=$(/usr/sbin/q1000k-omci -i pon status 2>/dev/null) && json_load "$data"; then
		json_get_type type schema_version
		json_get_var version schema_version
		[ "$type" = int ] && [ "$version" = 1 ] && omci_available=1
	fi
	json_set_namespace "$previous"
}

omci_field() {
	local name="$1" expected="$2" output="${3:-$1}" previous type value
	json_set_namespace q1000k_omci previous
	json_get_type type "$name"
	json_get_var value "$name"
	json_set_namespace "$previous"
	if [ "$omci_available" = 1 ] && [ "$type" = "$expected" ]; then
		case "$type" in
			int) json_add_int "$output" "$value" ;;
			string) json_add_string "$output" "$value" ;;
		esac
	else
		json_add_null "$output"
	fi
}

supervisor_status() {
	local available=0 enabled=0 data version type stage= error= previous
	[ -x /etc/init.d/q1000k-xgspon ] && available=1
	[ "$(uci -q get q1000k-xgspon.service.enabled)" = 1 ] && enabled=1
	# This is the last recorded state, which can survive an abrupt process exit.
	# It does not prove that a supervisor is alive or that optical service works.
	json_set_namespace q1000k_supervisor previous
	json_init
	if data=$(cat /var/run/q1000k-xgspon/status.json 2>/dev/null) && json_load "$data"; then
		json_get_type type schema_version
		json_get_var version schema_version
		if [ "$type" = int ] && [ "$version" = 1 ]; then
			json_get_type type stage
			[ "$type" != string ] || json_get_var stage stage
			json_get_type type error
			[ "$type" != int ] || json_get_var error error
		fi
	fi
	json_set_namespace "$previous"
	json_add_object supervisor
	json_add_boolean available "$available"
	json_add_boolean enabled "$enabled"
	if [ -n "$stage" ]; then json_add_string last_stage "$stage"; else json_add_null last_stage; fi
	if [ -n "$error" ]; then json_add_int last_error "$error"; else json_add_null last_error; fi
	json_close_object
}

xgspon_status() {
	local phy=0 mac=0 uptime=0
	read_identity
	read_firmware
	read_controller
	read_omci
	[ -d /sys/module/phy_10g ] && phy=1
	[ -d /sys/module/xpon_10g ] && mac=1
	read -r uptime ignored < /proc/uptime
	json_init
	json_add_int schema_version 1
	json_add_int sampled_uptime "${uptime%%.*}"
	json_add_string model 'Quantum Fiber Q1000K'
	json_add_string soc 'AN7581SIT'
	json_add_string optics '2 × EN7573AN'
	json_add_string mode 'XGS-PON'
	json_add_boolean activation_supported 0
	json_add_string limitation 'The optional supervisor supports explicit experimental startup; optical operation and hardware acceptance remain unverified.'
	supervisor_status
	json_add_object factory
	json_add_boolean available "${factory_available:-0}"
	json_add_string source "$factory_source"
	json_add_string serial "$factory_serial"
	json_add_string wan_mac "$factory_mac"
	json_close_object
	json_add_object identity
	json_add_boolean valid "$identity_valid"
	json_add_string serial "$serial"
	json_add_string serial_source "$serial_source"
	json_add_string wan_mac "$wan_mac"
	json_add_string mac_source "$mac_source"
	json_close_object
	json_add_object firmware
	json_add_boolean program_verified "$pm_ready"
	json_add_boolean data_verified "$dm_ready"
	json_close_object
	json_add_object modules
	json_add_boolean phy_loaded "$phy"
	json_add_boolean mac_loaded "$mac"
	json_close_object
	json_add_object controller
	json_add_boolean available "$controller_available"
	controller_field mode string
	controller_field stage string
	controller_field gpon_detected boolean
	controller_field xgspon_detected boolean
	controller_field checked_uptime int
	controller_field md32_enabled boolean
	controller_field tx_disabled boolean
	controller_field firmware_verified boolean
	controller_field calibration_supplied boolean
	controller_field last_error int
	json_close_object
	controller_field los boolean
	omci_field state int registration
	if [ "$omci_available" = 1 ]; then
		json_add_object omci
		for field in device_id ifindex onu_id gem_port_id authenticated agent_enabled \
		             agent_operational service_rules service_error mib_sync mib_objects olt_profile \
		             telemetry_valid rx_power_nw tx_power_nw; do
			omci_field "$field" int
		done
		for field in rx_packets rx_dropped tx_packets tx_errors responses unsupported; do
			omci_field "$field" string
		done
		json_close_object
	else
		json_add_null omci
	fi
	# Configured rules may be dormant: they do not establish Internet service.
	json_add_null service_ready
	json_add_null optical
	json_dump
}

xgspon_validate() {
	read_identity
	json_init
	json_add_boolean valid "$identity_valid"
	[ "$identity_valid" = 1 ] || json_add_string error 'A valid factory identity or serial/MAC override is required.'
	json_dump
	[ "$identity_valid" = 1 ]
}

# Identity schema shared by the staged OMCI CLI and both launchers.
identity_keys='serial vendor_id equipment_id hardware_version sync_circuit_pack software_version_a software_version_b active_bank committed_bank registration_id logical_onu_id logical_password wan_mac omci_version mib_profile fix_vlans'
identity_option_valid() {
	local key="$1" value="$2" maximum
	case " $identity_keys " in *" $key "*) ;; *) return 1 ;; esac
	[ -n "$value" ] || return 0
	case "$value" in *'
'*) return 1 ;; esac
	case "$key" in
		serial) valid_serial "$value" ;;
		wan_mac) valid_mac "$value" ;;
		vendor_id) [ "${#value}" = 4 ] && printf '%s\n' "$value" | grep -Eq '^[A-Za-z0-9]{4}$' ;;
		registration_id)
			[ "${#value}" -le 72 ] && [ "$(( ${#value} % 2 ))" = 0 ] &&
				printf '%s\n' "$value" | grep -Eq '^[0-9a-fA-F]+$' ;;
		sync_circuit_pack|active_bank|committed_bank|fix_vlans) [ "$value" = 0 ] || [ "$value" = 1 ] ;;
		mib_profile) [ "$value" = native-pptp ] ;;
		*)
			case "$key" in
				equipment_id) maximum=20 ;;
				logical_onu_id) maximum=24 ;;
				logical_password) maximum=12 ;;
				*) maximum=14 ;;
			esac
			[ "${#value}" -le "$maximum" ] &&
				! printf '%s' "$value" | grep -q '[^ -~]'
			;;
	esac
}
identity_option_read() {
	# Keep embedded/trailing newlines so they are rejected, not silently stripped.
	identity_value=$(uci -q get "q1000k-xgspon.identity.$1"; printf '.')
	identity_value=${identity_value%.}
	identity_value=${identity_value%'
'}
	identity_option_valid "$1" "$identity_value"
}
identity_options() {
	local key param encoded
	identity_params= registration=
	for key in $identity_keys; do
		identity_option_read "$key" || { echo "Invalid PON identity option: $key" >&2; return 1; }
		[ -n "$identity_value" ] || continue
		case "$key" in
			serial|wan_mac|mib_profile) continue ;;
			registration_id)
				registration=$identity_value
				while [ "${#registration}" -lt 72 ]; do registration="${registration}00"; done
				continue ;;
			sync_circuit_pack|active_bank|committed_bank|fix_vlans)
				identity_params="$identity_params pon_$key=$identity_value"
				continue ;;
			software_version_a) param=software0 ;;
			software_version_b) param=software1 ;;
			omci_version) param=omci_version ;;
			*) param=$key ;;
		esac
		encoded=$(printf '%s' "$identity_value" | hexdump -v -e '1/1 "%02x"') || return 1
		identity_params="$identity_params pon_${param}_hex=$encoded"
	done
	identity_value=
}
