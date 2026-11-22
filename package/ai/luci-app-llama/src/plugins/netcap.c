/* netcap.c — 抓包分析插件（tcpdump + 自己解析 pcap）
 *
 * 为什么自己做 pcap 解析而不是调 tshark：板上没有 tshark（装不上，依赖太多），
 * 而 tcpdump 只能把包写成二进制或打印十六进制。DNS 查询名和 TLS SNI 都在
 * 明文里，自己按协议字段抠出来最省事，也不用装任何额外东西。
 *
 * 支持的 op：
 *   ifaces                有哪些网卡、各自 IP（抓之前先看这个）
 *   capture iface secs    抓包落成 pcap（默认 br-lan / 10 秒 / 5000 包）
 *   dns    [file]         DNS 查询过的域名（含次数）
 *   sni    [file]         TLS 握手里的域名（SNI）+ 目标 IP —— HTTPS 也能看出访问了谁
 *   http   [file]         明文 HTTP 的请求行 + Host（完整 URL 路径）
 *   top    [file]         流量排行（按对端 IP 统计字节）
 *   urls   [file]         dns + sni + http 合并成一张"访问了哪些域名/网址"的清单
 *
 * 明确做不到的：HTTPS 的具体路径（/a/b?q=1）。TLS 里那部分是加密的，
 * 没有会话密钥谁都解不开 —— 抓到也只能看到域名。这一条写进 urls 的输出里，
 * 免得用户以为插件坏了。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <strings.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "plugin.h"

#define OUTMAX   24576
#define WORK_DIR "/tmp/llama_agent"
#define MAXPCAP  (48L * 1024 * 1024)
#define MAXENT   160

/* ---------------- JSON 取值 ---------------- */
static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return -1;
    p = found + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            default: out[i++] = *p;
            }
        } else out[i++] = *p;
        p++;
    }
    out[i] = 0;
    return 0;
}

/* 数字取值：jget 只认带引号的字符串，`"duration":20` 这种裸数字会被它
 * 判成"没有这个字段"，于是全走默认值（实测截 20 秒变成 10 秒）。
 * 这里单独实现，字符串和裸数字都认。 */
static long jget_int(const char *js, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return dflt;
    p = found + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return dflt;
    return v;
}

static double jget_num(const char *js, const char *key, double dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return dflt;
    p = found + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return dflt;
    return v;
}

/* ---------------- 结果构造 ---------------- */
static char *json_text(const char *text) {
    size_t cap = strlen(text) * 6 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, cap, "{\"value\":\"");
    for (const char *p = text; *p && i + 8 < cap; p++) {
        switch (*p) {
        case '"':  o[i++] = '\\'; o[i++] = '"';  break;
        case '\\': o[i++] = '\\'; o[i++] = '\\'; break;
        case '\n': o[i++] = '\\'; o[i++] = 'n';  break;
        case '\r': o[i++] = '\\'; o[i++] = 'r';  break;
        case '\t': o[i++] = '\\'; o[i++] = 't';  break;
        default:   o[i++] = *p;
        }
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

static char *json_pair(const char *text, const char *answer) {
    size_t cap = strlen(text) * 6 + strlen(answer) * 6 + 128;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, cap, "{\"value\":\"");
    for (const char *p = text; *p && i + 8 < cap; p++) {
        switch (*p) {
        case '"':  o[i++] = '\\'; o[i++] = '"';  break;
        case '\\': o[i++] = '\\'; o[i++] = '\\'; break;
        case '\n': o[i++] = '\\'; o[i++] = 'n';  break;
        case '\r': o[i++] = '\\'; o[i++] = 'r';  break;
        case '\t': o[i++] = '\\'; o[i++] = 't';  break;
        default:   o[i++] = *p;
        }
    }
    i += (size_t)snprintf(o + i, cap - i, "\",\"answer\":\"");
    for (const char *p = answer; *p && i + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

static char *errf(const char *fmt, const char *a, const char *b) {
    char m[700];
    snprintf(m, sizeof(m), fmt, a ? a : "", b ? b : "");
    char *o = malloc(strlen(m) * 6 + 64);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, strlen(m) * 6 + 64, "{\"error\":\"");
    for (const char *p = m; *p; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    snprintf(o + i, strlen(m) * 6 + 64 - i, "\"}");
    return o;
}

static void resolve_path(const char *in, char *out, size_t cap) {
    if (!in || !in[0]) { snprintf(out, cap, "%s", WORK_DIR); return; }
    if (in[0] == '/') { snprintf(out, cap, "%s", in); return; }
    snprintf(out, cap, "%s/%s", WORK_DIR, in);
}

/* 抓包参数白名单：BPF 表达式只允许这些字符，杜绝 shell 注入 */
static int filter_ok(const char *f) {
    for (const char *p = f; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == ' ' || c == '.' || c == ':' || c == '/' ||
            c == '-' || c == '_' || c == '(' || c == ')' || c == '[' ||
            c == ']' || c == '!' || c == '=' || c == ',' || c == '<' || c == '>')
            continue;
        return 0;
    }
    return 1;
}

static int iface_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 15) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_')
            return 0;
    return 1;
}

