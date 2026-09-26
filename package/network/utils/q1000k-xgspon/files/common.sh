# SPDX-License-Identifier: GPL-2.0-only
export LC_ALL=C
. /usr/share/libubox/jshn.sh

# The service reads an immutable committed snapshot. Interactive inspection
# continues to use the ordinary UCI view, including staged changes.
pon_config_get() {
	if [ -n "${Q1000K_UCI_SNAPSHOT:-}" ]; then
		local dir="${Q1000K_UCI_SNAPSHOT%/*}" name="${Q1000K_UCI_SNAPSHOT##*/}"
		uci -q -c "$dir" -C "$dir" -t "$dir" get "$name.$1"
	else
		uci -q get "xgspon.$1"
	fi
}

valid_serial() {
	[ "${#1}" -eq 12 ] || return 1
	printf '%s\n' "$1" | grep -Eq '^[A-Za-z0-9]{4}[0-9A-Fa-f]{8}$'
}

valid_mac() {
	[ "${#1}" -eq 17 ] || return 1
	printf '%s\n' "$1" | grep -Eq '^[0-9A-Fa-f][02468aAcCeE](:[0-9A-Fa-f]{2}){5}$' || return 1
	[ "$1" != '00:00:00:00:00:00' ]
}

factory_only() {
	[ -f /sys/firmware/devicetree/base/quantum,xgspon-service ] &&
		[ ! -f /sys/firmware/devicetree/base/quantum,xgspon-bench ]
}

read_identity() {
	factory_available=0 factory_serial= factory_mac= factory_source=
	factory_lan_mac= factory_unit_serial=
	factory_json=$(/usr/sbin/q1000k-pon-factory inspect 2>/dev/null)
	if json_load "$factory_json"; then
		json_get_var factory_available available
		json_get_var factory_serial serial
		json_get_var factory_mac wan_mac
		json_get_var factory_source source
		json_get_var factory_lan_mac lan_mac
		json_get_var factory_unit_serial unit_serial
	fi
	# Subscriber identity is configurable on both normal and RAM firmware.
	# Calibration still belongs to the physical unit, independently of these
	# overrides, and normal images require the UBI factory volume below.
	serial=$(pon_config_get identity.serial)
	wan_mac=$(pon_config_get identity.wan_mac)
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

# RAM images cannot read NAND. A separately staged, unit-specific calibration
# record enables monitoring without inventing a factory or subscriber identity.
read_calibration() {
	calibration_available=0 calibration_source=
	if [ "$factory_available" = 1 ]; then
		calibration_available=1 calibration_source=factory
	elif ! factory_only && /usr/sbin/q1000k-pon-factory calibration --calibration-file \
		/lib/firmware/airoha/q1000k/xgspon-calibration.bin >/dev/null 2>&1; then
		calibration_available=1 calibration_source=staged
	fi
}

calibration_read() {
	if [ "$calibration_source" = factory ]; then
		/usr/sbin/q1000k-pon-factory calibration
	elif [ "$calibration_source" = staged ]; then
		/usr/sbin/q1000k-pon-factory calibration --calibration-file \
			/lib/firmware/airoha/q1000k/xgspon-calibration.bin
	else
		return 1
	fi
}

# Called only by explicit lifecycle operations, never by status polling.
stop_diagnostic_monitor() {
	[ ! -x /etc/init.d/xgspon ] || /etc/init.d/xgspon stop_monitor
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
	if [ -x /usr/sbin/omci ] && \
	   data=$(/usr/sbin/omci -i pon status 2>/dev/null) && json_load "$data"; then
		json_get_type type schema_version
		json_get_var version schema_version
		[ "$type" = int ] && [ "$version" = 1 ] && omci_available=1
	fi
	json_set_namespace "$previous"
}

# Passive, fixed sysfs operation. Each request starts with an empty namespace;
# a failed refresh must never reuse the previous sample. The published DDMI
# values are asynchronous and do not prove power at the optical connector.
read_optical() {
	local path data previous type version
	optical_available=0
	json_set_namespace q1000k_optical previous
	json_init
	path=$(find_controller)
	if [ -n "$path" ] && data=$(cat "$path/transmitter_status" 2>/dev/null) && json_load "$data"; then
		json_get_type type transmitter_version
		json_get_var version transmitter_version
		[ "$type" = int ] && [ "$version" = 1 ] && optical_available=1
	fi
	json_set_namespace "$previous"
}

optical_reading() {
	local name="$1" unit="$2" previous type valid error actual number=
	json_set_namespace q1000k_optical previous
	if json_select fields && json_select "$name"; then
		json_get_type type valid
		json_get_var valid valid
		if [ "$type" = boolean ] && [ "$valid" = 1 ]; then
			json_get_type type error
			json_get_var error error
			if [ "$type" = int ] && [ "$error" = 0 ]; then
				json_get_var actual unit
				json_get_type type value
				[ "$actual" != "$unit" ] || [ "$type" != int ] || json_get_var number value
			fi
		fi
	fi
	# Always restore the namespace's root, even after an absent child.
	json_select
	json_set_namespace "$previous"
	json_add_object "$name"
	json_add_string unit "$unit"
	if [ -n "$number" ]; then json_add_int value "$number"; else json_add_null value; fi
	# No verified EN7573 threshold/alarm ABI: unknown is not a clear alarm.
	json_add_null thresholds
	json_add_null alarms
	json_close_object
}

optical_status() {
	if [ "$optical_available" != 1 ]; then json_add_null optical; return; fi
	json_add_object optical
	json_add_string source 'EN7573 published DDMI'
	json_add_boolean sensor_refresh_verified 0
	json_add_boolean connector_emission_verified 0
	json_add_string thresholds_status 'unavailable'
	json_add_object readings
	optical_reading temperature mC
	optical_reading supply uV
	optical_reading bias uA
	optical_reading tx_power nW
	optical_reading rx_power nW
	json_close_object
	json_close_object
}

link_status() {
	local carrier
	json_add_object link
	# A carrier indication describes the netdevice, not end-to-end Internet.
	carrier=$(cat /sys/class/net/pon/carrier 2>/dev/null) || carrier=
	case "$carrier" in
		0|1) json_add_boolean carrier "$carrier" ;;
		*) json_add_null carrier ;;
	esac
	json_close_object
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
	local available=0 enabled=0 monitor=1 data version type stage= error= previous
	[ -x /etc/init.d/xgspon ] && available=1
	[ "$(uci -q get xgspon.service.enabled)" = 1 ] && enabled=1
	[ "$(uci -q get xgspon.service.monitor)" != 0 ] || monitor=0
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
	json_add_boolean monitor_enabled "$monitor"
	if [ -n "$stage" ]; then json_add_string last_stage "$stage"; else json_add_null last_stage; fi
	if [ -n "$error" ]; then json_add_int last_error "$error"; else json_add_null last_error; fi
	json_close_object
}

