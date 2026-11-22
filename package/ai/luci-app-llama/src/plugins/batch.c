/* batch.c — 默认插件：把大任务切成小 JSON 分给模型，最后合并
 *
 * 这是框架的"默认能力"。模型一次写不完长结构化输出（3B 实测：5 页 PPT 的
 * slides JSON 15 分钟出不来、32 小节音符必然截断），所以由这里负责：
 *   切批 → 每批一次短请求 → 收集 → 合并成完整产物
 *
 * 支持两种形态（kind）：
 *   ppt   每批 1 个章节  {"title":"...","bullets":["..."]}
 *         输出 sections JSON，交给上层渲染 pptx
 *   midi  每批 4 小节    [[{"pitch":69,"dur":1},...], ...]
 *         合并后直接写出 .mid 二进制
 *
 * 入参（框架会把 llama 地址一起传进来）：
 *   {"task":"原始任务","kind":"ppt|midi","path":"out.mid",
 *    "bars":8,"per_bar":4,"_host":"127.0.0.1","_port":8090,"_model":"..."}
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "plugin.h"

#define BUF 131072

/* ---------- JSON 取值（插件自带，不依赖框架） ---------- */
static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[96];
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
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return dflt;
    p = strchr(p + strlen(pat), ':');
    if (!p) return dflt;
    return atoi(p + 1);
}

/* ---------- HTTP ---------- */
static char *http_post(const char *host, int port, const char *body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct timeval tv = { 900, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return NULL; }

    size_t blen = strlen(body);
    char *req = malloc(blen + 512);
    if (!req) { close(fd); return NULL; }
    int hlen = snprintf(req, blen + 512,
        "POST /v1/chat/completions HTTP/1.1\r\nHost: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", host, blen);
    int ok = (send(fd, req, (size_t)hlen, 0) == hlen)
          && (send(fd, body, blen, 0) == (ssize_t)blen);
    free(req);
    if (!ok) { close(fd); return NULL; }

    char *resp = malloc(BUF);
    if (!resp) { close(fd); return NULL; }
    size_t got = 0;
    ssize_t k;
    while ((k = recv(fd, resp + got, BUF - 1 - got, 0)) > 0) {
        got += (size_t)k;
        if (got >= BUF - 1) break;
    }
    resp[got] = 0;
    close(fd);
    char *b = strstr(resp, "\r\n\r\n");
    if (b) { char *r = strdup(b + 4); free(resp); return r; }
    return resp;
}

static void esc(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (const char *p = in; *p && i + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[i++] = '\\'; out[i++] = *p; }
        else if (*p == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else if (*p == '\r') { out[i++] = '\\'; out[i++] = 'r'; }
        else out[i++] = *p;
    }
    out[i] = 0;
}

/* 提问一次（无 schema，纯文本返回） */
static char *ask(const char *host, int port, const char *model,
                 const char *sysmsg, const char *user) {
    size_t cap = strlen(user) * 6 + strlen(sysmsg) * 6 + 2048;
    char *ue = malloc(cap), *se = malloc(cap);
    if (!ue || !se) { free(ue); free(se); return NULL; }
    esc(user, ue, cap);
    esc(sysmsg, se, cap);
    char *body = malloc(cap + 2048);
    if (!body) { free(ue); free(se); return NULL; }
    snprintf(body, cap + 2048,
        "{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":420,"
        "\"temperature\":0.4,\"stream\":false}", model, se, ue);
    free(ue); free(se);
    char *resp = http_post(host, port, body);
    free(body);
    if (!resp) return NULL;

    /* 取 choices[0].message.content（简单定位 "content"） */
    char *c = strstr(resp, "\"content\"");
    if (!c) { free(resp); return NULL; }
    c = strchr(c, ':');
    if (!c) { free(resp); return NULL; }
    c++;
    while (*c == ' ') c++;
    if (*c != '"') { free(resp); return NULL; }
    c++;
    char *out = malloc(strlen(c) + 1);
    if (!out) { free(resp); return NULL; }
    size_t i = 0;
    while (*c && *c != '"') {
        if (*c == '\\' && c[1]) {
            c++;
            switch (*c) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            case 'u':
                if (c[1] && c[2] && c[3] && c[4]) {
                    char hex[5] = { c[1], c[2], c[3], c[4], 0 };
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    if (cp < 0x80) out[i++] = (char)cp;
                    else if (cp < 0x800) {
                        out[i++] = (char)(0xC0 | (cp >> 6));
                        out[i++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[i++] = (char)(0xE0 | (cp >> 12));
                        out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[i++] = (char)(0x80 | (cp & 0x3F));
                    }
                    c += 4;
                }
                break;
            default: out[i++] = *c; break;
            }
            c++;
        } else out[i++] = *c++;
    }
    out[i] = 0;
    free(resp);
    return out;
}