/* 默认抓哪块网卡：跟着默认路由走。
 * 固定 br-lan 是错的 —— 板上自己访问外网走的是 eth0，抓 br-lan 一个包都没有，
 * 模型看到空结果会反复重试（实测重试 3 次、白跑 6 分钟）。 */
static void default_iface(char *out, size_t cap) {
    snprintf(out, cap, "br-lan");
    FILE *pp = popen("/sbin/ip route show default 2>/dev/null", "r");
    if (!pp) return;
    char line[256];
    while (fgets(line, sizeof(line), pp)) {
        char *d = strstr(line, "dev ");
        if (!d) continue;
        d += 4;
        size_t i = 0;
        while (d[i] && d[i] != ' ' && d[i] != '\n' && i + 1 < cap) {
            out[i] = d[i];
            i++;
        }
        if (i) { out[i] = 0; break; }
    }
    pclose(pp);
}

/* ---------------- 抓包 ---------------- */
static int run_capture(const char *iface, const char *filter, int secs, int pkts,
                       const char *outp, char *err, size_t errcap) {
    pid_t pid = fork();
    if (pid < 0) { snprintf(err, errcap, "fork 失败"); return -1; }
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        char cnt[16], sp[16];
        snprintf(cnt, sizeof(cnt), "%d", pkts);
        snprintf(sp, sizeof(sp), "%d", secs * 2 + 10);
        /* -s 0 保留完整包（SNI/HTTP 都在负载里，截断就解析不出来）
         * 不加 -p：br-lan 是网桥，要看其他设备的流量必须开混杂模式 */
        char *argv[16];
        int n = 0;
        argv[n++] = (char *)"tcpdump";
        argv[n++] = (char *)"-i"; argv[n++] = (char *)iface;
        argv[n++] = (char *)"-n";
        argv[n++] = (char *)"-s"; argv[n++] = (char *)"0";
        argv[n++] = (char *)"-c"; argv[n++] = cnt;
        argv[n++] = (char *)"-w"; argv[n++] = (char *)outp;
        if (filter && filter[0]) argv[n++] = (char *)filter;
        argv[n] = NULL;
        execv("/usr/sbin/tcpdump", argv);
        execv("/usr/bin/tcpdump", argv);
        _exit(127);
    }
    /* 到点主动停：tcpdump 收到 SIGTERM 会把缓冲刷进文件 */
    int waited = 0;
    while (waited < secs * 10) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return 0;               /* -c 到数自己退了 */
        usleep(100000);
        waited++;
    }
    kill(pid, SIGTERM);
    int st = 0;
    waitpid(pid, &st, 0);
    return 0;
}

/* ---------------- 解析状态 ---------------- */
typedef struct { char name[160]; int cnt; } item_t;
static item_t g_dns[MAXENT];  static int g_ndns;
static item_t g_sni[MAXENT];  static int g_nsni;
static char   g_http[80][320]; static int g_nhttp;
typedef struct { char ip[48]; unsigned long long bytes; } peer_t;
static peer_t g_peer[MAXENT]; static int g_npeer;
static unsigned long long g_pkts, g_bytes;

static void reset_state(void) {
    g_ndns = g_nsni = g_nhttp = g_npeer = 0;
    g_pkts = g_bytes = 0;
}

static void add_item(item_t *arr, int *n, const char *name) {
    if (!name || !name[0]) return;
    for (int i = 0; i < *n; i++)
        if (!strcmp(arr[i].name, name)) { arr[i].cnt++; return; }
    if (*n >= MAXENT) return;
    snprintf(arr[*n].name, sizeof(arr[*n].name), "%s", name);
    arr[*n].cnt = 1;
    (*n)++;
}

