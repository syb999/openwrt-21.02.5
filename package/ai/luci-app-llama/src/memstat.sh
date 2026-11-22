#!/bin/sh
# memstat.sh — 扫任务日志里的 [mem] 行，汇总分配计数
#
# 用法：memstat.sh [日志目录]
#
# 判读方法：
#   ① 同一 where 点的 alloc 平均值应稳定（同类任务分配量一致）
#   ② alloc 的最大值不应随时间持续抬高
#   ③ live 应稳定在一个小数字（不是每步递增）
#
# 注意 live 会偏负/偏小：插件是独立 .so，它内部 malloc 不记账，
# 但它返回的字符串由框架 free → 记一次 free 没有对应 alloc。

D=${1:-/tmp/agent-jobs}

awk '
/\[mem\]/ {
    where = $2
    alloc = free = live = rss = -1
    for (i = 3; i <= NF; i++) {
        if ($i ~ /^alloc=/)   { split($i,a,"="); alloc = a[2] }
        if ($i ~ /^free=/)    { split($i,a,"="); free  = a[2] }
        if ($i ~ /^live=/)    { split($i,a,"="); live  = a[2] }
        if ($i ~ /^rss=/)     { split($i,a,"="); gsub(/KB/,"",a[2]); rss = a[2] }
    }
    if (alloc < 0) next
    n[where]++
    sa[where] += alloc
    sl[where] += live
    sr[where] += rss
    if (alloc > ma[where] || n[where] == 1) ma[where] = alloc
    if (live  > ml[where] || n[where] == 1) ml[where] = live
    if (rss   > mr[where] || n[where] == 1) mr[where] = rss
}
END {
    printf "%-14s %7s %11s %11s %10s %10s %10s\n",
           "打点", "次数", "alloc均值", "alloc最大", "live均值", "live最大", "rss最大KB"
    printf "%-14s %7s %11s %11s %10s %10s %10s\n",
           "--------------", "-------", "-----------", "-----------", "----------", "----------", "----------"
    total = 0
    for (w in n) {
        printf "%-14s %7d %11.1f %11d %10.1f %10d %10d\n",
               w, n[w], sa[w]/n[w], ma[w], sl[w]/n[w], ml[w], mr[w]
        total += n[w]
    }
    if (total == 0) printf "（日志里没有 [mem] 行：任务用 AGENT_MEMCHECK=0 跑的，或版本较旧）\n"
}
' "$D"/*/*.log 2>/dev/null
