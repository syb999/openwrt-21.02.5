/* net.c — 网络诊断插件（直接调用本机命令）
 *
 * 为什么调命令而不是纯系统调用：板上就有 ip/ifconfig/ping/nslookup/nc/traceroute，
 * 它们的行为和参数跟本机 OpenWrt 完全一致（多网段、VLAN、PPPoE、WireGuard
 * 这些场景自己用 getifaddrs 拼很容易漏），而且输出就是用户平时看到的样子。
 *
 * 安全：所有参数过白名单校验，命令一律用绝对路径，不走 system() 的 shell，
 * 拼进 popen 的字符串只可能由字母数字和 . : _ - 组成。
 *
 * 支持的 op：
 *   ip [iface]        接口地址表（默认全部）
 *   route             路由表
 *   dns               DNS 服务器
 *   arp               邻居表
 *   ping host [count] ICMP 连通性 + RTT
 *   port host port    TCP 端口探测（nc）
 *   resolve host      域名解析（nslookup）
 *   stat              接口收发流量
 *   link              链路状态 / 速率
 *   wifi              无线接口与信道（iw）
 *   trace host        路由跟踪（5 跳）
 *   hostname          主机名
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "plugin.h"

#define OUTMAX 16384

/* ---------------- JSON 取值 ---------------- */
static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    /* 只认「后面紧跟冒号」的匹配：字段值恰好等于某个 key 名时会误命中
     * （{"op":"query",...,"query":"x"} 找 query 会先撞上 op 的值），
     * 那样解析出来的就是隔壁字段。 */
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return -1;
    p = found;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) { p++; out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

static int jget_int(const char *js, const char *key, int dflt) {
    /* 键名带上冒号再找：否则 {"op":"port","port":22} 里
     * 会先匹配到 op 的值 "port"，然后找到 host 的冒号，取到一串垃圾 */
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return dflt;
    return atoi(p + strlen(pat));
}

/* ---------------- 参数白名单 ---------------- */
/* 接口名：br-lan、eth0.2、wlan0、pppoe-wan、wg0、@ 后面是父设备 */
static int safe_iface(const char *s) {
    if (!s || !*s || strlen(s) > 20) return 0;
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_'
            && *p != '-' && *p != '@') return 0;
    }
    return 1;
}

/* 主机：IPv4/IPv6/域名。只放行字母数字和 . : - _ */
static int safe_host(const char *s) {
    if (!s || !*s || strlen(s) > 64) return 0;
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != ':'
            && *p != '-' && *p != '_') return 0;
    }
    return 1;
}

/* ---------------- 跑命令 ---------------- */
static char *run_cmd(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char *buf = malloc(OUTMAX);
    if (!buf) { pclose(p); return NULL; }
    size_t n = fread(buf, 1, OUTMAX - 1, p);
    buf[n] = 0;
    pclose(p);
    return buf;
}