static void add_peer(const unsigned char *ip, unsigned long long bytes) {
    char s[48];
    snprintf(s, sizeof(s), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    for (int i = 0; i < g_npeer; i++)
        if (!strcmp(g_peer[i].ip, s)) { g_peer[i].bytes += bytes; return; }
    if (g_npeer >= MAXENT) return;
    snprintf(g_peer[g_npeer].ip, sizeof(g_peer[0].ip), "%s", s);
    g_peer[g_npeer].bytes = bytes;
    g_npeer++;
}

/* ---------------- 协议解析 ---------------- */
static int parse_dns_name(const unsigned char *p, size_t len, char *out, size_t cap) {
    if (len < 13) return 0;
    int qd = (p[4] << 8) | p[5];
    if (qd < 1) return 0;
    size_t i = 12, o = 0;
    while (i < len) {
        unsigned l = p[i++];
        if (l == 0) break;
        if (l > 63 || i + l > len) return 0;
        if (o + l + 2 >= cap) return 0;
        if (o) out[o++] = '.';
        memcpy(out + o, p + i, l);
        o += l;
        i += l;
    }
    out[o] = 0;
    return o > 0;
}

/* TLS ClientHello 里的 server_name 扩展 */
static int parse_tls_sni(const unsigned char *p, size_t len, char *out, size_t cap) {
    if (len < 6 || p[0] != 0x16 || p[1] != 0x03) return 0;
    size_t i = 5;                                  /* 跳过 record 头 */
    if (i + 4 > len || p[i] != 0x01) return 0;     /* ClientHello */
    i += 4;
    if (i + 34 > len) return 0;
    i += 34;                                       /* version + random */
    if (i >= len) return 0;
    i += 1 + p[i];                                 /* session id */
    if (i + 2 > len) return 0;
    i += 2 + (size_t)((p[i] << 8) | p[i + 1]);     /* cipher suites */
    if (i >= len) return 0;
    i += 1 + p[i];                                 /* compression */
    if (i + 2 > len) return 0;
    size_t ext_end = i + 2 + (size_t)((p[i] << 8) | p[i + 1]);
    i += 2;
    if (ext_end > len) ext_end = len;
    while (i + 4 <= ext_end) {
        unsigned type = (unsigned)((p[i] << 8) | p[i + 1]);
        unsigned elen = (unsigned)((p[i + 2] << 8) | p[i + 3]);
        i += 4;
        if (i + elen > ext_end) break;
        if (type == 0 && elen >= 5) {
            unsigned nlen = (unsigned)((p[i + 3] << 8) | p[i + 4]);
            if (nlen > 0 && nlen < cap && i + 5 + nlen <= ext_end) {
                memcpy(out, p + i + 5, nlen);
                out[nlen] = 0;
                return 1;
            }
        }
        i += elen;
    }
    return 0;
}

static void parse_http_req(const unsigned char *p, size_t len) {
    static const char *M[] = {"GET ", "POST ", "HEAD ", "PUT ", "DELETE ",
                              "CONNECT ", "OPTIONS ", NULL};
    int ok = 0;
    for (int i = 0; M[i]; i++)
        if (len > strlen(M[i]) && !strncmp((const char *)p, M[i], strlen(M[i]))) { ok = 1; break; }
    if (!ok) return;

    char line[220];
    size_t n = 0;
    while (n < len && n < sizeof(line) - 1 && p[n] != '\r' && p[n] != '\n') {
        line[n] = (char)p[n];
        n++;
    }
    line[n] = 0;

    char host[140] = {0};
    size_t scan = len < 4000 ? len : 4000;
    for (size_t k = 0; k + 7 < scan; k++) {
        if (!strncasecmp((const char *)p + k, "\r\nhost:", 7)) {
            size_t j = k + 7;
            while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
            size_t h = 0;
            while (j < len && p[j] != '\r' && p[j] != '\n' && h < sizeof(host) - 1)
                host[h++] = (char)p[j++];
            host[h] = 0;
            break;
        }
    }
    if (g_nhttp < 80) {
        /* 方法 + 路径（来自请求行） */
        char meth[16] = {0};
        size_t ml = 0;
        while (line[ml] && line[ml] != ' ' && ml < sizeof(meth) - 1) { meth[ml] = line[ml]; ml++; }
        meth[ml] = 0;
        const char *path = strchr(line, ' ');
        if (path) { path++; while (*path == ' ') path++; } else path = "/";
        char pbuf[200];
        snprintf(pbuf, sizeof(pbuf), "%s", path);
        char *hv = strstr(pbuf, " HTTP/");
        if (hv) *hv = 0;

        if (host[0]) {
            /* Host 可能自带端口，路径可能已是绝对 URL（代理请求） */
            if (!strncmp(pbuf, "http://", 7) || !strncmp(pbuf, "https://", 8))
                snprintf(g_http[g_nhttp], sizeof(g_http[0]), "%s %s", meth, pbuf);
            else
                snprintf(g_http[g_nhttp], sizeof(g_http[0]), "%s http://%s%s",
                         meth, host, pbuf);
        } else {
            snprintf(g_http[g_nhttp], sizeof(g_http[0]), "%s %s", meth, pbuf);
        }
        g_nhttp++;
    }
}

static unsigned rd32(const unsigned char *p, int le) {
    if (le) return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}
static unsigned rd16(const unsigned char *p, int le) {
    if (le) return (unsigned)p[0] | ((unsigned)p[1] << 8);
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static void handle_pkt(const unsigned char *p, size_t len, unsigned linktype,
                       unsigned long long orig_len) {
    size_t off = 0;
    unsigned eth = 0;

    if (linktype == 1) {                     /* Ethernet */
        if (len < 14) return;
        eth = (unsigned)((p[12] << 8) | p[13]);
        off = 14;
        while ((eth == 0x8100 || eth == 0x88a8) && off + 4 <= len) {
            eth = (unsigned)((p[off + 2] << 8) | p[off + 3]);
            off += 4;
        }
    } else if (linktype == 113) {            /* Linux cooked */
        if (len < 16) return;
        eth = (unsigned)rd16(p + 14, 0);
        off = 16;
    } else if (linktype == 101) {            /* raw IP */
        eth = 0x0800;
        off = 0;
    } else {
        return;                              /* 其它链路层不支持 */
    }
    if (eth != 0x0800) return;               /* 只看 IPv4，IPv6 暂时略过 */

    if (off + 20 > len) return;
    unsigned ihl = (unsigned)((p[off] & 0x0f) * 4);
    if (ihl < 20 || off + ihl > len) return;
    unsigned proto = p[off + 9];
    const unsigned char *dst = p + off + 16;

    g_pkts++;
    g_bytes += orig_len;

    if (proto == 6) {                        /* TCP */
        if (off + ihl + 20 > len) return;
        unsigned doff = (unsigned)((p[off + ihl + 12] >> 4) * 4);
        if (doff < 20 || off + ihl + doff > len) return;
        const unsigned char *pl = p + off + ihl + doff;
        size_t plen = len - (off + ihl + doff);
        unsigned dport = (unsigned)((p[off + ihl + 2] << 8) | p[off + ihl + 3]);
        unsigned sport = (unsigned)((p[off + ihl] << 8) | p[off + ihl + 1]);

        add_peer(dst, orig_len);

        if (plen > 5 && (dport == 443 || sport == 443 || dport == 8443)) {
            char host[160];
            if (parse_tls_sni(pl, plen, host, sizeof(host)))
                add_item(g_sni, &g_nsni, host);
        }
        if (plen > 4 && (dport == 80 || sport == 80 || dport == 8080 || dport == 8000))
            parse_http_req(pl, plen);
    } else if (proto == 17) {                /* UDP */
        if (off + ihl + 8 > len) return;
        unsigned dport = (unsigned)((p[off + ihl + 2] << 8) | p[off + ihl + 3]);
        unsigned sport = (unsigned)((p[off + ihl] << 8) | p[off + ihl + 1]);
        const unsigned char *pl = p + off + ihl + 8;
        size_t plen = len - (off + ihl + 8);
        add_peer(dst, orig_len);
        if ((dport == 53 || sport == 53) && plen > 13) {
            char name[160];
            if (parse_dns_name(pl, plen, name, sizeof(name)))
                add_item(g_dns, &g_ndns, name);
        }
    }
}

static int parse_pcap(const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "打不开 %s", path); return -1; }
    unsigned char gh[24];
    if (fread(gh, 1, 24, f) != 24) { fclose(f); snprintf(err, errcap, "文件太短，不是 pcap"); return -1; }

    int le;
    if (gh[0] == 0xd4 && gh[1] == 0xc3 && gh[2] == 0xb2 && gh[3] == 0xa1) le = 1;
    else if (gh[0] == 0xa1 && gh[1] == 0xb2 && gh[2] == 0xc3 && gh[3] == 0xd4) le = 0;
    else if (gh[0] == 0x4d && gh[1] == 0x3c && gh[2] == 0xb2 && gh[3] == 0xa1) le = 1;
    else { fclose(f); snprintf(err, errcap, "不认识的 pcap 格式（magic 不对）"); return -1; }

    unsigned linktype = rd32(gh + 20, le) & 0xffff;
    unsigned char rh[16];
    long total = 0;
    while (fread(rh, 1, 16, f) == 16) {
        unsigned incl = rd32(rh + 8, le);
        unsigned orig = rd32(rh + 12, le);
        if (incl == 0 || incl > 262144) break;
        if (total > MAXPCAP) break;
        unsigned char *pkt = malloc(incl);
        if (!pkt) break;
        if (fread(pkt, 1, incl, f) != incl) { free(pkt); break; }
        total += (long)incl;
        handle_pkt(pkt, incl, linktype, orig);
        free(pkt);
    }
    fclose(f);
    return 0;
}