xgspon_status() {
	local phy=0 mac=0 uptime=0 supported=0 ram=0
	read_identity
	read_firmware
	read_calibration
	read_controller
	read_omci
	read_optical
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
	[ -x /etc/init.d/xgspon ] && supported=1
	[ -f /sys/firmware/devicetree/base/quantum,xgspon-bench ] && ram=1
	json_add_boolean activation_supported "$supported"
	json_add_boolean ram_bench "$ram"
	json_add_string limitation 'Optical readings require the controller, verified OEM firmware and unit calibration. Registration also requires a configured subscriber identity and MAC/OMCI startup.'
	supervisor_status
	link_status
	json_add_object calibration
	json_add_boolean available "$calibration_available"
	json_add_string source "$calibration_source"
	json_close_object
	json_add_object factory
	json_add_boolean available "${factory_available:-0}"
	json_add_string source "$factory_source"
	json_add_string serial "$factory_serial"
	json_add_string wan_mac "$factory_mac"
	json_add_string lan_mac "$factory_lan_mac"
	json_add_string unit_serial "$factory_unit_serial"
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
	controller_field tx_inhibited boolean
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
		             telemetry_valid temperature_mc voltage_uv bias_ua rx_power_nw tx_power_nw; do
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
	optical_status
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
identity_keys='serial vendor_id equipment_id hardware_version sync_circuit_pack software_version_a software_version_b active_bank committed_bank registration_id logical_onu_id logical_password wan_mac omci_version mib_profile fix_vlans omcc_version pon_slot iphost_mac iphost_hostname iphost_domain olt_profile'
identity_option_valid() {
	local key="$1" value="$2" maximum
	case " $identity_keys " in *" $key "*) ;; *) return 1 ;; esac
	[ -n "$value" ] || return 0
	case "$value" in *'
'*) return 1 ;; esac
	case "$key" in
		serial) valid_serial "$value" ;;
		wan_mac|iphost_mac) valid_mac "$value" ;;
		omcc_version) printf '%s\n' "$value" | grep -Eq '^0x[89aAbB][0-9a-fA-F]$' ;;
		pon_slot)
			printf '%s\n' "$value" | grep -Eq '^[1-9][0-9]{0,2}$' &&
				[ "$value" -le 254 ] && [ "$value" != 128 ] ;;
		olt_profile) case "$value" in auto|generic|nokia|dasan|huawei|fiberhome|zte) return 0 ;; *) return 1 ;; esac ;;
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
				iphost_hostname|iphost_domain) maximum=25 ;;
				*) maximum=14 ;;
			esac
			[ "${#value}" -le "$maximum" ] &&
				! printf '%s' "$value" | grep -q '[^ -~]'
			;;
	esac
}
identity_option_read() {
	# Keep embedded/trailing newlines so they are rejected, not silently stripped.
	identity_value=$(pon_config_get "identity.$1"; printf '.')
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
			olt_profile)
				case "$identity_value" in generic) param=1 ;; auto) param=2 ;; nokia) param=3 ;; dasan) param=4 ;; huawei) param=5 ;; fiberhome) param=6 ;; zte) param=7 ;; esac
				identity_params="$identity_params pon_olt_profile=$param"
				continue ;;
			pon_slot)
				identity_params="$identity_params pon_uni_slot=$identity_value"
				continue ;;
			omcc_version|iphost_mac)
				identity_params="$identity_params pon_$key=$identity_value"
				continue ;;
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