/* 输出包成 JSON：把命令输出放进 value，前端会按 markdown 渲染 */
static char *json_text(const char *text) {
    size_t cap = strlen(text) * 2 + 128;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = 0;
    i += (size_t)snprintf(o + i, cap - i, "{\"value\":\"");
    for (const char *p = text; *p && i + 12 < cap; p++) {
        switch (*p) {
        case '"':  o[i++] = '\\'; o[i++] = '"';  break;
        case '\\': o[i++] = '\\'; o[i++] = '\\'; break;
        case '\n': o[i++] = '\\'; o[i++] = 'n';  break;
        case '\r': break;
        case '\t': o[i++] = '\\'; o[i++] = 't';  break;
        default:
            if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
            else o[i++] = *p;
        }
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

/* {"value":"表格...", "answer":"一句话结论"} —— answer 给模型和框架收尾用 */
static char *json_pair(const char *text, const char *answer) {
    size_t cap = strlen(text) * 2 + strlen(answer) * 2 + 256;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = 0;
    i += (size_t)snprintf(o + i, cap - i, "{\"value\":\"");
    for (const char *p = text; *p && i + 12 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else if (*p == '\r') { }
        else if (*p == '\t') { o[i++] = ' '; }
        else if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
        else o[i++] = *p;
    }
    i += (size_t)snprintf(o + i, cap - i, "\",\"answer\":\"");
    for (const char *p = answer; *p && i + 12 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
        else o[i++] = *p;
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

static char *errf(const char *fmt, const char *a, const char *b) {
    char *o = malloc(512);
    if (!o) return NULL;
    char msg[400];
    snprintf(msg, sizeof(msg), fmt, a ? a : "", b ? b : "");
    snprintf(o, 512, "{\"error\":\"%s\"}", msg);
    return o;
}

/* ---------------- op: ip ---------------- */
/* 把 ip addr 的输出解析成 markdown 表格：接口 | 状态 | IP | MAC */
static char *op_ip(const char *iface) {
    int show_all = 0;
    char cmd[256];
    if (iface && *iface) {
        if (!safe_iface(iface)) return errf("接口名不合法：%s%s", iface, "");
        snprintf(cmd, sizeof(cmd), "/sbin/ip addr show dev '%s' 2>&1", iface);
    } else {
        /* 不能加 -4：带 -4 时 ip 不输出 link/ether 行，MAC 就拿不到 */
        snprintf(cmd, sizeof(cmd), "/sbin/ip addr show 2>&1");
    }

    char *raw = run_cmd(cmd);
    if (!raw) return errf("执行 ip 命令失败%s%s", "", "");
    if (!raw[0]) { free(raw); return errf("ip 命令没有输出%s%s", "", ""); }

    /* 解析 */
    typedef struct { char name[24]; char state[16]; char ip[48]; char mac[24]; } it_t;
    it_t items[32];
    int n = 0;
    memset(items, 0, sizeof(items));

    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line && n < 32; line = strtok_r(NULL, "\n", &save)) {
        /* "2: br-lan: <BROADCAST,...> mtu 1500 ... state UP ..." 或 "2: br-lan@eth0: ..." */
        unsigned idx;
        char nm[32] = {0};
        if (sscanf(line, "%u: %31[^:]:", &idx, nm) == 2) {
            if (strchr(nm, '@')) *strchr(nm, '@') = 0;
            snprintf(items[n].name, sizeof(items[n].name), "%s", nm);
            char *st = strstr(line, "state ");
            if (st) sscanf(st + 6, "%15s", items[n].state);
            n++;
            continue;
        }
        if (n == 0) continue;
        it_t *cur = &items[n - 1];
        char *le = strstr(line, "link/ether ");
        if (le) { sscanf(le + 11, "%23s", cur->mac); continue; }
        char *inet = strstr(line, "inet ");
        if (inet) {
            char a[48] = {0};
            sscanf(inet + 5, "%47s", a);
            if (cur->ip[0]) strncat(cur->ip, " ", sizeof(cur->ip) - strlen(cur->ip) - 1);
            strncat(cur->ip, a, sizeof(cur->ip) - strlen(cur->ip) - 1);
            continue;
        }
        char *inet6 = strstr(line, "inet6 ");
        if (inet6) {
            char a[48] = {0};
            sscanf(inet6 + 6, "%47s", a);
            if (a[0] && strncmp(a, "fe80", 4) != 0) {     /* 链路本地地址略过，太占地方 */
                if (cur->ip[0]) strncat(cur->ip, " ", sizeof(cur->ip) - strlen(cur->ip) - 1);
                strncat(cur->ip, a, sizeof(cur->ip) - strlen(cur->ip) - 1);
            }
        }
    }
    free(raw);

    if (n == 0) return json_text("（没解析出接口，原始输出：）\n```\n（空）\n```");

    char *out = malloc(OUTMAX);
    if (!out) return NULL;
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i,
        "| 接口 | 状态 | 地址 | MAC |\n|---|---|---|---|\n");
    int shown = 0;
    for (int k = 0; k < n && i < OUTMAX - 400; k++) {
        /* 没指定接口时，默认跳过 DOWN 的：
         * 板上一堆 ip6tnl0/tunl0/sit0/gre0 常年 DOWN，列出来只是噪音。
         * 想看全的传 all=1。 */
        if (!show_all && !iface[0] && !strcmp(items[k].state, "DOWN")) continue;
        i += (size_t)snprintf(out + i, OUTMAX - i, "| %s | %s | %s | %s |\n",
            items[k].name,
            items[k].state[0] ? items[k].state : "-",
            items[k].ip[0] ? items[k].ip : "-",
            items[k].mac[0] ? items[k].mac : "-");
        shown++;
    }
    if (!shown)
        i += (size_t)snprintf(out + i, OUTMAX - i,
            "| （无 UP 的接口） | - | - | - |\n");
    /* 拼一句结论：模型和框架收尾时直接用这句 */
    char ans[256] = {0};
    for (int k = 0; k < n; k++) {
        if (!show_all && !iface[0] && !strcmp(items[k].state, "DOWN")) continue;
        if (iface[0] || strlen(ans) < 150) {
            char seg[96];
            snprintf(seg, sizeof(seg), "%s%s=%s", ans[0] ? "，" : "",
                     items[k].name, items[k].ip[0] ? items[k].ip : "(无地址)");
            strncat(ans, seg, sizeof(ans) - strlen(ans) - 1);
        }
    }
    char *r = json_pair(out, ans[0] ? ans : "没有 UP 的接口");
    free(out);
    return r;
}

/* ---------------- op: route ---------------- */
static char *op_route(void) {
    char *raw = run_cmd("/sbin/ip route 2>&1");
    if (!raw) return errf("执行 ip route 失败%s%s", "", "");
    if (!raw[0]) { free(raw); return json_text("路由表为空"); }

    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i,
        "| 目标 | 网关/下一跳 | 设备 |\n|---|---|---|\n");
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line && i < OUTMAX - 300;
         line = strtok_r(NULL, "\n", &save)) {
        char dst[64] = {0}, via[64] = {0}, dev[32] = {0};
        sscanf(line, "%63s", dst);
        char *v = strstr(line, "via ");
        if (v) sscanf(v + 4, "%63s", via);
        char *d = strstr(line, "dev ");
        if (d) sscanf(d + 4, "%31s", dev);
        if (!dst[0]) continue;
        i += (size_t)snprintf(out + i, OUTMAX - i, "| %s | %s | %s |\n",
            dst, via[0] ? via : "-", dev[0] ? dev : "-");
    }
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: dns ---------------- */
static char *op_dns(void) {
    char *raw = run_cmd("cat /etc/resolv.conf 2>/dev/null");
    if (!raw) return errf("读 /etc/resolv.conf 失败%s%s", "", "");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = 0;
    int cnt = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i, "| 项 | 值 |\n|---|---|\n");
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line && i < OUTMAX - 300;
         line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "nameserver", 10)) {
            char ip[64] = {0};
            sscanf(line + 10, "%63s", ip);
            if (ip[0]) { i += (size_t)snprintf(out + i, OUTMAX - i, "| DNS 服务器 | %s |\n", ip); cnt++; }
        } else if (!strncmp(line, "search", 6) || !strncmp(line, "domain", 6)) {
            char d[128] = {0};
            sscanf(line + 6, "%127s", d);
            if (d[0]) i += (size_t)snprintf(out + i, OUTMAX - i, "| 搜索域 | %s |\n", d);
        }
    }
    if (!cnt && i < OUTMAX - 200)
        i += (size_t)snprintf(out + i, OUTMAX - i, "| （无） | /etc/resolv.conf 里没有 nameserver |\n");
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: arp ---------------- */
static char *op_arp(void) {
    char *raw = run_cmd("/sbin/ip neigh 2>&1");
    if (!raw) return errf("执行 ip neigh 失败%s%s", "", "");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i, "| 邻居 | MAC | 设备 | 状态 |\n|---|---|---|---|\n");
    char *save = NULL;
    int cnt = 0;
    for (char *line = strtok_r(raw, "\n", &save); line && i < OUTMAX - 300 && cnt < 40;
         line = strtok_r(NULL, "\n", &save)) {
        char ip[64] = {0}, dev[32] = {0}, st[24] = {0}, mac[32] = {0};
        sscanf(line, "%63s", ip);
        char *d = strstr(line, "dev ");
        if (d) sscanf(d + 4, "%31s", dev);
        char *m = strstr(line, "lladdr ");
        if (m) sscanf(m + 7, "%31s", mac);
        /* 状态：最后一段 */
        char *last = strrchr(line, ' ');
        if (last) sscanf(last + 1, "%23s", st);
        if (!ip[0]) continue;
        i += (size_t)snprintf(out + i, OUTMAX - i, "| %s | %s | %s | %s |\n",
            ip, mac[0] ? mac : "-", dev[0] ? dev : "-", st[0] ? st : "-");
        cnt++;
    }
    if (!cnt) i += (size_t)snprintf(out + i, OUTMAX - i, "| （空） | 邻居表没有条目 | - | - |\n");
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: ping ---------------- */
static char *op_ping(const char *host, int count) {
    if (!safe_host(host)) return errf("主机名不合法：%s%s", host, "");
    if (count <= 0) count = 3;
    if (count > 10) count = 10;

    /* 有冒号当 IPv6 */
    const char *bin = strchr(host, ':') ? "/bin/ping6" : "/bin/ping";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s -c %d -W 2 '%s' 2>&1", bin, count, host);
    char *raw = run_cmd(cmd);
    if (!raw) return errf("执行 ping 失败%s%s", "", "");

    /* 从 summary 里抠丢包和 RTT */
    char loss[32] = "?", rtt[48] = "?";
    char *pc = strstr(raw, "% packet loss");
    if (pc) {
        /* 从 % 往回退到数字开头才是 "0%"，停在空格上会读到 "packet" */
        char *q = pc;
        while (q > raw && (isdigit((unsigned char)q[-1]) || q[-1] == '.')) q--;
        snprintf(loss, sizeof(loss), "%.*s%%", (int)(pc - q), q);
    }
    char *r1 = strstr(raw, "min/avg/max");
    if (r1) {
        char *eq = strstr(r1, "= ");
        if (eq) sscanf(eq + 2, "%47s", rtt);
    }
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    int ok = (strstr(raw, " 0% packet loss") != NULL);
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i,
        "%s `%s`（%d 次）\n\n| 项 | 值 |\n|---|---|\n| 结果 | %s |\n| 丢包 | %s |\n| RTT | %s |\n",
        ok ? "**通**" : "**不通**", host, count,
        ok ? "可达" : "不可达", loss, rtt);
    free(raw);
    char ans[200];
    snprintf(ans, sizeof(ans), "%s %s（丢包 %s，RTT %s）", host, ok ? "可达" : "不可达", loss, rtt);
    char *r = json_pair(out, ans);
    free(out);
    return r;
}

