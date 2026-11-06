#!/bin/sh
#
# 每块板的网络参数表 —— 唯一需要按板子维护的地方。
# 新增设备时只在这里加一个 case 分支（必要时再改 board.d/02_network 的接线方式）。
#
# 参数：
#   WAN_DEV      上联口的物理设备名
#   HAS_LAN      是否还有 LAN 桥（单口盒子为 0）
#   FACTORY_MAC  可选的 MAC 覆盖值。默认留空 —— MAC 应当由 u-boot 从这台
#                机器的 efuse/VPD 读出来、通过内核 cmdline 的 mac= 传入
#                （见 image/mesongx-autoscript.cmd 的 addmac），写死在板级
#                文件里等于全型号共用一个 MAC，是错的。
#                留空时：cmdline 的 mac= 优先，其次 CID 推导的本地地址。
#
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

# 未知板子时的兜底 MAC：用 eMMC CID 推导出稳定的 02:xx 本地地址
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
