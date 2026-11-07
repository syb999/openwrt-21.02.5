#!/bin/sh

board_net_params() {
	WAN_DEV=""
	HAS_LAN=0
	FACTORY_MAC=""

	case "$1" in
	sumavision,q5)
		WAN_DEV=eth0
		HAS_LAN=0
		FACTORY_MAC=
		;;
	*)
		return 1
		;;
	esac

	return 0
}

mac_from_cmdline() {
	grep -o 'mac=[0-9a-fA-F:][0-9a-fA-F:]*' /proc/cmdline 2>/dev/null | head -1 | cut -d= -f2
}

board_resolve_mac() {
	local m
	m=$(mac_from_cmdline)
	[ -n "$m" ] || m="$FACTORY_MAC"
	[ -n "$m" ] || m=$(board_fallback_mac)
	[ -n "$m" ] && printf '%s' "$m"
}

board_fallback_mac() {
	local h
	h=$(cat /sys/class/mmc_host/mmc1/mmc1:*/cid 2>/dev/null | head -1 | tr -d ' \n')
	[ ${#h} -ge 10 ] || h=$(cat /sys/class/mmc_host/*/mmc*:*/cid 2>/dev/null | head -1 | tr -d ' \n')
	[ ${#h} -ge 10 ] || return 1
	h=$(printf '%s' "$h" | tail -c 10)
	printf '02:%s:%s:%s:%s:%s' \
		"$(printf '%s' "$h" | cut -c1-2)" "$(printf '%s' "$h" | cut -c3-4)" \
		"$(printf '%s' "$h" | cut -c5-6)" "$(printf '%s' "$h" | cut -c7-8)" \
		"$(printf '%s' "$h" | cut -c9-10)"
}