/* ---------------- op: port ---------------- */
static char *op_port(const char *host, int port) {
    if (!safe_host(host)) return errf("主机名不合法：%s%s", host, "");
    if (port <= 0 || port > 65535) return errf("端口不合法：%s%s", "", "");
    char cmd[256];
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/nc -z -w 3 '%s' %s >/dev/null 2>&1 && echo OPEN || echo CLOSED", host, ps);
    char *raw = run_cmd(cmd);
    int rc = raw ? 0 : 1;
    (void)rc;
    /* nc 通了返回 0，关了返回非 0；popen 拿不到退出码，用输出判断：
     * 通了一般没输出，不通会打印 "nc: ... Connection refused" */
    int open = (raw && strstr(raw, "OPEN")) ? 1 : 0;
    char *out = malloc(2048);
    if (!out) { free(raw); return NULL; }
    snprintf(out, 2048,
        "`%s:%d` → **%s**\n\n| 项 | 值 |\n|---|---|\n| 端口 | %d |\n| 状态 | %s |\n%s",
        host, port, open ? "开放/可达" : "关闭/不可达", port,
        open ? "开放" : "关闭",
        (raw && raw[0]) ? "" : "");
    free(raw);
    char ans[160];
    snprintf(ans, sizeof(ans), "%s:%d %s", host, port, open ? "开放/可达" : "关闭/不可达");
    char *r = json_pair(out, ans);
    free(out);
    return r;
}