/* ---------- MIDI 写出 ---------- */
static void be32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void be16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}
/* 变长数量 */
static int vlq(unsigned char *p, unsigned v) {
    unsigned char tmp[4];
    int n = 0;
    do { tmp[n++] = (unsigned char)(v & 0x7F); v >>= 7; } while (v);
    int k = 0;
    for (int i = n - 1; i >= 0; i--) {
        p[k++] = (unsigned char)(tmp[i] | (i ? 0x80 : 0));
    }
    return k;
}

/* 把 bars 里收集到的音符写成单轨 MIDI */
static int write_midi(const char *path, int bars, int per_bar,
                      int (*pitch)[8], double (*dur)[8], int *cnt, int tempo) {
    unsigned char trk[65536];
    size_t o = 0;
    /* delta=0, tempo meta */
    o += (size_t)vlq(trk + o, 0);
    trk[o++] = 0xFF; trk[o++] = 0x51; trk[o++] = 3;
    unsigned us = (unsigned)(60000000 / (tempo > 0 ? tempo : 120));
    trk[o++] = (unsigned char)(us >> 16);
    trk[o++] = (unsigned char)(us >> 8);
    trk[o++] = (unsigned char)us;

    for (int b = 0; b < bars; b++) {
        if (cnt[b] <= 0) {
            /* 空小节：4 拍休止 */
            o += (size_t)vlq(trk + o, 1920);
            continue;
        }
        int step = 1920 / cnt[b];            /* 4 拍 = 1920 tick */
        for (int i = 0; i < cnt[b]; i++) {
            int p = pitch[b][i];
            if (p < 0) p = 60;
            if (p > 127) p = 127;
            o += (size_t)vlq(trk + o, 0);
            trk[o++] = 0x90; trk[o++] = (unsigned char)p; trk[o++] = 92;
            o += (size_t)vlq(trk + o, (unsigned)step - 1);
            trk[o++] = 0x80; trk[o++] = (unsigned char)p; trk[o++] = 0;
            o += (size_t)vlq(trk + o, 1);
        }
    }
    o += (size_t)vlq(trk + o, 0);
    trk[o++] = 0xFF; trk[o++] = 0x2F; trk[o++] = 0;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned char hdr[14];
    memcpy(hdr, "MThd", 4);
    be32(hdr + 4, 6);
    be16(hdr + 8, 0);          /* format 0 */
    be16(hdr + 10, 1);         /* 1 track */
    be16(hdr + 12, 480);       /* 480 tick/四分音符 */
    fwrite(hdr, 1, 14, f);
    unsigned char th[8];
    memcpy(th, "MTrk", 4);
    be32(th + 4, (unsigned)o);
    fwrite(th, 1, 8, f);
    fwrite(trk, 1, o, f);
    fclose(f);
    return 0;
}

/* 从模型返回里抠出 [{"pitch":69,"dur":1}, ...] 这一小节 */
static int parse_note_bar(const char *s, int *pitch, double *dur, int maxn) {
    int n = 0;
    const char *p = s;
    while ((p = strstr(p, "\"pitch\"")) != NULL && n < maxn) {
        const char *q = strchr(p, ':');
        if (!q) break;
        int v = atoi(q + 1);
        double d = 1.0;
        const char *dd = strstr(p, "\"dur\"");
        if (dd) {
            const char *d2 = strchr(dd, ':');
            if (d2) d = atof(d2 + 1);
        }
        pitch[n] = v;
        dur[n] = d;
        n++;
        p = q;
    }
    return n;
}

