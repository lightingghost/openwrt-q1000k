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

xgspon_status() {
	local phy=0 mac=0 uptime=0
	read_identity
	read_firmware
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
	json_add_string limitation 'Optical loader and MAC datapath integration are pending.'
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
	# No authoritative AN7581 control/status ABI yet. Never infer an optical
	# signal, O5, OMCI health or a provisioned service from module presence.
	json_add_null los
	json_add_null registration
	json_add_null omci
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
