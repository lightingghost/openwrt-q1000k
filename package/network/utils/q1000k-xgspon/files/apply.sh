#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Shared committed-config validation and keys for explicit XGS-PON applies.
. /lib/q1000k-xgspon/common.sh

xgspon_config_value() {
	xgspon_value=$(pon_config_get "$1"; printf '.')
	xgspon_value=${xgspon_value%.}
	xgspon_value=${xgspon_value%'
'}
}

xgspon_config_sections() {
	uci -q export "$Q1000K_UCI_SNAPSHOT" >/dev/null 2>&1 &&
		[ "$(pon_config_get identity)" = identity ] &&
		[ "$(pon_config_get service)" = service ] &&
		[ "$(pon_config_get passthrough)" = passthrough ]
}

xgspon_optical_values() {
	xgspon_config_value service.enabled; xgspon_enabled=${xgspon_value:-0}
	xgspon_config_value service.monitor; xgspon_monitor=${xgspon_value:-1}
	xgspon_config_value service.lower; xgspon_lower=$xgspon_value
	xgspon_config_value service.continuous_bench; xgspon_continuous=${xgspon_value:-0}
}

xgspon_validate_optical() {
	xgspon_config_sections || return 1
	xgspon_optical_values
	case "$xgspon_enabled:$xgspon_monitor:$xgspon_continuous" in
		0:0:0|0:0:1|0:1:0|0:1:1|1:0:0|1:0:1|1:1:0|1:1:1) ;;
		*) return 1 ;;
	esac
	if [ "$xgspon_enabled" = 1 ]; then
		[ -n "$xgspon_lower" ] && [ "${#xgspon_lower}" -le 15 ] &&
			printf '%s\n' "$xgspon_lower" | grep -Eq '^[A-Za-z0-9_.-]+$' || return 1
	fi
	if [ "$xgspon_enabled:$xgspon_monitor" != 0:0 ]; then
		identity_options >/dev/null 2>&1 || return 1
		[ "$xgspon_enabled" != 1 ] || [ -n "$registration" ] || return 1
	fi
}

xgspon_optical_key() {
	local key
	for key in $identity_keys; do
		xgspon_config_value "identity.$key"
		printf '%s:%s:%s\n' "$key" "${#xgspon_value}" "$xgspon_value"
	done
	xgspon_optical_values
	for key in enabled monitor lower continuous_bench; do
		case "$key" in
			enabled) xgspon_value=$xgspon_enabled ;;
			monitor) xgspon_value=$xgspon_monitor ;;
			lower) xgspon_value=$xgspon_lower ;;
			continuous_bench) xgspon_value=$xgspon_continuous ;;
		esac
		printf '%s:%s:%s\n' "$key" "${#xgspon_value}" "$xgspon_value"
	done
}

xgspon_passthrough_values() {
	xgspon_config_value passthrough.mode; xgspon_mode=${xgspon_value:-router}
	xgspon_config_value passthrough.client_mac
	xgspon_mac=$(printf '%s' "$xgspon_value" | tr 'A-F' 'a-f')
}

xgspon_validate_passthrough() {
	xgspon_config_sections || return 1
	xgspon_passthrough_values
	case "$xgspon_mode" in
		router) ;;
		l3) valid_mac "$xgspon_mac" ;;
		*) return 1 ;;
	esac
}

xgspon_passthrough_key() {
	xgspon_passthrough_values
	printf 'mode:%s:%s\nmac:%s:%s\n' "${#xgspon_mode}" "$xgspon_mode" \
		"${#xgspon_mac}" "$xgspon_mac"
}