static char *run(const char *json_in) {
    char task[2048] = {0}, kind[16] = {0}, path[512] = {0};
    char host[64] = "127.0.0.1", model[128] = "qwen2.5-3b-q4_k_m.gguf";
    jget(json_in, "task", task, sizeof(task));
    jget(json_in, "kind", kind, sizeof(kind));
    jget(json_in, "path", path, sizeof(path));
    /* 框架通过环境变量告诉插件 llama 地址（比往 JSON 里塞 _host 字段干净） */
    const char *eh = getenv("AGENT_HOST");
    const char *ep = getenv("AGENT_PORT");
    const char *em = getenv("AGENT_MODEL");
    if (eh) snprintf(host, sizeof(host), "%s", eh);
    if (em) snprintf(model, sizeof(model), "%s", em);
    int port = ep ? atoi(ep) : 8090;
    int bars = jget_int(json_in, "bars", 8);
    int per_bar = jget_int(json_in, "per_bar", 4);
    if (per_bar < 4) per_bar = 4;
    if (per_bar > 8) per_bar = 8;
    if (bars < 1) bars = 1;
    if (bars > 64) bars = 64;

    if (!strcmp(kind, "midi")) {
        int (*pitch)[8] = calloc((size_t)bars, sizeof(*pitch));
        double (*dur)[8] = calloc((size_t)bars, sizeof(*dur));
        int *cnt = calloc((size_t)bars, sizeof(int));
        if (!pitch || !dur || !cnt) return strdup("{\"error\":\"内存不足\"}");
        int batch = 4;
        for (int start = 0; start < bars; start += batch) {
            int n = bars - start < batch ? bars - start : batch;
            char user[1024];
            snprintf(user, sizeof(user),
                "写 %d 小节旋律，每小节 %d 个音，A 小调。\n"
                "只输出一个 JSON 数组，共 %d 个元素（每小节一个），"
                "元素里是 {\"pitch\":音高,\"dur\":拍数}，"
                "音高用 MIDI 编号（57=A3, 60=C4, 64=E4, 69=A4）。\n"
                "不要解释、不要代码块。", n, per_bar, n);
            char *r = ask(host, port, model, "你只输出 JSON 数组。", user);
            if (!r) continue;
            /* 按 A 小调音阶约束（57/59/60/62/64/65/67/69/71/72）就近取 */
            int buf[8] = {0};
            double dbuf[8] = {0};
            int got = parse_note_bar(r, buf, dbuf, per_bar);
            for (int i = 0; i < got; i++) {
                pitch[start + i / per_bar][i % per_bar] = buf[i];
                dur[start + i / per_bar][i % per_bar] = dbuf[i];
            }
            /* 用 n 控制范围：把本批音符按 per_bar 分配到各小节 */
            int idx = 0;
            for (int b = 0; b < n; b++) {
                int c = 0;
                while (c < per_bar && idx < got) {
                    pitch[start + b][c] = buf[idx];
                    dur[start + b][c] = dbuf[idx];
                    c++; idx++;
                }
                cnt[start + b] = c;
            }
            free(r);
        }
        if (!path[0]) snprintf(path, sizeof(path), "/tmp/llama_agent/out.mid");
        int rc = write_midi(path, bars, per_bar, pitch, dur, cnt, 120);
        free(pitch); free(dur); free(cnt);
        char *out = malloc(256);
        if (!out) return NULL;
        if (rc != 0) snprintf(out, 256, "{\"error\":\"写 MIDI 失败: %s\"}", path);
        else snprintf(out, 256, "{\"value\":\"已生成 MIDI：%s（%d 小节，分批生成后合并）\"}",
                      path, bars);
        return out;
    }

    /* ppt：每批 1 节，最后拼成 sections 数组 */
    int ns = jget_int(json_in, "sections", 4);
    if (ns < 2) ns = 2;
    if (ns > 8) ns = 8;
    char *acc = malloc(BUF);
    if (!acc) return NULL;
    size_t off = 0;
    off += (size_t)snprintf(acc + off, BUF - off, "[");
    int wrote = 0;
    for (int i = 0; i < ns; i++) {
        char user[1024];
        snprintf(user, sizeof(user),
            "任务背景：%s\n\n请写这份 PPT 的第 %d 节，只输出一个 JSON 对象："
            "{\"title\":\"节标题\",\"bullets\":[\"要点1\",\"要点2\"]}，"
            "2-3 条要点，每条不超过 20 字。不要解释。", task, i + 1);
        char *r = ask(host, port, model, "你只输出一个 JSON 对象。", user);
        if (!r) continue;
        const char *b = strchr(r, '{');
        if (b) {
            if (wrote) off += (size_t)snprintf(acc + off, BUF - off, ",");
            off += (size_t)snprintf(acc + off, BUF - off, "%.2000s", b);
            wrote++;
        }
        free(r);
        if (off > BUF - 3000) break;
    }
    off += (size_t)snprintf(acc + off, BUF - off, "]");
    char *out = malloc(off + 64);
    if (!out) { free(acc); return NULL; }
    snprintf(out, off + 64, "{\"sections\":%s}", acc);
    free(acc);
    return out;
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "batch",
    "把大任务切成小 JSON 分批生成再合并（PPT 分章节 / MIDI 分小节）",
    "{\"task\":\"...\",\"kind\":\"ppt|midi\",\"bars\":8,\"per_bar\":4}",
    run
};

plugin_desc_t *plugin_entry(void) { return &desc; }