/* ---------------- op 实现 ---------------- */
static char *op_ifaces(void) {
    char text[4000];
    size_t n = (size_t)snprintf(text, sizeof(text),
        "| 网卡 | 状态 | IPv4 |\n|---|---|---|\n");
    /* 从 /sys/class/net 拿名字，地址走 `ip -o -4 addr` */
    FILE *pp = popen("/sbin/ip -o -4 addr show 2>/dev/null", "r");
    char line[512];
    int cnt = 0;
    while (pp && fgets(line, sizeof(line), pp) && n < sizeof(text) - 200) {
        /* 2: br-lan    inet 192.168.28.1/24 brd ... scope global br-lan */
        char name[32] = {0}, addr[64] = {0};
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        c1++;
        while (*c1 == ' ') c1++;
        char *sp = strchr(c1, ' ');
        if (!sp) continue;
        size_t L = (size_t)(sp - c1);
        if (L >= sizeof(name)) continue;
        memcpy(name, c1, L); name[L] = 0;
        char *in = strstr(line, "inet ");
        if (in) {
            in += 5;
            char *e = strchr(in, ' ');
            if (e) { size_t a = (size_t)(e - in); if (a >= sizeof(addr)) a = sizeof(addr) - 1; memcpy(addr, in, a); addr[a] = 0; }
        }
        char st[24] = "-";
        char sp2[64];
        snprintf(sp2, sizeof(sp2), "/sys/class/net/%s/operstate", name);
        FILE *fs = fopen(sp2, "r");
        if (fs) { if (fgets(st, sizeof(st), fs)) { char *nl = strchr(st, '\n'); if (nl) *nl = 0; } fclose(fs); }
        n += (size_t)snprintf(text + n, sizeof(text) - n, "| %s | %s | %s |\n", name, st, addr[0] ? addr : "-");
        cnt++;
    }
    if (pp) pclose(pp);
    if (!cnt) return json_text("没读到网卡（ip 命令不可用）");
    n += (size_t)snprintf(text + n, sizeof(text) - n,
        "\n抓包默认在 `br-lan` 上做（网桥，能看到所有 LAN 设备的流量）。");
    return json_text(text);
}