/* ---------------- op: resolve ---------------- */
static char *op_resolve(const char *host) {
    if (!safe_host(host)) return errf("域名不合法：%s%s", host, "");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/usr/bin/nslookup '%s' 2>&1", host);
    char *raw = run_cmd(cmd);
    if (!raw) return errf("执行 nslookup 失败%s%s", "", "");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i, "解析 `%s`：\n\n```\n", host);
    /* 只留关键行，避免整段 nslookup 输出太长 */
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line && i < OUTMAX - 400;
         line = strtok_r(NULL, "\n", &save)) {
        if (strstr(line, "Name:") || strstr(line, "Address") || strstr(line, "can't find")
            || strstr(line, "NXDOMAIN") || strstr(line, "answer")) {
            i += (size_t)snprintf(out + i, OUTMAX - i, "%s\n", line);
        }
    }
    i += (size_t)snprintf(out + i, OUTMAX - i, "```");
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: stat ---------------- */
static char *op_stat(void) {
    char *raw = run_cmd("cat /proc/net/dev 2>&1");
    if (!raw) return errf("读 /proc/net/dev 失败%s%s", "", "");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i,
        "| 接口 | 接收 | 发送 |\n|---|---|---|\n");
    char *save = NULL;
    int cnt = 0;
    for (char *line = strtok_r(raw, "\n", &save); line && i < OUTMAX - 300;
         line = strtok_r(NULL, "\n", &save)) {
        char ifc[32] = {0};
        unsigned long long rb = 0, rp = 0, tb = 0, tp = 0;
        if (sscanf(line, " %31[^:]: %llu %llu %*u %*u %*u %*u %*u %*u %llu %llu",
                   ifc, &rb, &rp, &tb, &tp) >= 4) {
            i += (size_t)snprintf(out + i, OUTMAX - i,
                "| %s | %.1f MB / %llu 包 | %.1f MB / %llu 包 |\n",
                ifc, rb / 1048576.0, rp, tb / 1048576.0, tp);
            cnt++;
        }
    }
    if (!cnt) i += (size_t)snprintf(out + i, OUTMAX - i, "| - | 无数据 | - |\n");
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: link ---------------- */
static char *op_link(void) {
    char *raw = run_cmd("/sbin/ip -s link 2>&1");
    if (!raw) return errf("执行 ip -s link 失败%s%s", "", "");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i, "```\n");
    size_t n = strlen(raw);
    if (n > OUTMAX - 200) n = OUTMAX - 200;
    memcpy(out + i, raw, n);
    i += n;
    i += (size_t)snprintf(out + i, OUTMAX - i, "\n```");
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: wifi ---------------- */
static char *op_wifi(void) {
    char *raw = run_cmd("/usr/sbin/iw dev 2>&1");
    if (!raw) return errf("执行 iw 失败%s%s", "", "");
    if (!raw[0] || strstr(raw, "not found")) {
        free(raw);
        return json_text("这台设备没有无线接口（iw dev 无输出）。");
    }
    /* 再补一段信号质量 */
    char *surv = run_cmd("/usr/sbin/iw dev 2>/dev/null | "
                         "awk '/Interface/{ifc=$2} /ssid/{s=$2} /channel/{c=$2} "
                         "END{}' 2>/dev/null");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); free(surv); return NULL; }
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i, "```\n%s\n```", raw);
    if (surv && surv[0] && i < OUTMAX - 500)
        i += (size_t)snprintf(out + i, OUTMAX - i, "\n\n```\n%s\n```", surv);
    free(raw);
    free(surv);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: trace ---------------- */
