#!/bin/sh
# 启动新命令前清理上一次没退出的 dss 进程
# 背景：命令台把输出重定向到同一个 /tmp/dss/console.out，
# 旧进程不退就会继续往里面写（表现为页面一直刷 [wait] 还剩 N 分钟）
P=/tmp/dss/console.pid
[ -f "$P" ] || exit 0
pid=$(cat "$P" 2>/dev/null)
case "$pid" in
	''|*[!0-9]*) : > "$P"; exit 0 ;;
esac
# 只处理确实是 dss 的进程：pid 可能已被系统复用给别的程序
if [ -d "/proc/$pid" ] && tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q '/usr/bin/dss '; then
	kill "$pid" 2>/dev/null
	sleep 1
	[ -d "/proc/$pid" ] && kill -9 "$pid" 2>/dev/null
fi
: > "$P"
exit 0