/* 通用：拿 pcap（没给就现抓） */
static int ensure_pcap(const char *json, const char *iface, int secs, int pkts,
                       char *outfile, size_t cap, char *err, size_t errcap, int *captured) {
    char pf[512] = {0}, filter[256] = {0};
    jget(json, "file", pf, sizeof(pf));
    jget(json, "path", pf, sizeof(pf));
    jget(json, "filter", filter, sizeof(filter));

    *captured = 0;
    if (pf[0]) { resolve_path(pf, outfile, cap); return 0; }

    if (!iface_ok(iface)) { snprintf(err, errcap, "网卡名不合法：%s", iface); return -1; }
    if (filter[0] && !filter_ok(filter)) { snprintf(err, errcap, "filter 里有不允许的字符"); return -1; }
    if (secs <= 0) secs = 10;
    if (secs > 60) secs = 60;
    if (pkts <= 0) pkts = 5000;

    mkdir(WORK_DIR, 0777);
    snprintf(outfile, cap, "%s/cap_%ld.pcap", WORK_DIR, (long)time(NULL));
    if (run_capture(iface, filter, secs, pkts, outfile, err, errcap) != 0) return -1;
    *captured = 1;
    return 0;
}

static char *op_capture(const char *json) {
    char iface[32] = {0}, filter[256] = {0}, outp[512];
    jget(json, "iface", iface, sizeof(iface));
    if (!iface[0]) default_iface(iface, sizeof(iface));
    jget(json, "filter", filter, sizeof(filter));
    int secs = (int)jget_int(json, "secs", 10);
    int pkts = (int)jget_int(json, "packets", 5000);
    if (!iface_ok(iface)) return errf("网卡名不合法：%s %s", iface, "");
    if (filter[0] && !filter_ok(filter)) return errf("filter 里有不允许的字符：%s %s", filter, "");
    if (secs <= 0) secs = 10;
    if (secs > 60) secs = 60;
    if (pkts < 10) pkts = 10;
    if (pkts > 50000) pkts = 50000;

    mkdir(WORK_DIR, 0777);
    snprintf(outp, sizeof(outp), "%s/cap_%ld.pcap", WORK_DIR, (long)time(NULL));
    char err[256];
    if (run_capture(iface, filter, secs, pkts, outp, err, sizeof(err)) != 0)
        return errf("%s", err, "");

    struct stat st;
    if (stat(outp, &st) != 0) return errf("tcpdump 没产出文件（可能是权限或网卡名不对）%s %s", "", "");
    char text[1200], ans[220];
    snprintf(text, sizeof(text),
        "**抓包完成**（%s，%d 秒，最多 %d 包%s%s）\n\n"
        "| 项 | 值 |\n|---|---|\n| 文件 | `%s` |\n| 大小 | %.1f KB |\n\n"
        "接着看内容：`{\"op\":\"urls\",\"file\":\"%s\"}`（域名+网址）、"
        "`{\"op\":\"top\",\"file\":\"%s\"}`（流量排行）",
        iface, secs, pkts,
        filter[0] ? "，filter " : "", filter[0] ? filter : "",
        outp, st.st_size / 1024.0, outp, outp);
    snprintf(ans, sizeof(ans), "抓到 %.1f KB → %s", st.st_size / 1024.0, outp);
    return json_pair(text, ans);
}