static char *op_trace(const char *host) {
    if (!safe_host(host)) return errf("主机名不合法：%s%s", host, "");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/bin/traceroute -m 5 -w 2 -q 1 '%s' 2>&1", host);
    char *raw = run_cmd(cmd);
    if (!raw) return errf("执行 traceroute 失败%s%s", "", "");
    char *out = malloc(OUTMAX);
    if (!out) { free(raw); return NULL; }
    size_t i = (size_t)snprintf(out, OUTMAX, "```\n%s\n```", raw);
    (void)i;
    free(raw);
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- op: hostname ---------------- */
static char *op_hostname(void) {
    char *raw = run_cmd("cat /proc/sys/kernel/hostname 2>&1");
    char *out = malloc(1024);
    if (!out) { free(raw); return NULL; }
    if (raw) {
        char h[128] = {0};
        sscanf(raw, "%127s", h);
        snprintf(out, 1024, "主机名：`%s`", h);
        free(raw);
    } else {
        snprintf(out, 1024, "读主机名失败");
    }
    char *r = json_text(out);
    free(out);
    return r;
}

/* ---------------- 入口 ---------------- */
static char *run(const char *json_in) {
    char op[32] = {0}, host[80] = {0}, iface[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    jget(json_in, "host", host, sizeof(host));
    jget(json_in, "iface", iface, sizeof(iface));
    if (!host[0]) jget(json_in, "target", host, sizeof(host));
    int port = jget_int(json_in, "port", 0);
    int count = jget_int(json_in, "count", 3);

    if (!op[0]) {
        /* 没给 op 就按参数猜 */
        if (port > 0 && host[0])      snprintf(op, sizeof(op), "port");
        else if (host[0])             snprintf(op, sizeof(op), "ping");
        else if (iface[0])            snprintf(op, sizeof(op), "ip");
        else                          snprintf(op, sizeof(op), "ip");
    }

    if (!strcmp(op, "ip") || !strcmp(op, "addr") || !strcmp(op, "ifconfig")) {
        char *r = op_ip(iface);
        return r;
    }
    if (!strcmp(op, "route") || !strcmp(op, "gw"))    return op_route();
    if (!strcmp(op, "dns"))                            return op_dns();
    if (!strcmp(op, "arp") || !strcmp(op, "neigh"))    return op_arp();
    if (!strcmp(op, "ping"))                           return op_ping(host, count);
    if (!strcmp(op, "port") || !strcmp(op, "tcp"))     return op_port(host, port);
    if (!strcmp(op, "resolve") || !strcmp(op, "dnsq")) return op_resolve(host);
    if (!strcmp(op, "stat") || !strcmp(op, "traffic")) return op_stat();
    if (!strcmp(op, "link"))                           return op_link();
    if (!strcmp(op, "wifi"))                           return op_wifi();
    if (!strcmp(op, "trace") || !strcmp(op, "traceroute")) return op_trace(host);
    if (!strcmp(op, "hostname"))                       return op_hostname();

    return errf("不支持的 op：%s（可用 ip/route/dns/arp/ping/port/resolve/stat/link/wifi/trace/hostname）%s",
                op, "");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "net",
    "网络诊断：ip route dns arp ping port resolve stat link wifi trace hostname（调本机命令，参数白名单）"
    "连通性(ping) 端口(port) 域名解析(resolve) 流量(stat) 链路(link) 无线(wifi) 路由跟踪(trace)",
    "{\"op\":\"ip\"}"
    "  或 {\"op\":\"ip\",\"iface\":\"br-lan\"}"
    "  或 {\"op\":\"route\"}"
    "  或 {\"op\":\"dns\"}"
    "  或 {\"op\":\"ping\",\"host\":\"223.5.5.5\"}"
    "  或 {\"op\":\"port\",\"host\":\"10.80.1.1\",\"port\":80}"
    "  或 {\"op\":\"resolve\",\"host\":\"www.baidu.com\"}"
    "  或 {\"op\":\"stat\"}"
    "  或 {\"op\":\"wifi\"}"
    "  或 {\"op\":\"trace\",\"host\":\"223.5.5.5\"}",
    run,
    "ifconfig,address,addr,route,gateway,dns,arp,ping,port,resolve,nslookup,traffic,link,wifi,trace"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
