#!/bin/sh
# 把 foo2zjs 的 sihp*.img 固件转成 CUPS 驱动要的 .dl
# 用法: mkfirmware.sh <build_dir> <dest_zjs> <dest_xqx> <arm2hpdl 可执行>
#
# 为什么要自己转:
#   1) 上游 install-extra 用的是"目标架构"的 arm2hpdl, 在构建机上根本执行不了
#      ("not found" + shell 重定向先建文件) => 打出来的 .dl 全是 0 字节;
#   2) 它的通配是 sihp1*.img, 漏掉 P1005/P1006/P1505 (文件名 sihpP*.img);
#   3) 0 字节的 .dl 会被装进 /usr/share/foo2xqx/firmware/, 真刷固件时等于发空包。
# 这里用 host 编译的同源转换工具统一生成, 两个目录都放真文件, 并清掉 0 字节残留。
set -e
B=$1
DZ=$2
DX=$3
T=$4

[ -d "$B" ] || { echo "mkfirmware: no build dir $B" >&2; exit 1; }
[ -x "$T" ] || { echo "mkfirmware: no converter $T" >&2; exit 1; }
mkdir -p "$DZ" "$DX"

n=0
for i in "$B"/sihp*.img; do
	[ -f "$i" ] || continue
	b=$(basename "$i" .img)
	"$T" "$i" > "$B/$b.dl"
	[ -s "$B/$b.dl" ] || { echo "mkfirmware: $b 转换失败(0 字节)" >&2; exit 1; }
	install -m0644 "$B/$b.dl" "$DZ/"
	install -m0644 "$B/$b.dl" "$DX/"
	echo "mkfirmware: $b.dl $(wc -c < "$B/$b.dl") bytes"
	n=$((n + 1))
done
[ "$n" -gt 0 ] || { echo "mkfirmware: 没找到任何 sihp*.img" >&2; exit 1; }

# 清掉上游留下的 0 字节 .dl
for d in "$DZ" "$DX"; do
	for f in "$d"/*.dl; do
		[ -f "$f" ] || continue
		[ -s "$f" ] || { rm -f "$f"; echo "mkfirmware: 删除空文件 $f"; }
	done
done