static char *render_list(const char *title, item_t *arr, int n, const char *unit) {
    static char text[OUTMAX];
    size_t i = (size_t)snprintf(text, sizeof(text), "%s\n\n", title);
    if (!n) { snprintf(text + i, sizeof(text) - i, "（没有记录）\n"); return text; }
    i += (size_t)snprintf(text + i, sizeof(text) - i, "| # | 名称 | %s |\n|---|---|---|\n", unit);
    for (int k = 0; k < n && i < sizeof(text) - 120; k++)
        i += (size_t)snprintf(text + i, sizeof(text) - i, "| %d | `%s` | %d |\n",
                             k + 1, arr[k].name, arr[k].cnt);
    return text;
}

static char *op_list(const char *json, int which) {
    char iface[32] = {0}, file[512], err[300];
    jget(json, "iface", iface, sizeof(iface));
    if (!iface[0]) default_iface(iface, sizeof(iface));
    int secs = (int)jget_int(json, "secs", 10);
    int captured = 0;

    if (ensure_pcap(json, iface, secs, 5000, file, sizeof(file), err, sizeof(err), &captured) != 0)
        return errf("%s", err, "");
    reset_state();
    if (parse_pcap(file, err, sizeof(err)) != 0) return errf("%s", err, "");

    char note[300];
    snprintf(note, sizeof(note), "%s抓了 %llu 包 / %.1f KB",
             captured ? "现抓 " : "", g_pkts, g_bytes / 1024.0);

    static char text[OUTMAX];
    if (which == 0) {
        char *t = render_list("**DNS 查询过的域名**", g_dns, g_ndns, "查询次数");
        snprintf(text, sizeof(text), "%s\n%s", t, note);
        char ans[128];
        snprintf(ans, sizeof(ans), "DNS 域名 %d 个（%s）", g_ndns, note);
        return json_pair(text, ans);
    }
    if (which == 1) {
        char *t = render_list("**HTTPS 访问的域名（TLS SNI）**", g_sni, g_nsni, "握手次数");
        snprintf(text, sizeof(text),
            "%s\n%s\n\n"
            "> SNI 只在 TLS 握手里有，是明文；**后面的具体路径（/a/b?q=1）是加密的，"
            "没有会话密钥谁都解不开**。要看完整 URL，见下方说明。\n",
            t, note);
        char ans[128];
        snprintf(ans, sizeof(ans), "HTTPS 域名 %d 个（%s）", g_nsni, note);
        return json_pair(text, ans);
    }
    /* HTTP */
    size_t i = (size_t)snprintf(text, sizeof(text),
        "**明文 HTTP 请求（完整 URL 可见）**\n\n");
    if (!g_nhttp) {
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "（没有明文 HTTP 流量 —— 现在绝大多数流量都走 HTTPS）\n");
    } else {
        for (int k = 0; k < g_nhttp && i < sizeof(text) - 200; k++)
            i += (size_t)snprintf(text + i, sizeof(text) - i, "- `%s`\n", g_http[k]);
    }
    snprintf(text + i, sizeof(text) - i, "\n%s", note);
    char ans[128];
    snprintf(ans, sizeof(ans), "明文 HTTP 请求 %d 条（%s）", g_nhttp, note);
    return json_pair(text, ans);
}

static char *op_top(const char *json) {
    char iface[32] = {0}, file[512], err[300];
    jget(json, "iface", iface, sizeof(iface));
    if (!iface[0]) default_iface(iface, sizeof(iface));
    int secs = (int)jget_int(json, "secs", 10);
    int captured = 0;
    if (ensure_pcap(json, iface, secs, 8000, file, sizeof(file), err, sizeof(err), &captured) != 0)
        return errf("%s", err, "");
    reset_state();
    if (parse_pcap(file, err, sizeof(err)) != 0) return errf("%s", err, "");

    /* 简单选择排序，取前 12 */
    for (int a = 0; a < g_npeer; a++)
        for (int b = a + 1; b < g_npeer; b++)
            if (g_peer[b].bytes > g_peer[a].bytes) {
                peer_t t = g_peer[a]; g_peer[a] = g_peer[b]; g_peer[b] = t;
            }
    char text[OUTMAX];
    size_t i = (size_t)snprintf(text, sizeof(text),
        "**流量排行**（%s抓 %llu 包 / %.1f KB）\n\n| # | 对端 IP | 收发 |\n|---|---|---|\n",
        captured ? "现抓，" : "", g_pkts, g_bytes / 1024.0);
    int show = g_npeer < 12 ? g_npeer : 12;
    for (int k = 0; k < show; k++)
        i += (size_t)snprintf(text + i, sizeof(text) - i, "| %d | `%s` | %.1f KB |\n",
                             k + 1, g_peer[k].ip, g_peer[k].bytes / 1024.0);
    char ans[160];
    snprintf(ans, sizeof(ans), "前 %d 个对端，总共 %.1f KB / %llu 包",
             show, g_bytes / 1024.0, g_pkts);
    return json_pair(text, ans);
}

static char *op_urls(const char *json) {
    char iface[32] = {0}, file[512], err[300];
    jget(json, "iface", iface, sizeof(iface));
    if (!iface[0]) default_iface(iface, sizeof(iface));
    int secs = (int)jget_int(json, "secs", 12);
    int captured = 0;
    if (ensure_pcap(json, iface, secs, 8000, file, sizeof(file), err, sizeof(err), &captured) != 0)
        return errf("%s", err, "");
    reset_state();
    if (parse_pcap(file, err, sizeof(err)) != 0) return errf("%s", err, "");

    static char text[OUTMAX];
    size_t i = (size_t)snprintf(text, sizeof(text),
        "**访问了什么**（%s%llu 包 / %.1f KB）\n\n", captured ? "现抓，" : "",
        g_pkts, g_bytes / 1024.0);

    i += (size_t)snprintf(text + i, sizeof(text) - i, "### HTTPS 域名（SNI，%d 个）\n", g_nsni);
    if (!g_nsni) i += (size_t)snprintf(text + i, sizeof(text) - i, "（无）\n");
    for (int k = 0; k < g_nsni && k < 20 && i < sizeof(text) - 300; k++)
        i += (size_t)snprintf(text + i, sizeof(text) - i, "- `%s` ×%d\n", g_sni[k].name, g_sni[k].cnt);

    i += (size_t)snprintf(text + i, sizeof(text) - i, "\n### DNS 域名（%d 个）\n", g_ndns);
    if (!g_ndns) i += (size_t)snprintf(text + i, sizeof(text) - i, "（无，可能都用加密 DNS/DoH）\n");
    for (int k = 0; k < g_ndns && k < 20 && i < sizeof(text) - 300; k++)
        i += (size_t)snprintf(text + i, sizeof(text) - i, "- `%s` ×%d\n", g_dns[k].name, g_dns[k].cnt);

    i += (size_t)snprintf(text + i, sizeof(text) - i, "\n### 明文 HTTP 完整 URL（%d 条）\n", g_nhttp);
    if (!g_nhttp) i += (size_t)snprintf(text + i, sizeof(text) - i, "（无）\n");
    for (int k = 0; k < g_nhttp && k < 12 && i < sizeof(text) - 400; k++)
        i += (size_t)snprintf(text + i, sizeof(text) - i, "- `%s`\n", g_http[k]);

    i += (size_t)snprintf(text + i, sizeof(text) - i,
        "\n---\n**HTTPS 的路径看不到，这是 TLS 本身的性质**，不是抓包没抓到：\n"
        "域名在握手时是明文（SNI），握手之后的内容全部加密，没有会话密钥无法还原。\n"
        "真要拿完整 URL，只有三条路：\n"
        "1. 我们自己的浏览器插件（CDP 直接读页面发出的请求）—— 已经在用，能拿到完整 URL 和头\n"
        "2. 设备端 Trust CA + 中间人代理（mitmproxy/squid ssl_bump）—— 只能解信任我们 CA 的客户端，"
        "App 做了证书固定就无效\n"
        "3. 客户端导出 SSLKEYLOGFILE —— 只有我们能改客户端时才可行\n");

    if (!g_nsni && !g_ndns && !g_nhttp) {
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "\n**没抓到任何流量**（网卡 `%s`，%llu 包）。%s\n"
            "不用重试 —— 换个网卡或抓长一点：`{\"op\":\"urls\",\"iface\":\"br-lan\",\"secs\":15}`\n",
            iface, g_pkts,
            g_pkts < 5 ? "这块网卡上基本没流量，多半选错了：本机上网走默认路由那块（eth0），"
                         "看别的设备走 br-lan。" : "有包但没有 HTTP/DNS/SNI，可能是本机内部通信。");
    }

    char ans[220];
    snprintf(ans, sizeof(ans), "网卡 %s：HTTPS 域名 %d / DNS %d / 明文 URL %d（%s，%llu 包）",
             iface, g_nsni, g_ndns, g_nhttp, captured ? "现抓" : "读包", g_pkts);
    return json_pair(text, ans);
}

/* ---------------- 入口 ---------------- */
static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    if (!op[0]) snprintf(op, sizeof(op), "urls");

    if (!strcmp(op, "ifaces") || !strcmp(op, "iface")) return op_ifaces();
    if (!strcmp(op, "capture") || !strcmp(op, "cap") || !strcmp(op, "tcpdump"))
        return op_capture(json_in);
    if (!strcmp(op, "dns") || !strcmp(op, "dnsq")) return op_list(json_in, 0);
    if (!strcmp(op, "sni"))   return op_list(json_in, 1);
    if (!strcmp(op, "http"))  return op_list(json_in, 2);
    if (!strcmp(op, "top") || !strcmp(op, "talkers")) return op_top(json_in);
    if (!strcmp(op, "urls") || !strcmp(op, "domains") || !strcmp(op, "sites"))
        return op_urls(json_in);

    return errf("不支持的 op：%s。netcap 支持 ifaces / capture / dns / sni / http / top / urls %s",
                op, "");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "netcap",
    "抓包分析（tcpdump）：ifaces=看网卡 capture=抓包落pcap dns=查过哪些域名 "
    "sni=HTTPS访问了哪些域名 http=明文HTTP完整URL top=流量排行 urls=三者合一。"
    "**HTTPS 的路径看不到（TLS 加密），只能看域名**",
    "{\"op\":\"urls\",\"secs\":12}"
    "  或 {\"op\":\"capture\",\"iface\":\"br-lan\",\"secs\":10,\"filter\":\"tcp port 443\"}"
    "  或 {\"op\":\"dns\",\"file\":\"/tmp/llama_agent/cap_xxx.pcap\"}",
    run
};

plugin_desc_t *plugin_entry(void) { return &desc; }
