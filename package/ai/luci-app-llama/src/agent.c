#define _GNU_SOURCE
/* agent.c — C 版 agent 框架
 *
 * 和 Python 版做同样的事：任务丢给 llama-server，模型输出 {"tool":...}，
 * 框架找到同名插件执行、结果回灌，直到模型给 final。
 *
 * 结构差异：每个功能都是独立 .so 插件，框架不认识任何具体功能。
 * 新增能力 = 新增一个 .so，框架零改动。
 *
 * 无第三方依赖：自带最小 JSON 取值 + 裸 socket HTTP。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "plugin.h"
#include "json.h"

#define MAX_PLUGINS 32
#define WORK_DIR "/tmp/llama_agent"
#define BIG 262144
#define SMALL 8192

static plugin_desc_t *g_plug[MAX_PLUGINS];
static int g_n = 0;
static int g_direct_answer = 1;   /* 插件已给答案时不再让模型复述 */
/* 插件启停：名单落在 /etc/llama.plugoff，加载时跳过被停用的插件。
 * 放在文件最前面是因为 env_json 也要读（它比 load_plugins 早）。 */
static char g_plugoff[512] = {0};
static char g_disabled[32][32];
static int  g_ndis = 0;

/* 自动化实例的运行状态落盘在这：/tmp/agent-auto/<name>.pid
 * 页面靠它判断"在跑 / 没跑"，停止也靠它找 pid。
 * 放 /tmp 是有意的：tmpfs，重启自动清空，不会留下脏 pid 文件。 */
#define AUTO_DIR "/tmp/agent-auto"

static int auto_pid_of(const char *name, pid_t *out) {
    char pf[256];
    snprintf(pf, sizeof(pf), AUTO_DIR "/%s.pid", name);
    FILE *f = fopen(pf, "r");
    if (!f) return 0;
    int pid = 0;
    int ok = (fscanf(f, "%d", &pid) == 1);
    fclose(f);
    if (!ok || pid <= 0) return 0;
    if (kill((pid_t)pid, 0) != 0) return 0;      /* 进程没了，脏 pid */
    if (out) *out = (pid_t)pid;
    return 1;
}

static int auto_state(const char *name, pid_t *pid) {
    return auto_pid_of(name, pid);
}

static void auto_pid_write(const char *name) {
    mkdir(AUTO_DIR, 0755);
    char pf[256];
    snprintf(pf, sizeof(pf), AUTO_DIR "/%s.pid", name);
    FILE *f = fopen(pf, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

static void auto_pid_clear(const char *name) {
    char pf[256];
    snprintf(pf, sizeof(pf), AUTO_DIR "/%s.pid", name);
    unlink(pf);
}

/* 自动化插件（第二类，导出 plugin_auto_entry 的那些） */
#define MAX_AUTO 16
static plugin_auto_t *g_auto[MAX_AUTO];
static int g_nauto = 0;

static plugin_desc_t *find_plugin(const char *name);

/* --memcheck 模式下每步打印分配记账（memcheck.c，链接期 --wrap 实现） */
void memcheck_report(const char *where);
static int g_memcheck = 0;
#define MEMCHK(where) do { if (g_memcheck) memcheck_report(where); } while (0)

/* ============ JSON 取值 ============ */
/* llama-server 响应的 content：走正规解析 choices[0].message.content，
 * 取不到再退回顶层 content。不再用 strstr 找 key（字符串里出现
 * "content" 字样会取错位置）。 */

static char *extract_content(const char *resp) {
    char err[128];
    jval *v = json_parse(resp, err, sizeof(err));
    if (!v) return NULL;
    const char *c = NULL;
    jval *msg = json_get(json_at(json_get(v, "choices"), 0), "message");
    c = json_str(json_get(msg, "content"));
    if (!c) c = json_str(json_get(v, "content"));
    char *out = c ? strdup(c) : NULL;
    json_free(v);
    return out;
}

/* 取 finish_reason（"stop" 正常结束 / "length" 撞到 max_tokens 被截断）。
 * 以前完全没看这个字段：模型写到一半被截断，agent 也当正常结果往下走，
 * 前端就把半截 JSON（{"tool":"doc","op":"write","content":"# ...）当答案显示出来。 */
static void extract_finish(const char *resp, char *out, size_t cap) {
    char err[128];
    if (out && cap) out[0] = 0;
    jval *v = json_parse(resp, err, sizeof(err));
    if (!v) return;
    const char *fr = json_str(json_get(json_at(json_get(v, "choices"), 0), "finish_reason"));
    if (fr && out) snprintf(out, cap, "%s", fr);
    json_free(v);
}

/* 旧的 strstr 版取值，留着给插件参数这种扁平结构用（新代码别用） */
__attribute__((unused))
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int flat_get(const char *js, const char *key, char *out, size_t cap) {
    char pat[128];
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
    p = skip_ws(p + 1);
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            case 'b': out[i++] = '\b'; break;
            case 'f': out[i++] = '\f'; break;
            case '"': out[i++] = '"'; break;
            case '\\': out[i++] = '\\'; break;
            case '/': out[i++] = '/'; break;
            case 'u':
                if (p[1] && p[2] && p[3] && p[4]) {
                    char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                    if (cp < 0x80) {
                        out[i++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[i++] = (char)(0xC0 | (cp >> 6));
                        out[i++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[i++] = (char)(0xE0 | (cp >> 12));
                        out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[i++] = (char)(0x80 | (cp & 0x3F));
                    }
                    p += 4;
                }
                break;
            default: out[i++] = *p; break;
            }
            p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return 0;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (const char *p = in; *p && i + 8 < cap; p++) {
        switch (*p) {
        case '"':  i += (size_t)snprintf(out + i, cap - i, "\\\""); break;
        case '\\': i += (size_t)snprintf(out + i, cap - i, "\\\\"); break;
        case '\n': i += (size_t)snprintf(out + i, cap - i, "\\n"); break;
        case '\r': i += (size_t)snprintf(out + i, cap - i, "\\r"); break;
        case '\t': i += (size_t)snprintf(out + i, cap - i, "\\t"); break;
        default:
            if ((unsigned char)*p < 0x20) {
                i += (size_t)snprintf(out + i, cap - i, "\\u%04x", *p);
            } else {
                out[i++] = *p;
            }
        }
    }
    out[i] = 0;
}

/* ============ HTTP ============ */
static char *http_post(const char *host, int port, const char *path,
                       const char *body, int timeout_s) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct timeval tv;
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (getenv("AGENT_DEBUG")) fprintf(stderr, "  [http] connect 失败: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    if (getenv("AGENT_DEBUG")) fprintf(stderr, "  [http] 已连接 %s:%d body=%zu 字节\n", host, port, strlen(body));
    size_t blen = strlen(body);
    size_t rlen = blen + 512;
    char *req = malloc(rlen);
    if (!req) { close(fd); return NULL; }
    int hlen = snprintf(req, rlen,
        "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        path, host, port, blen);
    int ok = (send(fd, req, (size_t)hlen, 0) == hlen)
          && (send(fd, body, blen, 0) == (ssize_t)blen);
    free(req);
    if (!ok) {
        if (getenv("AGENT_DEBUG")) fprintf(stderr, "  [http] send 失败: %s (hlen=%d)\n", strerror(errno), hlen);
        close(fd);
        return NULL;
    }

    char *resp = malloc(BIG);
    if (!resp) { close(fd); return NULL; }
    size_t got = 0;
    ssize_t k;
    while ((k = recv(fd, resp + got, BIG - 1 - got, 0)) > 0) {
        got += (size_t)k;
        if (got >= BIG - 1) break;
    }
    resp[got] = 0;
    close(fd);

    char *bodyp = strstr(resp, "\r\n\r\n");
    if (!bodyp) {
        /* 没有头部结束标记 → 不是合法 HTTP 响应 */
        free(resp);
        return NULL;
    }
    /* 状态码检查：4xx/5xx 直接把响应体当错误返回，让上层报出来 */
    if (strncmp(resp, "HTTP/1.", 7) == 0) {
        int code = atoi(resp + 9);
        if (code >= 400) {
            char *out = strdup(bodyp + 4);
            free(resp);
            return out;
        }
    }
    /* chunked 传输：把各个 chunk 拼起来 */
    char hdr_end = 0;
    (void)hdr_end;
    char *te = NULL;
    {
        /* 只在头部范围内搜，避免误命中 body */
        char save = *bodyp;
        *bodyp = 0;
        te = strcasestr(resp, "transfer-encoding: chunked");
        *bodyp = save;
    }
    if (te) {
        char *src = bodyp + 4;
        size_t cap = strlen(src) + 1;
        char *out = malloc(cap);
        if (!out) { free(resp); return NULL; }
        size_t o = 0;
        for (;;) {
            unsigned long sz = strtoul(src, &src, 16);
            if (sz == 0) break;
            while (*src == '\r' || *src == '\n') src++;
            if (o + sz >= cap) break;
            memcpy(out + o, src, sz);
            o += sz;
            src += sz;
            /* 数据后面还有 CRLF，不跳过的话下一次 strtoul 读到 '\r'
             * 直接返回 0，多 chunk 的响应只会解出第一块 */
            while (*src == '\r' || *src == '\n') src++;
        }
        out[o] = 0;
        free(resp);
        return out;
    }

    char *out = strdup(bodyp + 4);
    free(resp);
    return out;
}

/* 按 UTF-8 字符边界截断：printf("%.400s") 是按字节截的，会切出半个汉字，
 * 页面上显示成 "�"。这里退到最后一个完整字符的边界。 */
static void utf8_cut(char *s, size_t max_bytes) {
    size_t n = strlen(s);
    if (n <= max_bytes) return;
    size_t i = max_bytes;
    /* 往回退到非续字节（0b10xxxxxx）的位置 */
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s[i] = 0;
}


/* SSE 解析用的两个小工具（curl.c 里也有一份，插件各自独立不共享符号） */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int utf8_emit(char *out, unsigned cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ============ 流式请求 ============
 * 3B 在 RK3566 上生成 300 token 要三分钟。一次性返回的话用户盯着空屏幕
 * 等三分钟，以为卡死了。这里边收边打印：每收到一段就把增量写到 stdout
 * （= job 日志），前端轮询就能看到字一个个冒出来。
 *
 * 只处理 llama-server 的 SSE 格式：
 *   data: {"choices":[{"delta":{"content":"字"}}]}
 *   data: [DONE]
 * 返回累积的完整文本（malloc），失败 NULL。
 */
/* 调用 /v1/chat/completions 并在模型"还在加载"时自动等一会儿重试。
 * 装完包/重启后 llama-server 要 10~30 秒才 ready，以前这里直接判失败，
 * 整个任务白跑（用户看到的就是"模型服务没响应"）。 */
static char *http_post_chat(const char *host, int port, const char *body, int timeout) {
    for (int att = 1; att <= 6; att++) {
        char *r = http_post(host, port, "/v1/chat/completions", body, timeout);
        int retry = 0;
        if (!r) retry = 1;
        else if (strstr(r, "Loading model") || strstr(r, "\"code\":503")
                 || strstr(r, "unavailable_error")) retry = 1;
        if (!retry) return r;
        if (att == 6) return r;
        printf("（模型正在加载，%d 秒后重试 %d/6…）\n", 6, att + 1);
        fflush(stdout);
        free(r);
        sleep(6);
    }
    return NULL;
}

static char *http_post_sse(const char *host, int port, const char *path,
                           const char *body, int timeout_s,
                           int emit_stdout) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct timeval tv;
    /* 单次 read 最多等 90 秒。原来是 900 秒 —— 一旦服务端不关连接，
     * 用户要干等 15 分钟（实测卡死过一次，后面的任务全被挡）。 */
    tv.tv_sec = timeout_s > 90 ? 90 : timeout_s;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return NULL; }

    size_t blen = strlen(body);
    size_t rlen = blen + 512;
    char *req = malloc(rlen);
    if (!req) { close(fd); return NULL; }
    snprintf(req, rlen,
        /* 必须 HTTP/1.1：流式响应靠 chunked 编码分隔，1.0 没有 chunked，
         * llama-server 会卡在"怎么结束响应"上（实测占满 4 核不出结果）。
         * chunked 的解码在下面的 read 循环里做。 */
        "POST %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        path, host, port, blen, body);
    size_t sent = 0, rl = strlen(req);
    while (sent < rl) {
        ssize_t w = write(fd, req + sent, rl - sent);
        if (w <= 0) { free(req); close(fd); return NULL; }
        sent += (size_t)w;
    }
    free(req);

    char *acc = malloc(BIG);
    if (!acc) { close(fd); return NULL; }
    size_t alen = 0;
    size_t shown_off = 0;        /* 已经打出去的 answer 字符数（每次调用从 0 开始） */
    char headbuf[8192];
    size_t hn = 0;
    int in_body = 0, done = 0;
    int chunked = 0;
    char line[8192];
    /* chunked 解码状态：0=读长度行 1=读数据 2=吃掉数据后的 CRLF */
    int cstate = 0;
    long cremain = 0;
    char clen[32];
    int cln = 0;

    while (!done) {
        char buf[4096];
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        if (r <= 0) {
            /* 超时或对端不关连接：已经收到的内容照样返回，
             * 别让模型白生成一遍（用户至少能看到已有的部分） */
            if (alen > 0) fprintf(stderr, "（流式读取中断，已收到 %zu 字节）\n", alen);
            break;
        }
        buf[r] = 0;

        if (!in_body) {
            /* 先吃掉 HTTP 头 */
            size_t need = sizeof(headbuf) - hn - 1;
            size_t cp = (size_t)r < need ? (size_t)r : need;
            memcpy(headbuf + hn, buf, cp);
            hn += cp;
            headbuf[hn] = 0;
            char *sep = strstr(headbuf, "\r\n\r\n");
            if (!sep) continue;
            in_body = 1;
            chunked = (strstr(headbuf, "chunked") != NULL);
            /* 头后面可能已经带了正文，把它接着处理（以前直接 continue 丢掉，
             * 小响应会整段丢失） */
            size_t bodyoff = (size_t)(sep + 4 - headbuf);
            if (bodyoff < hn) {
                memmove(buf, headbuf + bodyoff, hn - bodyoff);
                buf[hn - bodyoff] = 0;
            } else {
                continue;
            }
        }

        /* chunked 解码：<hex长度>\r\n<数据>\r\n...0\r\n\r\n */
        char plain[8192];
        const char *p;
        if (chunked) {
            size_t pn = 0;
            for (char *q = buf; *q && pn < sizeof(plain) - 1; q++) {
                if (cstate == 0) {
                    if (*q == '\n') {
                        clen[cln] = 0;
                        cremain = strtol(clen, NULL, 16);
                        cln = 0;
                        cstate = (cremain > 0) ? 1 : 3;   /* 0 = 最后一个 chunk */
                    } else if (*q != '\r' && cln < (int)sizeof(clen) - 1) {
                        clen[cln++] = *q;
                    }
                } else if (cstate == 1) {
                    plain[pn++] = *q;
                    if (--cremain <= 0) cstate = 2;
                } else if (cstate == 2) {
                    if (*q == '\n') cstate = 0;          /* 吃掉数据后的 CRLF */
                } else {                                   /* cstate == 3: 结束 */
                    break;
                }
            }
            plain[pn] = 0;
            p = plain;
        } else {
            p = buf;
        }

        /* 按行切 data: */
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            if (ll > sizeof(line) - 1) ll = sizeof(line) - 1;
            memcpy(line, p, ll);
            line[ll] = 0;
            p = nl ? nl + 1 : p + ll;

            char *d = line;
            while (*d == ' ' || *d == '\r') d++;
            if (strncmp(d, "data:", 5)) continue;
            d += 5;
            while (*d == ' ') d++;
            /* 行尾的 \r 必须去掉：SSE 是 \r\n 结尾，不去掉
             * strcmp("[DONE]") 永远失败 → 生成完了还在等 → 卡到超时 */
            {
                size_t dl = strlen(d);
                while (dl > 0 && (d[dl-1] == '\r' || d[dl-1] == '\n' || d[dl-1] == ' '))
                    d[--dl] = 0;
            }
            if (!strcmp(d, "[DONE]")) { done = 1; break; }

            /* 取 delta.content（流式响应里只有这一处 content） */
            char *c = strstr(d, "\"content\":\"");
            if (!c) continue;
            c += 11;
            while (*c && *c != '"' && alen + 8 < BIG) {
                if (*c == '\\' && c[1]) {
                    c++;
                    switch (*c) {
                    case 'n': acc[alen++] = '\n'; break;
                    case 'r': acc[alen++] = '\r'; break;
                    case 't': acc[alen++] = '\t'; break;
                    case 'u': {
                        unsigned cp2 = 0;
                        int ok2 = 1;
                        for (int k = 1; k <= 4; k++) {
                            int hv = hexval((unsigned char)c[k]);
                            if (hv < 0) { ok2 = 0; break; }
                            cp2 = cp2 * 16 + (unsigned)hv;
                        }
                        if (ok2) { c += 4; alen += (size_t)utf8_emit(acc + alen, cp2); }
                        else acc[alen++] = *c;
                        break;
                    }
                    default: acc[alen++] = *c; break;
                    }
                    c++;
                } else acc[alen++] = *c++;
            }
            acc[alen] = 0;
            if (emit_stdout && alen) {
                /* 只显示 answer 字段的内容，别把 {"tool":"final",...} 这种
                 * 协议 JSON 漏给用户 —— 他看到一屏花括号会以为出错了。 */
                const char *a = strstr(acc, "\"answer\":\"");
                if (a) {
                    size_t begin = (size_t)(a - acc) + 10;     /* 跳过 "answer":" */
                    if (alen > begin) {
                        /* 增量输出（JSON 转义在这里就地还原） */
                        const char *p2 = acc + begin + shown_off;
                        const char *end2 = acc + alen;
                        char obuf[4096];
                        size_t oi = 0;
                        while (p2 < end2 && oi < sizeof(obuf) - 4) {
                            if (*p2 == '\\') {
                                /* 转义序列还没到齐（\ 是这一块的最后一个字节）→
                                 * 不消费，等下一块拼上再处理，否则会打印出字面 \n */
                                if (p2 + 1 >= end2) break;
                                p2++;
                                switch (*p2) {
                                case 'n': obuf[oi++] = '\n'; break;
                                case 'r': obuf[oi++] = '\r'; break;
                                case 't': obuf[oi++] = '\t'; break;
                                case '"': obuf[oi++] = '"'; break;
                                case '\\': obuf[oi++] = '\\'; break;
                                default: obuf[oi++] = *p2; break;
                                }
                                p2++;
                            } else if (*p2 == '"') {
                                break;                          /* 答案结束 */
                            } else obuf[oi++] = *p2++;
                        }
                        if (oi) {
                            fwrite(obuf, 1, oi, stdout);
                            fflush(stdout);
                            shown_off = (size_t)(p2 - (acc + begin));
                        }
                    }
                }
            }
        }
    }
    close(fd);
    acc[alen] = 0;
    return acc;
}

/* ============ 环境检测 ============ */
#include <sys/stat.h>

/* PATH 里找可执行的 name */
static int have_cmd(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin:/usr/sbin:/sbin";
    char buf[512];
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n < sizeof(buf) - 128) {
            snprintf(buf, sizeof(buf), "%.*s/%s", (int)n, p, name);
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISREG(st.st_mode)
                && (st.st_mode & 0111)) {
                return 1;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* 输出环境能力 + 插件清单（页面顶部那个按钮点开要看的东西） */
/* 判断 python 模块在不在：直接看 site-packages 目录，不起 python 进程。
 * 原来是 system("python3 -c 'import xxx'")，6 个库 = 6 次 python3 启动；
 * RK3566 上一次几百毫秒满载，页面每刷新一次就烧掉几秒 CPU。
 * 装了的模块一定有目录/文件，查目录足够判断。 */
static int have_pymod(const char *mod) {
    static const char *dirs[] = {
        "/usr/lib/python3.13/site-packages", "/usr/lib/python3.12/site-packages",
        "/usr/lib/python3.11/site-packages", "/usr/lib/python3.10/site-packages",
        "/usr/lib/python3.9/site-packages",  "/usr/lib/python3.8/site-packages",
        "/usr/lib/python3/dist-packages",    "/usr/lib/python3/site-packages",
        "/usr/lib/python3.11/dist-packages", "/usr/lib/python3.10/dist-packages",
        "/usr/lib/python3.9/dist-packages",
        "/usr/local/lib/python3.11/dist-packages",
        "/usr/local/lib/python3.10/dist-packages",
        "/usr/local/lib/python3.9/dist-packages",
        "/usr/local/lib/python3/site-packages",
        NULL
    };
    char p[320];
    for (int i = 0; dirs[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s", dirs[i], mod);
        if (access(p, F_OK) == 0) return 1;
        snprintf(p, sizeof(p), "%s/%s.py", dirs[i], mod);   /* 单文件模块 */
        if (access(p, F_OK) == 0) return 1;
    }
    return 0;
}

/* 环境结果缓存：--env 结果 60 秒内复用，避免反复刷新页面反复探库 */
#define ENV_CACHE "/tmp/.agent-env.cache"
#define ENV_TTL   60

static int env_from_cache(char *out, size_t cap) {
    struct stat st;
    if (stat(ENV_CACHE, &st) != 0) return 0;
    if (time(NULL) - st.st_mtime > ENV_TTL) return 0;
    FILE *f = fopen(ENV_CACHE, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return n > 1;
}

static void env_to_cache(const char *s) {
    FILE *f = fopen(ENV_CACHE, "w");
    if (!f) return;
    fputs(s, f);
    fclose(f);
}

static void env_json(char *out, size_t cap) {
    /* 缓存只存"探测结果位串"（python3/node/ffmpeg/sympy/midiutil/pypdf/docx/openpyxl/pptx）。
     * 不能连插件列表一起缓存——running/pid 是实时的，缓存会导致
     * 已经停掉的自动化在页面上还显示"运行中"。 */
    char bits[16] = {0};
    size_t bl = 0;
    if (env_from_cache(bits, sizeof(bits))) bl = strlen(bits);
    /* 校验格式：必须是 9 个 0/1。旧版本缓存里存的是 JSON，要认出来重算 */
    int bits_ok = (bl >= 9);
    for (size_t k = 0; bits_ok && k < 9; k++)
        if (bits[k] != '0' && bits[k] != '1') bits_ok = 0;
    if (!bits_ok) {
        int py0 = have_cmd("python3");
        snprintf(bits, sizeof(bits), "%d%d%d%d%d%d%d%d%d",
                 py0, have_cmd("node"), have_cmd("ffmpeg"),
                 py0 ? have_pymod("sympy") : 0,
                 py0 ? have_pymod("midiutil") : 0,
                 py0 ? have_pymod("pypdf") : 0,
                 py0 ? have_pymod("docx") : 0,
                 py0 ? have_pymod("openpyxl") : 0,
                 py0 ? have_pymod("pptx") : 0);
        env_to_cache(bits);
    }
    int py     = bits[0] == '1';
    int f_sympy = bits[3] == '1', f_midi = bits[4] == '1', f_pypdf = bits[5] == '1';
    int f_docx  = bits[6] == '1', f_xlsx = bits[7] == '1', f_pptx  = bits[8] == '1';
    /* jq / curl / ppocr 是外部命令：有没有直接看可执行文件在不在 */
    int f_jq    = access("/usr/bin/jq", X_OK) == 0;
    int f_curl  = access("/usr/bin/curl", X_OK) == 0;
    int f_ppocr = access("/usr/bin/ppocr", X_OK) == 0;

    /* 分两组：runtimes = 底层运行时（谁都得有），libs = 专用库（特定插件才用） */
    size_t off = (size_t)snprintf(out, cap,
        "{\"runtimes\":{\"python3\":%d,\"node\":%d,\"ffmpeg\":%d},"
        "\"libs\":{\"jq\":%d,\"curl\":%d,\"ppocr\":%d,\"sympy\":%d,\"midiutil\":%d,\"pypdf\":%d,\"docx\":%d,"
        "\"openpyxl\":%d,\"pptx\":%d},\"plugins\":[",
        /* 顺序必须跟格式串一致：runtimes 三个（python3/node/ffmpeg），
         * 然后 libs 九个（jq/curl/ppocr/sympy/midiutil/pypdf/docx/openpyxl/pptx） */
        py, bits[1] == '1', bits[2] == '1',
        f_jq, f_curl, f_ppocr,
        f_sympy, f_midi, f_pypdf, f_docx, f_xlsx, f_pptx);
    for (int i = 0; i < g_n && off < cap - 620; i++) {
        /* desc 必须转义：插件描述里出现 " 或 \ （比如 curl 的 "\uXXXX"）
         * 会把整个 env JSON 打断，前端 JSON.parse 失败 → 面板全空。 */
        char ne[256], de[4096];
        json_escape(g_plug[i]->name ? g_plug[i]->name : "", ne, sizeof(ne));
        json_escape(g_plug[i]->description ? g_plug[i]->description : "", de, sizeof(de));
        off += (size_t)snprintf(out + off, cap - off,
            "%s{\"name\":\"%s\",\"desc\":\"%s\",\"kind\":\"tool\"}",
            i ? "," : "", ne, de);
    }
    for (int i = 0; i < g_nauto && off < cap - 700; i++) {
        pid_t apid = 0;
        int running = auto_state(g_auto[i]->name, &apid);
        char ne[256], de[4096], te[256];
        json_escape(g_auto[i]->name ? g_auto[i]->name : "", ne, sizeof(ne));
        json_escape(g_auto[i]->description ? g_auto[i]->description : "", de, sizeof(de));
        json_escape(g_auto[i]->triggers ? g_auto[i]->triggers : "manual", te, sizeof(te));
        off += (size_t)snprintf(out + off, cap - off,
            "%s{\"name\":\"%s\",\"desc\":\"%s\",\"kind\":\"auto\",\"triggers\":\"%s\","
            "\"running\":%d,\"pid\":%d}",
            (g_n + i) ? "," : "", ne, de, te,
            running ? 1 : 0, running ? (int)apid : 0);
    }
    /* 被停用的插件也要报出来，否则页面上看不到它们、没法重新打开 */
    off += (size_t)snprintf(out + off, cap - off, "],\"disabled\":[");
    for (int i = 0; i < g_ndis && off < cap - 80; i++) {
        char de[64];
        json_escape(g_disabled[i], de, sizeof(de));
        off += (size_t)snprintf(out + off, cap - off, "%s\"%s\"", i ? "," : "", de);
    }
    snprintf(out + off, cap - off, "]}");
}

/* ============ 插件依赖 ============
 * 有些插件靠外部命令干活（curl 靠 /usr/bin/curl，jq 靠 /usr/bin/jq）。
 * 命令不在就直接不加载 —— 加载了也是每次调用报"找不到 jq"，
 * 不如让面板里干脆看不到这个插件，同时环境库那边显示缺什么。 */
typedef struct {
    const char *plugin;      /* 插件名 */
    const char *need;        /* 需要的可执行文件（绝对路径） */
    const char *label;       /* 面板里显示的名字 */
} plug_dep_t;

static const plug_dep_t PLUG_DEPS[] = {
    {"curl", "/usr/bin/curl", "curl"},
    {"jq",   "/usr/bin/jq",   "jq"},
    {"browser", "/opt/chrome-root/cdrv.sh", "chromedriver"},
    {NULL, NULL, NULL}
};

/* 这个插件缺不缺依赖；缺的话返回缺的东西，不缺返回 NULL */
static const char *plugin_missing_dep(const char *name) {
    for (int i = 0; PLUG_DEPS[i].plugin; i++) {
        if (strcmp(PLUG_DEPS[i].plugin, name)) continue;
        if (access(PLUG_DEPS[i].need, X_OK) != 0) return PLUG_DEPS[i].need;
        return NULL;
    }
    return NULL;
}

/* ============ 插件 ============ */
static plugin_desc_t *find_plugin(const char *name) {
    /* 先按插件名精确匹配 */
    for (int i = 0; i < g_n; i++) {
        if (strcmp(g_plug[i]->name, name) == 0) return g_plug[i];
    }
    /* 再按别名匹配：模型常把子功能名（calc/solve）当工具名写 */
    for (int i = 0; i < g_n; i++) {
        const char *a = g_plug[i]->aliases;
        if (!a || !*a) continue;
        const char *p = a;
        while (*p) {
            const char *e = strchr(p, ',');
            size_t L = e ? (size_t)(e - p) : strlen(p);
            if (L && strlen(name) == L && !strncmp(p, name, L)) return g_plug[i];
            /* 前缀匹配：tool=uci_show / iw_station 这种"别名_子操作"的写法，
             * 也归到这个插件，具体 op 由插件自己从 tool 名里认。 */
            if (L && strlen(name) > L && !strncmp(p, name, L) && name[L] == '_')
                return g_plug[i];
            if (!e) break;
            p = e + 1;
        }
    }
    return NULL;
}

/* 按名字排序：readdir 的顺序不保证，排序后工具清单每次一致，
 * 模型看到的选择顺序稳定，行为可复现。 */
static int cmp_plug(const void *a, const void *b) {
    const plugin_desc_t *x = *(const plugin_desc_t *const *)a;
    const plugin_desc_t *y = *(const plugin_desc_t *const *)b;
    return strcmp(x->name, y->name);
}

/* ============ 插件启停 ============
 * 插件一多，prompt 里的工具表就长，3B 选错率上升、每步都慢。
 * 允许在页面上关掉不用的插件：名单落在 /etc/llama.plugoff（一行一个名字），
 * 加载时直接不 dlopen，连内存都省了。
 * 被关掉的插件仍然会出现在环境面板里（标 disabled），方便重新打开。 */

static void read_plugoff(void) {
    FILE *f = fopen("/etc/llama.plugoff", "r");
    if (!f) return;
    char line[64];
    size_t off = 0;
    while (fgets(line, sizeof(line), f) && off + 64 < sizeof(g_plugoff)) {
        /* 一行一个，顺手转成逗号分隔 */
        size_t L = strcspn(line, "\r\n");
        if (L == 0) continue;
        line[L] = 0;
        off += (size_t)snprintf(g_plugoff + off, sizeof(g_plugoff) - off,
                                "%s%s", off ? "," : "", line);
    }
    fclose(f);
}

/* 名字是否在停用名单里（逗号分隔精确匹配） */
static int plugin_disabled(const char *name) {
    if (!g_plugoff[0] || !name || !name[0]) return 0;
    size_t nl = strlen(name);
    const char *p = g_plugoff;
    while (*p) {
        const char *c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n == nl && !strncmp(p, name, n)) return 1;
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

static void mark_disabled(const char *name) {
    if (!name || g_ndis >= MAX_PLUGINS) return;
    snprintf(g_disabled[g_ndis], sizeof(g_disabled[0]), "%s", name);
    g_ndis++;
}

static void load_plugins(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "插件目录打不开: %s\n", dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t L = strlen(e->d_name);
        if (L < 4 || strcmp(e->d_name + L - 3, ".so") != 0) continue;
        /* 设置里关掉的插件：连 dlopen 都不做，工具表里也不会出现。
         * 之前只读了 /etc/llama.plugoff 却从来不查，开关等于没接上 ——
         * 用户反馈"关不掉插件"就是这个。 */
        char pname[80];
        snprintf(pname, sizeof(pname), "%.*s", (int)(L - 3), e->d_name);
        if (plugin_disabled(pname)) {
            /* 必须登记：env_json 靠 g_disabled[] 把停用的插件报给前端，
             * 不登记的话设置面板里就再也看不到它、无法重新打开。 */
            mark_disabled(pname);
            fprintf(stderr, "已停用 %s（/etc/llama.plugoff）\n", pname);
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        void *h = dlopen(path, RTLD_NOW);
        if (!h) {
            fprintf(stderr, "跳过 %s: %s\n", e->d_name, dlerror());
            continue;
        }

        int loaded = 0;

        /* ---- 被动工具：plugin_entry() ---- */
        plugin_desc_t *(*entry)(void) =
            (plugin_desc_t *(*)(void))dlsym(h, "plugin_entry");
        if (entry) {
            plugin_desc_t *p = entry();
            if (!p || p->api_version != PLUGIN_API_VERSION) {
                fprintf(stderr, "跳过 %s 的工具接口: API 版本不符\n", e->d_name);
            } else if (plugin_missing_dep(p->name)) {
                fprintf(stderr, "跳过 %s: 缺 %s（这个插件靠它干活）\n",
                        p->name, plugin_missing_dep(p->name));
                loaded = -1;                 /* 标记"因缺依赖跳过"，后面别再注册 */
            } else if (plugin_disabled(p->name)) {
                fprintf(stderr, "跳过 %s: 页面上停用了\n", p->name);
                mark_disabled(p->name);
                loaded = -1;
            } else if (find_plugin(p->name)) {
                fprintf(stderr, "跳过 %s: 工具名重复(%s)\n", e->d_name, p->name);
            } else if (g_n < MAX_PLUGINS) {
                g_plug[g_n++] = p;
                fprintf(stderr, "已加载: %-8s %s\n", p->name, p->description);
                loaded = 1;
            }
        }

        /* ---- 自动化能力：plugin_auto_entry()（可选，独立符号）---- */
        plugin_auto_t *(*aentry)(void) =
            (plugin_auto_t *(*)(void))dlsym(h, "plugin_auto_entry");
        if (aentry) {
            plugin_auto_t *a = aentry();
            if (!a || a->api_version != PLUGIN_AUTO_API_VERSION) {
                fprintf(stderr, "跳过 %s 的自动化接口: 版本不符\n", e->d_name);
            } else if (g_nauto < MAX_AUTO) {
                int dup = 0;
                for (int i = 0; i < g_nauto; i++)
                    if (!strcmp(g_auto[i]->name, a->name)) dup = 1;
                if (!dup) {
                    g_auto[g_nauto++] = a;
                    fprintf(stderr, "自动化: %-8s %s [%s]\n",
                            a->name, a->description, a->triggers ? a->triggers : "manual");
                    loaded = 1;
                }
            }
        }

        if (!loaded) fprintf(stderr, "跳过 %s: 既没有可用的工具接口也没有自动化接口\n", e->d_name);
    }
    closedir(d);
    /* 排序：让工具清单顺序与 readdir 无关 */
    if (g_n > 1) qsort(g_plug, (size_t)g_n, sizeof(g_plug[0]), cmp_plug);
}

/* 热加载：每次任务开始前重扫插件目录。
 * 新增 .so → 自动生效；删掉 .so → 下次重扫就不再出现在清单里。
 * 旧 .so 的句柄不主动 dlclose（可能正被别的请求用着，进程退出时自然回收）。 */
static void reload_plugins(const char *dir) {
    g_n = 0;
    g_nauto = 0;
    load_plugins(dir);
}

/* 按名字找自动化能力 */
static plugin_auto_t *find_auto(const char *name) {
    for (int i = 0; i < g_nauto; i++)
        if (!strcmp(g_auto[i]->name, name)) return g_auto[i];
    return NULL;
}

/* ---------- 纯对话预判 ----------
 * 3B 在 A55 上读 1.5KB 插件描述要 40~70 秒。纯知识问答不需要工具，
 * 把插件表整个省掉，prompt 掉到 150 字节，第一步能快一半。
 * 只认强动作词：宁可漏判（多带插件、慢一点）也别误判（模型手里没工具只能瞎答）。
 * 漏判不死：模型写出没列出的插件名，find_plugin() 仍是全局搜索。 */
static int is_pure_chat(const char *task) {
    static const char *actions[] = {
        "文件", "目录", "磁盘", "内存", "进程", "命令", "shell", "下载",
        "搜索", "grep", "读取", "写入", "负载", "温度", "系统信息",
        "ping", "路由", "dns", "端口", "网卡", "curl", "http", "接口", "api",
        "uci", "wifi", "无线", "ssid", "信道", "logread", "iwinfo",
        "表格", "csv", "xlsx", "excel", "docx", "pdf", "pptx", "排序", "排行",
        "计算", "解方程", "方程组", "求平均", "中位数", "方差", "质因数",
        "生成", "创建", "做一个", "做个", "写个", "脚本", "转成", "导出",
        "midi", "mp3", "旋律", "监控", "巡检",
        NULL
    };
    if (!task || !task[0]) return 0;
    for (int i = 0; actions[i]; i++)
        if (strstr(task, actions[i])) return 0;
    return 1;
}

/* ---------------- 任务 → 相关插件 ----------------
 * 带文档扩展名的任务只给 doc：以前 ".xlsx" 同时命中 doc 和 sys（sys 有
 * read 操作），模型看到两个能读文件的插件就挑错，去列目录了。
 *
 * 3B 的上下文很宝贵：把 10 个插件全塞进 system prompt，光读就要一两百秒，
 * 而且选择面越宽越容易选错。这里先按任务特征挑出 1~3 个相关的，
 * 模型只在这几个里挑。
 *
 * 安全性：框架的 find_plugin() 仍是全局搜索 —— 模型就算写出没列出的插件名
 * 也能执行，只是 prompt 里没提示它。所以"筛错"最坏是多花几步，不会死。
 * 一个关键词都不命中就全列（宁可多带，别让模型手里没工具）。
 */
/* 框架已经自己跑过 curl（用户粘了命令）→ 数据就在上下文里，
 * 不需要再列别的插件让模型去"取数据"。 */
static int g_prefetched = 0;
/* tools_prompt 判定这个任务是纯对话（不需要任何工具）时置 1。
 * 主循环据此换一套 system prompt —— 带 op 归属表和少样本的那套会被
 * 小模型当成"必须调工具"的证据，问"解释一下 NPU"它真会去调 jq。 */
static int g_pure_chat_task = 0;
/* 框架直连登录失败时的诊断，会注入给模型判断 */
static char g_login_diag[1600] = {0};

typedef struct {
    const char *plugin;
    const char *keys;        /* 用 | 分隔；中文直接 strstr */
} route_t;

static const route_t ROUTES[] = {
    {"math",  "算|解方程|方程式|方程组|多少次|平均值|平均数|中位数|方差|标准差|"
              "质因数|阶乘|排列|组合|斐波那契|相遇|追及|环形跑道|火车|过桥|工程|合作|鸡兔|"
              "百分|比例|面积|周长|体积|浓度|利润|打折|原题|应用题|求.*的值"},
    {"curl",  "curl|http|https|接口|api|请求|抓取|抓包|爬|网页数据|token|认证|header|"
              "cookie|接口返回|调接口"},
    {"sys",   "文件|目录|磁盘|内存|CPU|cpu|温度|进程|命令|shell|下载|搜索|grep|"
              "读取|写入|系统信息|负载|占用|最大|最小的.*文件|查看.*内容|文本"},
    {"net",   "网络|ping|路由|dns|端口|网卡|连通|丢包|延迟|路由表|域名|解析"},
    {"owrt",  "uci|openwrt|wifi|无线|ssid|信道|服务|重载|reload|日志|logread|"
              "路由器|配置.*网络|init\\.d|iwinfo"},
    {"doc",   "pdf|PDF|word|Word|excel|Excel|ppt|PPT|文档|xlsx|docx|pptx|"
              "导出|生成.*报告|做个.*表|幻灯片|演示"},
    {"music", "音乐|midi|MIDI|旋律|作曲|编曲|mp3|MP3|节奏|伴奏|曲子|歌曲|音色|五线谱|乐谱|曲谱|简谱"},
    {"browser", "需要登录|登录后|cookie|session|鉴权|自动化浏览器|selenium"},
    {"table", "排序|排行|top|TOP|筛选|名次|前.*名"},
    /* JSON 类：探结构 / 取字段 / 导 CSV。以前表里没有 jq，只有"全都不命中
     * 才全列兜底"时它才进 prompt —— 问"这个 json 有哪些字段"基本选不到它。 */
    {"batch", "分批|大量|很多(页|章|节)|长文档|多章节|整套"},
    {"sysmon","监控|巡检|告警|阈值|采样"},
    {"voice", "语音|朗读|念出来|读出来|配音|播报|TTS|tts|转语音|文字转语音"},
    /* 抓包类：必须单独一项，否则这句话在 3B 眼里就是个知识问答
     * （实测直接答"你可以用 Wireshark"）。命中后只提供 netcap 一个插件，
     * 模型没有别的可挑，选对的概率高得多。 */
    {"video",  "视频|竖屏|横屏|转码|压缩视频|压缩.*视频|mp4|MP4|抽帧|缩略图|截图.*视频|"
               "GIF|gif|特效|水印|淡入|淡出|变速|播放|合并.*视频|视频.*合并|切片|分段|"
               "提取音轨|静音|场景检测|黑屏|转成mkv|转成mov|去字幕|去除字幕|字幕|"
               "去掉字幕|擦掉.*字幕|超分|画面优化|提升画质|变清晰|清晰度"},
    {"netcap", "抓包|tcpdump|pcap|sniff|"
               "抓.{0,4}秒|抓.{0,6}流量|流量.{0,8}(访问|看看|哪些)|"
               "访问了什么|访问了哪些|访问哪些网站|谁在占带宽|带宽占用"},
    /* 路由表是按 '|' 切成子串、用 strstr 逐个子串匹配的 —— 这里写 ".*"
     * 没用（会被当字面量），写「字段 / 多少条 / 有几条」又会把表格类问题
     * （"这个表有多少条记录""这列字段"）也拉进 jq，3B 于是改挑 jq，识别就乱了。
     * 现在只留"明确在说 JSON"的字面子串。 */
    {"jq",    "jq|JSON|json|键值|导出csv|导出 csv|转csv|转 csv|翻页"},
};

/* 返回选中插件的下标数组，返回个数；0 = 没命中（调用方应该全列） */
/* ---------- 插件命令直连 ----------
 * 用户直接把插件命令贴进来（形如 {"op":"probe","url":"..."}）时，别丢给
 * 模型猜：3B 模型不认识这种输入，会当普通文本乱答（实测 {"op":"probe",...}
 * 被答成 "Goofish / [统计] 12 个插件 / C 框架"）。这里查表找到插件直接执行，
 * 结果原样返回 —— 跟登录场景的"框架直连"是同一个思路。
 */
static int json_str_field(const char *js, const char *key, char *out, size_t cap) {
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
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

static const char *op_to_plugin(const char *js, const char *op) {
    /* 显式指定优先：{"plugin":"jq","op":"probe","path":"..."} */
    static char pe[32];        /* static：这个指针要返回出去，不能放栈上 */
    pe[0] = 0;
    if (json_str_field(js, "plugin", pe, sizeof(pe)) == 0 && pe[0]) return pe;

    const int has_url  = strstr(js, "\"url\"")  != NULL;
    const int has_path = strstr(js, "\"path\"") != NULL || strstr(js, "\"file\"") != NULL;

    /* read / write 有歧义：Office/PDF 走 doc，纯文本走 sys。
     * 注意别把 .txt/.md/.csv 算给 doc —— doc 没有 write 这个 op（它只认
     * text/docx/xlsx/pptx/pdf），路由过去会掉进读逻辑报"读不出内容"。
     * sys 自己就能读写纯文本。 */
    if (!strcmp(op, "read") || !strcmp(op, "write")) {
        static const char *EXT[] = {".pdf", ".docx", ".xlsx", ".pptx",
                                    ".doc", ".xls", ".ppt", NULL};
        for (int i = 0; EXT[i]; i++)
            if (strstr(js, EXT[i])) return "doc";
        return "sys";
    }
    /* probe 也有歧义：带 url 是 browser 探测页面，带 path 是 jq 探测 JSON */
    if (!strcmp(op, "probe")) return (has_path && !has_url) ? "jq" : "browser";
    /* plan 也有歧义：带 chunks 是 batch 的分片规划，否则是 math 的计划类应用题 */
    if (!strcmp(op, "plan")) return strstr(js, "\"chunks\"") ? "batch" : "math";

    static const struct { const char *op, *plug; } M[] = {
        {"autologin","browser"}, {"login","browser"}, {"hunt","browser"},
        {"drag","browser"}, {"captcha","browser"}, {"cookie","browser"},
        {"shot","browser"},
        {"ping","net"}, {"port","net"}, {"resolve","net"}, {"route","net"},
        {"arp","net"}, {"link","net"}, {"wifi","net"}, {"trace","net"},
        {"hostname","net"}, {"stat","net"},
        {"calc","math"}, {"solve","math"}, {"system","math"}, {"num","math"},
        {"meet","math"}, {"ring","math"}, {"train","math"}, {"work","math"},
        {"chicken","math"}, {"plan","math"},
        {"get","jq"}, {"query","jq"}, {"table","jq"}, {"keys","jq"},
        {"csv","jq"}, {"extract","jq"}, {"page","jq"},
        {"sort","table"},        /* table 插件的排序；jq 的 table 是导出 CSV，不冲突 */
        /* doc 的写操作：源码 is_write_op() 认这几个，不是 "write" */
        {"docx","doc"}, {"xlsx","doc"}, {"pptx","doc"}, {"pdf","doc"},
        {"text","doc"}, {"docx_append","doc"}, {"xlsx_edit","doc"},
        {"xlsx_read","doc"},
        {"ocr","doc"}, {"image","doc"}, {"img2txt","doc"},   /* 图片认字（RKNPU PP-OCR） */
        {"sysinfo","sys"}, {"shell","sys"}, {"list","sys"}, {"du","sys"},
        {"grep","sys"}, {"http_get","sys"}, {"download","sys"},
        {"uci_get","owrt"}, {"uci_set","owrt"}, {"uci_commit","owrt"},
        {"capture","netcap"}, {"urls","netcap"}, {"sni","netcap"},
        {"dnsq","netcap"}, {"cap_http","netcap"}, {"ifaces","netcap"},
        {"top","netcap"},
        {"portrait","video"}, {"effects","video"}, {"video_info","video"},
        {"trim","video"}, {"thumb","video"}, {"gif","video"}, {"hw","video"},
        {"compress","video"}, {"audio","video"}, {"watermark","video"},
        {"crop","video"}, {"scale","video"}, {"concat","video"}, {"merge","video"},
        {"split","video"}, {"sheet","video"}, {"convert","video"},
        {"detect","video"}, {"job","video"},
        {"desub","video"}, {"delogo","video"}, {"nsub","video"},
        /* 画面优化（2 倍超分）。这四个别名必须留在表里：漏了的话
           {"op":"sr","path":...} 不会被直连，会退回给模型猜，慢好几倍。 */
        {"sr","video"}, {"upscale","video"}, {"enhance","video"}, {"sr2x","video"},
        {"uci_show","owrt"}, {"uci_changes","owrt"}, {"uci_delete","owrt"},
        {"uci_add_list","owrt"}, {"uci_del_list","owrt"}, {"uci_revert","owrt"},
        {"iwinfo","owrt"}, {"iw","owrt"}, {"service","owrt"}, {"logread","owrt"},
        {"passwd","owrt"}, {"reboot","owrt"}, {"opkg_list","owrt"},
        {"midi","music"}, {"render","music"}, {"import","music"}, {"wav","music"},
        {"omr","music"}, {"sheetmusic","music"},    /* 五线谱图片→MIDI */
        {"bgm","music"},
        {"parse","curl"},        /* curl 插件的 op 名是 parse（吃 cURL 原文） */
        {"tts","voice"}, {"speak","voice"}, {"say","voice"}, {"voice","voice"},
        {"asr","voice"}, {"stt","voice"}, {"transcribe","voice"},   /* 语音转文字 */
        {"snapshot","sysmon"},
        {"start","sysmon-auto"}, {"stop","sysmon-auto"}, {"status","sysmon-auto"},
        {NULL, NULL}
    };
    for (int i = 0; M[i].op; i++)
        if (!strcmp(M[i].op, op)) return M[i].plug;
    return NULL;
}

/* 前端会把"已上传的文件 / 上文参考"这段框架自己生成的说明拼在任务前面。
 * 那段里写着"服务端已转成纯文本"、文件名里还带 .xlsx —— 这些词正好命中
 * 直连通道的关键词（"转" + "xlsx" = 打包 xlsx），用户真正的请求（"展示
 * 表格里播放量 top10"）就被顶掉，跑完只得到一句"直接转：用最新 csv"。
 *
 * 意图判断只看用户自己写的那一段：附件块以【用户已上传的文件 开头，
 * 到第一个空行为止；追问块以【当前任务（只做这个）】标记结尾。 */
static const char *intent_text(const char *task) {
    static char buf[4096];
    if (!task || !task[0]) return "";
    const char *cur = strstr(task, "【当前任务");
    if (cur) {
        const char *nl = strstr(cur, "】");
        if (nl) { snprintf(buf, sizeof(buf), "%s", nl + strlen("】")); return buf; }
    }
    if (!strncmp(task, "【用户已上传的文件", strlen("【用户已上传的文件"))) {
        const char *p = strstr(task, "\n\n");
        if (p) { snprintf(buf, sizeof(buf), "%s", p + 2); return buf; }
    }
    return task;
}

/* 认出是插件命令就直接执行，返回 1（结果已打到 stdout） */
static int try_direct_plugin(const char *task) {
    if (!task || !task[0]) return 0;
    /* 命令不一定在第一个字节：页面在上传过文件后会在任务前面拼一段
     * "【用户已上传的文件…】
<路径>

"，原来 task[0] != '{' 就直接放弃，
     * 于是 {"op":"sr",...} 被丢给模型猜（160 秒）。这里跳到第一个 '{'，
     * 并要求整段确实是一个完整的 JSON 对象才认。 */
    const char *js = strchr(task, '{');
    if (!js) return 0;
    {
        const char *tail = js + strlen(js);
        while (tail > js && (tail[-1] == ' ' || tail[-1] == '\n' ||
                             tail[-1] == '\r' || tail[-1] == '\t')) tail--;
        if (tail <= js + 1 || tail[-1] != '}') return 0;
    }
    char op[48] = {0};
    json_str_field(js, "op", op, sizeof(op));
    const char *pw = NULL;
    if (op[0]) {
        pw = op_to_plugin(js, op);
    } else {
        /* 没写 op 的命令：按内容形状归到 doc（生成类），或按扩展名归到 doc（读文档）。
         * 别的插件都要 op 才能确定动作，不猜。 */
        if (strstr(js, "\"markdown\"") || strstr(js, "\"sheets\"") || strstr(js, "\"pages\"")
            || strstr(js, "\"csv_files\"") || strstr(js, "\"csv\"") || strstr(js, "\"slides\""))
            pw = "doc";
        else if (strstr(js, ".pdf") || strstr(js, ".docx") || strstr(js, ".xlsx")
                 || strstr(js, ".xlsm") || strstr(js, ".pptx"))
            pw = "doc";
    }
    if (!pw) return 0;
    plugin_desc_t *pl = find_plugin(pw);
    if (!pl) return 0;
    printf("【插件 %s 开始工作】\n", pw);
    fflush(stdout);
    char *res = pl->run(js);
    if (res) {
        printf("%s\n", res);
        free(res);
    }
    return 1;
}

static int route_plugins(const char *task, int *idx, int maxn) {
    /* 任务里带网址、并且是要"抓/看/访问页面" → 只给 sys(能 http_get) 和
     * browser(能真正打开页面)。否则模型会去挑 curl，而 curl 要的是浏览器
     * 复制的整段命令，不是网址，一调就报"缺少 url"，白烧步数。 */
    if (maxn > 0 && task && (strstr(task, "http://") || strstr(task, "https://"))
        && (strstr(task, "抓取") || strstr(task, "爬取") || strstr(task, "抓")
            || strstr(task, "访问") || strstr(task, "打开") || strstr(task, "页面")
            || strstr(task, "网页") || strstr(task, "404") || strstr(task, "状态码"))) {
        int n = 0;
        for (int i = 0; i < g_n && n < maxn; i++)
            if (!strcmp(g_plug[i]->name, "sys") || !strcmp(g_plug[i]->name, "browser")
                || !strcmp(g_plug[i]->name, "curl"))
                idx[n++] = i;
        if (n > 0) return n;
    }
    /* 文档扩展名 → 只给 doc，不给别的选择 */
    /* 提到登录/验证码/抓请求头 → 只给 browser。curl 只能发请求，处理不了登录框，
     * 也拿不到 JS 现算的签名头（那得让浏览器自己跑一遍）。 */
    if (maxn > 0 && task && (strstr(task, "登录") || strstr(task, "登陆")
                             || strstr(task, "验证码") || strstr(task, "扫码")
                             || strstr(task, "请求头") || strstr(task, "抓包")
                             || strstr(task, "接口签名") || strstr(task, "签名参数")
                             || strstr(task, "login") || strstr(task, "signin"))) {
        for (int i = 0; i < g_n; i++)
            if (!strcmp(g_plug[i]->name, "browser")) { idx[0] = i; return 1; }
    }
    if (maxn > 0 && task && (strstr(task, ".xlsx") || strstr(task, ".xlsm")
                 || strstr(task, ".docx") || strstr(task, ".pptx")
                 || strstr(task, ".pdf"))) {
        for (int i = 0; i < g_n; i++)
            if (!strcmp(g_plug[i]->name, "doc")) { idx[0] = i; return 1; }
    }
    if (!task || !task[0]) return 0;
    int n = 0;
    int nroute = (int)(sizeof(ROUTES) / sizeof(ROUTES[0]));
    for (int i = 0; i < nroute && n < maxn; i++) {
        int pk = -1;
        for (int k = 0; k < g_n; k++)
            if (!strcmp(g_plug[k]->name, ROUTES[i].plugin)) { pk = k; break; }
        if (pk < 0) continue;                     /* 这个插件没装，跳过 */

        /* jq 只该在"明确在说 JSON"时出现。表格类任务（表格/表里/csv/xlsx/excel）
         * 且没提 json 时直接不给 —— 3B 看到候选里有 jq 就会去挑，识别就乱。 */
        if (!strcmp(ROUTES[i].plugin, "jq")) {
            int jsony = strstr(task, "json") || strstr(task, "JSON") || strstr(task, "Json");
            int datat = strstr(task, "表格") || strstr(task, "表里") || strstr(task, "csv")
                     || strstr(task, "CSV") || strstr(task, "xlsx") || strstr(task, "xls")
                     || strstr(task, "Excel") || strstr(task, "excel");
            if (datat && !jsony) continue;
        }

        char keys[2048];
        snprintf(keys, sizeof(keys), "%s", ROUTES[i].keys);
        char *save = NULL;
        int hit = 0;
        for (char *kw = strtok_r(keys, "|", &save); kw; kw = strtok_r(NULL, "|", &save)) {
            if (*kw && strstr(task, kw)) { hit = 1; break; }
        }
        if (hit) idx[n++] = pk;
    }
    return n;
}

static char *tools_prompt(const char *task) {
    char *s = malloc(SMALL);
    if (!s) return NULL;
    size_t off = 0;

    /* 框架刚刚自己试过登录但没成 —— 把原因告诉模型，让它判断该建议什么，
     * 而不是让它一头雾水地重试一遍 */
    if (g_login_diag[0]) {
        off += (size_t)snprintf(s + off, SMALL - off,
            "【重要背景】框架刚替你尝试过自动登录，结果失败了：\n%s\n"
            "请判断原因，给用户明确结论和建议。不要自己反复重试登录"
            "（验证码认得出来早成功了）。\n\n", g_login_diag);
    }

    int idx[8];
    int nsel = 0;

    g_pure_chat_task = 0;
    /* 纯对话：路由一个都没命中，也没有强动作词 → 不给插件表。
     * 顺序很重要：先跑路由（math 的关键词里有"算"，"帮我算一下"能命中），
     * 命中就说明要动手，别当纯对话。 */
    if (!g_prefetched) {
        nsel = route_plugins(task, idx, 8);
        if (nsel == 0 && is_pure_chat(task)) {
            off += (size_t)snprintf(s + off, SMALL - off,
                "这个问题不需要任何工具，直接回答。\n"
                "格式：{\"tool\":\"final\",\"answer\":\"你的回答\"}\n"
                "回答用中文，条理清楚，别超过 200 字。\n");
            g_pure_chat_task = 1;
            if (getenv("AGENT_DEBUG")) fprintf(stderr, "  [路由] 纯对话，不带插件\n");
            return s;
        }
    }

    if (g_prefetched) {
        /* 数据已经抓好放上下文里了，只留 curl（要 jq 分批）和 sys（可能读本地文件） */
        for (int k = 0; k < g_n && nsel < 2; k++) {
            if (!strcmp(g_plug[k]->name, "curl") || !strcmp(g_plug[k]->name, "sys"))
                idx[nsel++] = k;
        }
    } else {
        nsel = route_plugins(task, idx, 8);
    }
    if (getenv("AGENT_DEBUG")) {
        fprintf(stderr, "  [路由] ");
        if (nsel > 0) {
            for (int i = 0; i < nsel; i++) fprintf(stderr, "%s ", g_plug[idx[i]]->name);
        } else {
            fprintf(stderr, "没命中关键词，全列 %d 个", g_n);
        }
        fprintf(stderr, "\n");
        fflush(stderr);      /* 重定向到文件时 stderr 可能被缓冲，调试信息会丢 */
    }

    if (nsel > 0) {
        off += (size_t)snprintf(s + off, SMALL - off, "可用工具：\n");
        for (int i = 0; i < nsel && off < SMALL - 400; i++) {
            plugin_desc_t *p = g_plug[idx[i]];
            char dshort[256];
            snprintf(dshort, sizeof(dshort), "%s", p->description ? p->description : "");
            utf8_cut(dshort, 80);
            off += (size_t)snprintf(s + off, SMALL - off, "- %s：%s\n", p->name, dshort);
        }
    } else {
        /* 没识别出领域：全列。宁可多带，别让模型手里没工具。 */
        off += (size_t)snprintf(s + off, SMALL - off, "可用工具：\n");
        for (int i = 0; i < g_n && off < SMALL - 400; i++) {
            /* 只给名字 + 描述开头。参数示例（params）动辄几百字，10 个插件加起来
             * 能把 system prompt 撑到 4KB —— RK3566 上 3B 一步要多跑两三百秒。 */
            char dshort[256];
            snprintf(dshort, sizeof(dshort), "%s", g_plug[i]->description ? g_plug[i]->description : "");
            utf8_cut(dshort, 80);
            off += (size_t)snprintf(s + off, SMALL - off, "- %s：%s\n",
                                    g_plug[i]->name, dshort);
        }
    }
    off += (size_t)snprintf(s + off, SMALL - off,
        "- final：最终答案  参数 {\"answer\":\"...\"}\n");
    return s;
}

/* 把最近产出的文件塞进上下文。
 * 用户常说"读取上面获取到的 json 文件"—— 但每个任务都是独立进程，
 * 上一轮的 resp_xxx.json 根本不在上下文里，模型只能瞎猜一个假路径。 */
static void inject_recent_files(char *log, size_t cap) {
    typedef struct { char name[400]; time_t mt; } fent_t;
    static fent_t f[64];
    int n = 0;

    static const char *dirs[] = { WORK_DIR "/downloads", WORK_DIR "/gen", NULL };
    for (int di = 0; dirs[di] && n < 60; di++) {
        DIR *d = opendir(dirs[di]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < 60) {
            if (e->d_name[0] == '.') continue;
            char p[400];
            snprintf(p, sizeof(p), "%s/%s", dirs[di], e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            snprintf(f[n].name, sizeof(f[n].name), "%s", p);
            f[n].mt = st.st_mtime;
            n++;
        }
        closedir(d);
    }
    if (n == 0) return;

    /* 按修改时间倒序（n 很小，冒泡够用） */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (f[j].mt > f[i].mt) { fent_t t = f[i]; f[i] = f[j]; f[j] = t; }

    size_t off = strlen(log);
    if (off + 600 >= cap) return;
    off += (size_t)snprintf(log + off, cap - off,
        "\n【最近产出的文件（按时间倒序，用户说\"上面的文件\"一般指第一个）】\n");
    for (int i = 0; i < n && i < 5 && off + 260 < cap; i++)
        off += (size_t)snprintf(log + off, cap - off, "  %s\n", f[i].name);
}

/* 往对话记录里追加一行（结果回灌） */
static void log_append(char *log, size_t cap, const char *fmt, const char *a, const char *b) {
    size_t off = strlen(log);
    if (off + 2048 >= cap) return;
    snprintf(log + off, cap - off, fmt, a, b);
}

/* ============ 插件联动 ============
 * 一个插件的产出是另一个插件的输入时，别让用户再补一句话。
 * 规则表化：from 插件结果里的 field 等于 equals，就自动调 to，
 * 把结果里的 file 填进 args 的 %f。
 *
 * 现在一条：curl 抓到 JSON → jq 探测结构（有哪些字段、多少条）。
 * 响应不是 JSON（type != "json"）自然不触发。 */
typedef struct {
    const char *from;
    const char *field;
    const char *equals;
    const char *to;
    const char *args;
} pipe_t;

static const pipe_t PIPES[] = {
    {"curl", "type", "json", "jq", "{\"op\":\"probe\",\"file\":\"%f\"}"},
    {NULL, NULL, NULL, NULL, NULL}
};

/* ============ 任务分块 ============
 * 一句话里常有好几个动作（"输出表格，再转成 xlsx"）。以前是在一个 if 里
 * 把表格和 xlsx 写死串起来，用户加第三个动作就得改代码，而且后一句很容易
 * 被忽略。这里改成规则表：按关键词在任务里的位置切成有序步骤依次执行，
 * 后一步从上一步的产出里取文件继续。 */
typedef struct {
    const char *keys;      /* 触发关键词，| 分隔 */
    const char *plugin;    /* 执行插件 */
    const char *op;        /* 插件 op */
    int use_prev;          /* 1 = 要吃上一步的产出（file/sheets） */
} chunk_rule_t;

static const chunk_rule_t CHUNKS[] = {
    /* 只认"把数据做成表格"这个动作。以前关键词是光秃秃的「表格」，
     * "展示表格内播放量 top10" 也会命中，框架就抢在模型前面把最新一份
     * csv 原样 dump 15 行 —— 用户要的是按列排的榜单，拿到的是无关子表。 */
    {"转成表格|导出表格|生成表格|做成表格|弄成表格|输出表格|存成表格|"
     "保存为表格|表格化|转表格",  "jq",  "table", 0},
    {"xlsx|excel|电子表格",     "doc", "xlsx",  1},
    /* 系统状态类：确定性查询，直接调 sysinfo，别让模型先 list 再瞎算 */
    {"磁盘空间|剩余空间|磁盘占用|内存占用|系统负载|芯片温度|系统信息|load average|df|free -m",
                                "sys", "sysinfo", 0},
    {NULL, NULL, NULL, 0}
};

/* 按顺序执行任务里的各个动作块；返回执行了几块（0 = 没命中，走模型） */
/* downloads 里有没有 30 分钟内的 json —— 表格类分块靠它判断"有没有数据源" */
static int has_fresh_json(void) {
    DIR *d = opendir(WORK_DIR "/downloads");
    if (!d) return 0;
    struct dirent *e;
    time_t now = time(NULL);
    int ok = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!strstr(e->d_name, ".json")) continue;
        char p[900];
        snprintf(p, sizeof(p), "%s/downloads/%s", WORK_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (now - st.st_mtime < 1800) { ok = 1; break; }
    }
    closedir(d);
    return ok;
}

static int run_task_chunks(const char *task) {
    typedef struct { size_t pos; int ri; } hit_t;
    hit_t hits[8];
    int nh = 0;

    for (int ri = 0; CHUNKS[ri].keys; ri++) {
        char keys[512];
        snprintf(keys, sizeof(keys), "%s", CHUNKS[ri].keys);
        char *sv = NULL;
        size_t best = (size_t)-1;
        for (char *k = strtok_r(keys, "|", &sv); k; k = strtok_r(NULL, "|", &sv)) {
            if (!k[0]) continue;
            const char *q = strstr(task, k);
            if (q && (size_t)(q - task) < best) best = (size_t)(q - task);
        }
        if (best != (size_t)-1 && nh < 8) { hits[nh].pos = best; hits[nh].ri = ri; nh++; }
    }
    if (nh == 0) return 0;

    /* 按在任务里出现的先后排序 */
    for (int i = 0; i < nh; i++)
        for (int j = i + 1; j < nh; j++)
            if (hits[j].pos < hits[i].pos) { hit_t t = hits[i]; hits[i] = hits[j]; hits[j] = t; }

    char prev_file[900] = {0};        /* 上一步产出的主 csv */
    char prev_sheets[4096] = {0};     /* 上一步产出的子表，预拼成 ,{"name":..,"path":..} */
    int ran = 0;

    for (int hi = 0; hi < nh; hi++) {
        const chunk_rule_t *cr = &CHUNKS[hits[hi].ri];
        plugin_desc_t *pl = find_plugin(cr->plugin);
        if (!pl) continue;

        /* jq 的 table 必须有数据源。以前不管有没有数据都执行，jq 只好退到
         * downloads 里最新的 json —— 拿一份无关旧数据编出一张看着像样的表，
         * 用户看到的就"jq 报错"或"答非所问"。这里先判断，没源就跳过这一步。 */
        if (!strcmp(cr->plugin, "jq") && !strcmp(cr->op, "table")
            && !prev_file[0] && !g_prefetched && !has_fresh_json())
            continue;

        char args[8192];
        if (cr->use_prev) {
            if (!prev_file[0]) continue;         /* 没有数据源，跳过这一步 */
            snprintf(args, sizeof(args),
                "{\"op\":\"%s\",\"path\":\"%s/gen/multi_%ld.xlsx\",\"csv_files\":["
                "{\"name\":\"主表\",\"path\":\"%s\"}",
                cr->op, WORK_DIR, (long)time(NULL), prev_file);
            if (prev_sheets[0])
                strncat(args, prev_sheets, sizeof(args) - strlen(args) - 1);
            strncat(args, "]}", sizeof(args) - strlen(args) - 1);
        } else {
            snprintf(args, sizeof(args), "{\"op\":\"%s\",\"limit\":15}", cr->op);
        }

        char *res = pl->run(args);
        if (!res) continue;
        if (strstr(res, "\"error\"")) { free(res); continue; }

        printf("\n【第 %d 步：%s.%s（框架按任务顺序执行）】\n", ++ran, cr->plugin, cr->op);
        printf("  [%s] %s\n\n", cr->plugin, res);

        /* 把产出留给下一步 */
        char err5[128];
        jval *rv = json_parse(res, err5, sizeof(err5));
        if (rv) {
            const char *f = json_str(json_get(rv, "file"));
            if (f && f[0]) snprintf(prev_file, sizeof(prev_file), "%s", f);
            jval *sh = json_get(rv, "sheets");
            if (sh) {
                size_t si = 0;
                prev_sheets[0] = 0;
                for (int i = 0; si + 500 < sizeof(prev_sheets); i++) {
                    jval *it = json_at(sh, i);
                    if (!it) break;
                    const char *nm = json_str(json_get(it, "name"));
                    const char *pt = json_str(json_get(it, "path"));
                    if (!nm || !pt) continue;
                    si += (size_t)snprintf(prev_sheets + si, sizeof(prev_sheets) - si,
                        ",{\"name\":\"%s\",\"path\":\"%s\"}", nm, pt);
                }
            }
            json_free(rv);
        }
        free(res);
    }
    return ran;
}


/* ============ 登录流程直连 ============
 * 任务形如："账号=xxx 密码=yyy 登录 https://x.com/login"
 * 3B 组不出 {"op":"probe",...} 这种参数（实测两次传空 url），
 * 而登录本身是确定性流程：开页面 → 填账号密码 → 点登录 → 拿 cookie。
 * 框架自己做，几秒钟出结果。 */

/* 从任务里抠出账号和密码 */
static int extract_cred(const char *task, char *user, size_t ucap,
                        char *pass, size_t pcap) {
    static const char *ukeys[] = {"账号=", "用户名=", "帐号=", "用户=", NULL};
    static const char *pkeys[] = {"密码=", "口令=", NULL};
    for (int i = 0; ukeys[i]; i++) {
        const char *a = strstr(task, ukeys[i]);
        if (!a) continue;
        a += strlen(ukeys[i]);
        size_t n = 0;
        while (*a && !isspace((unsigned char)*a) && *a != '，' && *a != ',' && n + 1 < ucap) {
            if ((unsigned char)*a < 0x80 || (unsigned char)*a >= 0xe0) {
                /* ASCII 或中文首字节 → 收；中文账号少见，遇到标点就停 */
            }
            user[n++] = *a++;
        }
        user[n] = 0;
        for (int k = 0; pkeys[k]; k++) {
            const char *b = strstr(task, pkeys[k]);
            if (!b) continue;
            b += strlen(pkeys[k]);
            size_t m = 0;
            while (*b && !isspace((unsigned char)*b) && *b != '，' && *b != ',' && m + 1 < pcap)
                pass[m++] = *b++;
            pass[m] = 0;
            if (user[0] && pass[0]) return 1;
        }
    }
    return 0;
}

/* 返回 1 表示已处理（打印了结果），0 表示不是登录任务 */
static int run_login_flow(const char *task) {
    if (!task) return 0;
    if (!(strstr(task, "登录") || strstr(task, "登陆") || strstr(task, "login")))
        return 0;

    const char *u = strstr(task, "http");
    if (!u) return 0;
    char url[512];
    size_t ul = 0;
    while (u[ul] && ul + 1 < sizeof(url)) {
        unsigned char c = (unsigned char)u[ul];
        if (c <= 0x20) break;                          /* 空白 */
        if (c >= 0x80) break;                          /* 中文标点 */
        url[ul++] = (char)c;
    }
    url[ul] = 0;
    /* 去掉尾部可能粘上的标点 */
    while (ul > 0 && strchr(".,;:!?)]}>", url[ul-1])) url[--ul] = 0;
    if (ul < 8) return 0;

    char user[128] = {0}, pass[128] = {0};
    if (!extract_cred(task, user, sizeof(user), pass, sizeof(pass))) return 0;

    plugin_desc_t *bp = find_plugin("browser");
    if (!bp) return 0;

    char args[1400];
    snprintf(args, sizeof(args),
             "{\"op\":\"autologin\",\"url\":\"%s\",\"user\":\"%s\",\"pass\":\"%s\"}",
             url, user, pass);

    printf("\n【框架自动登录（不用模型）】\n  目标 %s\n", url);
    fflush(stdout);
    char *res = bp->run(args);
    if (!res) return 0;
    printf("  [browser] %s\n\n", res);

    /* 失败（验证码认不出 / 没登进去）→ 不截走这次任务，
     * 把诊断信息交给模型判断下一步（模型能看到 next_steps / diagnosis） */
    int failed = (!strstr(res, "\"ok\": true") && !strstr(res, "\"ok\":true"))
                 || strstr(res, "\"captcha_failed\"")
                 || strstr(res, "\"likely_ok\": false")
                 || strstr(res, "\"likely_ok\":false");
    if (failed) {
        /* 把插件返回的诊断（error / next_steps / diagnosis）摘给模型 */
        char jerr[128] = {0};
        jval *j = json_parse(res, jerr, sizeof(jerr));
        const char *emsg = j ? json_str(json_get(j, "error")) : NULL;
        const char *hint = j ? json_str(json_get(j, "hint")) : NULL;
        /* ★ 这里的占位符是 4 个（%s %s %s %.600s），原来只给了 3 个参数，
         *   %.600s 会去读一个不存在的可变参数 —— 登录失败时可能段错误。
         *   括号也顺手改成跟 hint 一起闭合。 */
        snprintf(g_login_diag, sizeof(g_login_diag),
                 "原因：%s%s%s%s\n原始返回：%.600s",
                 emsg ? emsg : "登录没通过",
                 hint ? "（" : "", hint ? hint : "", hint ? "）" : "",
                 res);
        if (j) json_free(j);
    }
    free(res);
    if (failed) {
        printf("【登录没成，交给模型判断】\n\n");
        fflush(stdout);
        return 0;                     /* 继续走模型流程，它能读诊断给建议 */
    }
    return 1;
}

/* 执行联动：返回下游插件的结果（malloc，调用方 free），没有联动返回 NULL */
static char *pipe_followup(const char *tool, const char *out, const char *why) {
    for (int pi = 0; PIPES[pi].from; pi++) {
        if (strcmp(tool, PIPES[pi].from)) continue;
        char perr[128];
        jval *rv = json_parse(out, perr, sizeof(perr));
        if (!rv) return NULL;
        const char *fv = json_str(json_get(rv, PIPES[pi].field));
        const char *filev = json_str(json_get(rv, "file"));
        int hit = fv && PIPES[pi].equals && !strcmp(fv, PIPES[pi].equals)
                  && filev && filev[0];
        /* file 必须复制出来：json_free 之后 filev 就悬空了，
         * 后面拼参数会拿到垃圾（表现为"没有可用的 json 文件"） */
        char filecpy[800] = {0};
        if (hit) snprintf(filecpy, sizeof(filecpy), "%s", filev);
        json_free(rv);
        if (!hit) return NULL;

        plugin_desc_t *nx = find_plugin(PIPES[pi].to);
        if (!nx) return NULL;
        char nargs[1400];
        size_t oi = 0;
        for (const char *tp = PIPES[pi].args; *tp && oi + 2 < sizeof(nargs); tp++) {
            if (tp[0] == '%' && tp[1] == 'f') {
                size_t fl = strlen(filecpy);
                if (oi + fl + 2 >= sizeof(nargs)) break;
                memcpy(nargs + oi, filecpy, fl);
                oi += fl;
                tp++;
            } else nargs[oi++] = *tp;
        }
        nargs[oi] = 0;
        (void)why;
        printf("  ［联动］%s → %s\n", tool, nx->name);
        return nx->run(nargs);
    }
    return NULL;
}

/* ============ main ============ */
/* ---------- 应用题直连 math ----------
 *
 * 工程/行程/鸡兔/环形这类题，math 插件能确定性解（毫秒级），而让 3B 先
 * 判断"该调哪个 op"再算，实测要 140 秒以上，小模型还经常列错式子。
 *
 * 判据直接用 ROUTES 里 math 那条的关键词（框架本来就有，不是新造的词表）。
 * 插件认不出题型就返回 error → 这里原样回落给模型，不影响开放式问题。
 */
static int try_math_direct(const char *task) {
    if (!task || task[0] == '{') return 0;
    size_t L = strlen(task);
    if (L < 8 || L > 320) return 0;

    /* 中文算式（根号 / √ / 平方 / 立方 + 加减乘除）：求值器本来就认这些记号，
     * 直接算，不用等模型翻译 —— 3B 一步 170 秒，而且它经常把算式发成
     * op=solve，被"方程里没有等号"挡回来，重试两次整个任务就废了。
     * 算不出来（含应用题那种叙述）就 return 0 退回模型，不硬猜。 */
    int plain_arith = (strstr(task, "计算") || strstr(task, "算一下") || strstr(task, "算算"))
                    && (strstr(task, "加") || strstr(task, "减") || strstr(task, "乘")
                        || strstr(task, "除"));
    if (strstr(task, "根号") || strstr(task, "√") || strstr(task, "平方")
        || strstr(task, "立方") || strstr(task, "分之") || strstr(task, "次方")
        || strstr(task, "正弦") || strstr(task, "余弦") || strstr(task, "正切")
        || strstr(task, "加到") || plain_arith) {
        plugin_desc_t *mp = find_plugin("math");
        if (mp) {
            char *esc = malloc(L * 6 + 64);
            if (esc) {
                json_escape(task, esc, L * 6 + 64);
                char *call = malloc(L * 6 + 128);
                if (call) {
                    snprintf(call, L * 6 + 128, "{\"op\":\"calc\",\"expr\":\"%s\"}", esc);
                    char *r = mp->run(call);
                    free(call);
                    if (r && strstr(r, "\"value\"") && !strstr(r, "\"error\"")) {
                        /* 必须是 "[math] {json}" 这种格式：前端的步骤卡片
                         * 只认 `  [名字] {`，不打前缀这行会被当成噪音丢掉，
                         * 用户就只看到一个数字、既没有过程也没有插件名。 */
                        printf("【插件 math 开始工作】\n  [math] %s\n", r);
                        char ans[200] = {0};
                        json_str_field(r, "answer", ans, sizeof(ans));
                        if (ans[0]) printf("\n最终回答: %s\n", ans);
                        free(r);
                        free(esc);
                        return 1;
                    }
                    free(r);
                }
                free(esc);
            }
        }
    }

    /* "解方程 3x+5=20" / "求 2x=8 的解"：方程就在文字里，没必要让模型
     * 再翻译一遍（3B 一步 170 秒，还经常漏掉等号被拒）。 */
    if (strstr(task, "方程") || strstr(task, "求解") || strstr(task, "解一下")) {
        char eq[256] = {0};
        size_t el = 0;
        int have_eq_sign = 0, have_var = 0;
        for (const char *p = task; *p && el + 1 < sizeof(eq); ) {
            unsigned char c = (unsigned char)*p;
            if ((c >= '0' && c <= '9') || c == '.' || c == '(' || c == ')' ||
                c == '+' || c == '-' || c == '*' || c == '/' || c == '^' ||
                c == '=' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                eq[el++] = (char)c;
                if (c == '=') have_eq_sign = 1;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) have_var = 1;
                p++;
                continue;
            }
            if (el > 0) break;          /* 算式片段结束 */
            p++;
        }
        eq[el] = 0;
        if (have_eq_sign && have_var && el >= 3) {
            plugin_desc_t *mp = find_plugin("math");
            if (mp) {
                char *esc = malloc(el * 2 + 64);
                if (esc) {
                    json_escape(eq, esc, el * 2 + 64);
                    char *call = malloc(el * 2 + 128);
                    if (call) {
                        snprintf(call, el * 2 + 128, "{\"op\":\"solve\",\"equation\":\"%s\"}", esc);
                        char *r = mp->run(call);
                        free(call);
                        if (r && strstr(r, "\"value\"") && !strstr(r, "\"error\"")) {
                            printf("【插件 math 开始工作】\n  [math] %s\n", r);
                            char ans[200] = {0};
                            json_str_field(r, "answer", ans, sizeof(ans));
                            if (ans[0]) printf("\n最终回答: %s\n", ans);
                            free(r); free(esc);
                            return 1;
                        }
                        free(r);
                    }
                    free(esc);
                }
            }
        }
    }

    static const char *K[] = {
        "工程", "合作", "合做", "相遇", "追及", "环形跑道", "火车", "过桥",
        "鸡兔", "多少次", "平均值", "平均数", "中位数", "方差", "标准差",
        "质因数", "阶乘", "排列", "组合", "斐波那契", "百分", "比例",
        "面积", "周长", "体积", "浓度", "利润", "打折", NULL
    };
    int hit = 0;
    for (int i = 0; K[i]; i++)
        if (strstr(task, K[i])) { hit = 1; break; }
    if (!hit) return 0;

    plugin_desc_t *pl = find_plugin("math");
    if (!pl) return 0;

    char *esc = malloc(strlen(task) * 6 + 16);
    if (!esc) return 0;
    json_escape(task, esc, strlen(task) * 6 + 16);

    char *call = malloc(strlen(esc) + 64);
    if (!call) { free(esc); return 0; }
    snprintf(call, strlen(esc) + 64, "{\"op\":\"plan\",\"task\":\"%s\"}", esc);
    char *res = pl->run(call);
    free(call);
    free(esc);
    if (!res) return 0;
    /* 先判成功再打"开始工作"：失败会退回模型，日志里留一行孤零零的
     * 【插件 math 开始工作】会让人以为插件跑过了。 */
    if (strstr(res, "\"error\"")) { free(res); return 0; }

    printf("【插件 math 开始工作】\n  [math] %s\n", res);
    char ans[200] = {0};
    json_str_field(res, "answer", ans, sizeof(ans));
    if (ans[0]) printf("\n最终回答: %s\n", ans);
    free(res);
    return 1;
}

/* ---------- 生成 BGM 直连 music ----------
 *
 * "生成一段 mid bgm，风格随意" 这类，交给模型是自己给自己找麻烦：3B 每次
 * 编出来的音符都不一样，一段 8 秒 BGM 要调三四次 music，五六分钟还不一定
 * 好听。风格→音阶/节奏/配器 是固定的乐理套路，插件用模板 + 固定种子生成，
 * 秒级出结果。这里只负责认出"这是要 BGM"，风格词交给插件自己解析。
 */
static int try_music_direct(const char *task) {
    if (!task || task[0] == '{') return 0;
    int want = strstr(task, "bgm") || strstr(task, "BGM") || strstr(task, "背景音乐")
            || strstr(task, "配乐") || strstr(task, "一段音乐") || strstr(task, "一段曲子")
            || strstr(task, "来段音乐") || strstr(task, "生成音乐");
    if (!want) return 0;
    if (!(strstr(task, "生成") || strstr(task, "来") || strstr(task, "做")
          || strstr(task, "写") || strstr(task, "给我") || strstr(task, "整")))
        return 0;

    plugin_desc_t *pl = find_plugin("music");
    if (!pl) return 0;

    char *esc = malloc(strlen(task) * 6 + 16);
    if (!esc) return 0;
    json_escape(task, esc, strlen(task) * 6 + 16);
    char *call = malloc(strlen(esc) + 48);
    if (!call) { free(esc); return 0; }
    snprintf(call, strlen(esc) + 48, "{\"op\":\"bgm\",\"task\":\"%s\"}", esc);
    printf("【插件 music 开始工作】\n");
    fflush(stdout);
    char *res = pl->run(call);
    free(call);
    free(esc);
    if (!res) return 0;
    if (strstr(res, "\"error\"")) { free(res); return 0; }
    printf("%s\n", res);
    free(res);
    return 1;
}

/* ---------- "把上面的 csv/表格转成 excel" ----------
 *
 * 这是很常见的一句追问，但它是【新的一次 agent 调用】，进程里没有上一轮
 * 的任何状态。以前会掉给模型：3B 看到"上面的csv"就去猜路径，或者干脆
 * 瞎答 —— 用户体感就是"答非所问"。
 *
 * csv 都是 jq.table / table 插件写出来的，就落在 WORK_DIR/downloads 下，
 * 而且同一批数据会带同样的前缀（resp_xxx_sub_*.csv）。所以：找 30 分钟内
 * 最新的 csv，同前缀的一起作为多 sheet，直接调 doc.xlsx。
 */
static int newest_csv(char *out, size_t cap, char *prefix, size_t pcap) {
    static const char *dirs[] = { WORK_DIR "/downloads", WORK_DIR "/gen", WORK_DIR, NULL };
    time_t best = 0, now = time(NULL);
    out[0] = 0;
    prefix[0] = 0;
    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) != NULL) {
            size_t L = strlen(e->d_name);
            if (L < 4 || strcasecmp(e->d_name + L - 4, ".csv")) continue;
            char full[900];
            snprintf(full, sizeof(full), "%s/%s", dirs[d], e->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            if (now - st.st_mtime > 1800) continue;      /* 超过 30 分钟不要 */
            if (st.st_mtime >= best) {
                best = st.st_mtime;
                snprintf(out, cap, "%s", full);
                /* 前缀：去掉 _sub_xxx.csv 或 .csv 结尾 */
                snprintf(prefix, pcap, "%s", e->d_name);
                char *sub = strstr(prefix, "_sub_");
                if (sub) *sub = 0;
                else {
                    char *dot = strrchr(prefix, '.');
                    if (dot) *dot = 0;
                }
            }
        }
        closedir(dp);
    }
    return out[0] ? 1 : 0;
}

static int try_csv_to_xlsx(const char *task) {
    if (!task || task[0] == '{') return 0;
    int want_x = strstr(task, "excel") || strstr(task, "Excel") || strstr(task, "xlsx")
              || strstr(task, "电子表格");
    int has_csv = strstr(task, "csv") || strstr(task, "CSV") || strstr(task, "表格");
    /* "输出 excel""写成 excel""另存为"都是同一个意思 —— 少一个词就整条快速
     * 通道失效，掉给模型后它会反复造 csv，跑几分钟都停不下来（实测 400 秒+）。 */
    int act = strstr(task, "转") || strstr(task, "导出") || strstr(task, "生成")
           || strstr(task, "做成") || strstr(task, "变成") || strstr(task, "存成")
           || strstr(task, "输出") || strstr(task, "写成") || strstr(task, "另存")
           || strstr(task, "保存") || strstr(task, "做成表格") || strstr(task, "弄成");
    if (!want_x || !has_csv || !act) return 0;

    char csv[900], prefix[300];
    if (!newest_csv(csv, sizeof(csv), prefix, sizeof(prefix))) {
        printf("（没找到 30 分钟内的 csv —— 先把数据做成表格，或者直接把文件路径给我）\n");
        return 0;      /* 回落给模型，让它问清楚 */
    }

    /* 同前缀的兄弟 csv 一起做成多 sheet（jq.table 生成的 resp_xxx_sub_*.csv） */
    char sheets[4096] = {0};
    size_t si = 0;
    const char *dir = WORK_DIR "/downloads";
    DIR *dp = opendir(dir);
    int nsheet = 0;
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) != NULL) {
            if (!strstr(e->d_name, ".csv")) continue;
            if (prefix[0] && !strstr(e->d_name, prefix)) continue;
            char full[900];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;

            /* sheet 名：去掉前缀和 .csv */
            char nm[64];
            snprintf(nm, sizeof(nm), "%s", e->d_name);
            char *dot = strrchr(nm, '.');
            if (dot) *dot = 0;
            if (prefix[0]) {
                char *pp = strstr(nm, prefix);
                if (pp) memmove(nm, pp + strlen(prefix), strlen(pp + strlen(prefix)) + 1);
            }
            while (nm[0] == '_') memmove(nm, nm + 1, strlen(nm));
            /* 净化：文件名里可能出现引号/逗号/斜杠（表头被当文件名时就见过），
             * 直接拼进 JSON 会把 csv_files 结构搞坏 → 生成一堆畸形文件 */
            for (char *q2 = nm; *q2; q2++)
                if (*q2 == '"' || *q2 == '\\' || *q2 == ',' || *q2 == '\n' ||
                    *q2 == '\r' || *q2 == '/' || *q2 == ':')
                    *q2 = '_';
            if (!nm[0]) snprintf(nm, sizeof(nm), "Sheet%d", nsheet + 1);
            if (strlen(nm) > 40) nm[40] = 0;   /* 粗限，别让表头整段进 sheet 名 */

            si += (size_t)snprintf(sheets + si, sizeof(sheets) - si,
                "%s{\"name\":\"%s\",\"path\":\"%s\"}", si ? "," : "", nm, full);
            nsheet++;
            if (si > sizeof(sheets) - 900) break;
        }
        closedir(dp);
    }
    if (!nsheet) {      /* 只有主表 */
        snprintf(sheets, sizeof(sheets), "{\"name\":\"Sheet1\",\"path\":\"%s\"}", csv);
        nsheet = 1;
    }

    plugin_desc_t *pl = find_plugin("doc");
    if (!pl) return 0;
    char outp[900], call[6000];
    mkdir(WORK_DIR "/gen", 0777);
    snprintf(outp, sizeof(outp), "%s/gen/%s.xlsx", WORK_DIR,
             prefix[0] ? prefix : "table");
    snprintf(call, sizeof(call),
        "{\"op\":\"xlsx\",\"path\":\"%s\",\"csv_files\":[%s]}", outp, sheets);
    printf("（直接转：用最新 csv `%s`，共 %d 个 sheet）\n", csv, nsheet);
    printf("【插件 doc 开始工作】\n");
    fflush(stdout);
    char *res = pl->run(call);
    if (!res) return 0;
    if (strstr(res, "\"error\"")) { free(res); return 0; }
    printf("%s\n", res);
    free(res);
    return 1;
}

/* ---------- 写指定字数的文章 ----------
 *
 * 为什么以前永远差很远：主循环的 max_tokens 是写死的 180（纯对话）/240，
 * 跟用户要多少字毫无关系。要 800 字的文章，模型在 180 token 就被截断，
 * 只能给出 200 来字，而且它自己也"不知道"被截断了。
 *
 * 这里单独走一条写作通道：
 *   ① 从任务里抠出目标字数，按 1 字≈1.6 token 放大 max_tokens（留余量）
 *   ② 写完用 C 自己数（中文字/英文词，不含标点），模型数数不可信
 *   ③ 差超过 12% 就再要一轮：把已有正文给它，让它扩写/压缩到目标
 *   ④ 最终答案里如实报"实际 N 字（目标 M 字）"
 */
#define ART_MAXROUND 4

/* 数中文字数：CJK 每字算 1，连续的英文/数字算 1 个词，标点/空白/markdown 不算 */
static int count_chars(const char *t) {
    int n = 0, in_word = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
        if (*p >= 0xE0) {                       /* 3 字节汉字（含全角标点） */
            unsigned cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            /* 全角标点不算字：，。、；：？！“”‘’（）《》—…·【】 */
            if (!((cp >= 0x3000 && cp <= 0x303f) || (cp >= 0xff00 && cp <= 0xff0f) ||
                  (cp >= 0x2010 && cp <= 0x2027) || cp == 0x2026 || cp == 0x2014))
                n++;
            in_word = 0;
            p += 2;
        } else if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            if (!in_word) { n++; in_word = 1; }
        } else {
            in_word = 0;
        }
    }
    return n;
}

/* 从任务里抠"800字""300 字""1500个字" */
static int want_chars(const char *t) {
    for (const char *p = t; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        /* 前面不能紧挨着数字/字母（避免从 "2026" 里抠） */
        if (p > t && (isdigit((unsigned char)p[-1]) || isalpha((unsigned char)p[-1]))) continue;
        long v = atol(p);
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        while (*q == ' ') q++;
        /* ★ 不能用 *q == '字'：'字' 是多字节常量（int），*q 是单字节，
         *   两者永远不相等 —— 之前就是这里写错，目标字数永远抠不出来，
         *   整条写作通道形同虚设。必须按 UTF-8 字节序列比较。 */
        if (!strncmp(q, "字", 3) || !strncmp(q, "个字", 6) || !strncmp(q, "字数", 6)) {
            if (v >= 20 && v <= 20000) return (int)v;
        }
    }
    return 0;
}

static int is_article_task(const char *t) {
    int w = strstr(t, "文章") || strstr(t, "作文") || strstr(t, "短文")
         || strstr(t, "报告") || strstr(t, "文案") || strstr(t, "总结")
         || strstr(t, "演讲稿") || strstr(t, "简介") || strstr(t, "介绍词")
         /* 下面这些之前漏了：用户说"写一篇…教程/指南/说明"，会被当成普通任务，
            走主线 max_tokens=220，模型把正文塞进工具调用的 JSON 里直接被截断，
            页面上就显示半截 {"tool":"doc",...} */
         || strstr(t, "教程") || strstr(t, "指南") || strstr(t, "说明")
         || strstr(t, "文档") || strstr(t, "方案") || strstr(t, "攻略")
         || strstr(t, "手册") || strstr(t, "邮件") || strstr(t, "讲稿")
         || strstr(t, "长文") || strstr(t, "解析") || strstr(t, "评测");
    int a = strstr(t, "写") || strstr(t, "生成") || strstr(t, "来一篇")
         || strstr(t, "帮我写") || strstr(t, "起草") || strstr(t, "拟");
    if (w && a) return 1;
    /* 兜底：只要点名了字数（>=100），就当写作任务 —— 名词怎么叫都无所谓。
       "写一篇…500字左右" 这种说法名词千奇百怪，靠关键词穷举永远漏。 */
    if (a && want_chars(t) >= 100) return 1;
    return 0;
}

/* ---------- 长文分段生成（含 topup 补写）----------
 *
 * 800 字一次性生成实测跑 35 分钟还没出结果：max_tokens 1640 × 1.5 tok/s
 * 就要 18 分钟，而且模型经常写不到目标又跑一轮 —— 翻倍。分段做：
 *   每段 300 字左右，独立请求，前一段的结尾作为下一段的上下文。
 * 好处：① 每次只生成 300 来 token，2~3 分钟返回，永远碰不到超时
 *       ② 结构更清楚（每段有明确主题）
 *       ③ 某一段失败只损失那一段，不用整篇重来
 */
static void jesc(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (const char *p = in; *p && k + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[k++] = '\\'; out[k++] = *p; }
        else if (*p == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (*p == '\r') { out[k++] = '\\'; out[k++] = 'r'; }
        else out[k++] = *p;
    }
    out[k] = 0;
}

static char *write_by_sections(const char *task, int target, const char *host,
                               int port, const char *model, int *out_chars) {
    int per = 300;
    int nsec = (target + per - 1) / per;
    if (nsec < 2) nsec = 2;
    if (nsec > 12) nsec = 12;
    int each = target / nsec;
    if (each < 120) each = 120;

    printf("（长文分段：%d 段，每段约 %d 字）\n", nsec, each);

    char *all = calloc(1, 40000);
    if (!all) return NULL;
    size_t used = 0;
    char tail[400] = {0};          /* 上一段结尾，给下一段接续 */

    /* outline_first 先要一份提纲：模型对着"某个要点"写，比对着"第 2/3 部分"写要充实得多。
     * 实测只给"约 266 字"它只写 85 字；给了明确要点才能写够。 */
    char outline[1600] = {0};
    {
        char te[6000], body[BIG];
        jesc(task, te, sizeof(te));
        snprintf(body, BIG,
            "{\"model\":\"%s\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"只输出提纲，每行一个要点，"
            "每行不超过 12 个字，不要编号不要解释，共 %d 行。\"},"
            "{\"role\":\"user\",\"content\":\"%s\"}],"
            "\"max_tokens\":220,\"temperature\":0.5,\"stream\":false}",
            model, nsec, te);
        char *r = http_post_chat(host, port, body, 300);
        if (r) {
            char *c = extract_content(r);
            free(r);
            if (c) snprintf(outline, sizeof(outline), "%s", c);
            free(c);
            if (outline[0]) printf("（提纲：%.80s…）\n", outline);
        }
    }

    for (int i = 1; i <= nsec; i++) {
        char te[6000], tl[1200], body[BIG];
        jesc(task, te, sizeof(te));
        jesc(tail, tl, sizeof(tl));

        /* 从提纲里取第 i 行作为本段要点 */
        char point[200] = {0};
        {
            int ln = 0;
            const char *q = outline;
            while (*q) {
                /* 模型常把提纲挤成一行用分号连起来，换行/；/。 都当分隔 */
                size_t L = strcspn(q, "\n");
                size_t s1 = strcspn(q, "；");
                size_t s2 = strcspn(q, ";");
                size_t s3 = strcspn(q, "。");
                if (s1 < L) L = s1;
                if (s2 < L) L = s2;
                if (s3 < L) L = s3;
                const char *e = q + L;
                if (!*e) e = NULL;
                if (L) {
                    ln++;
                    if (ln == i) {
                        size_t k = 0;
                        while (k < L && k + 1 < sizeof(point)) {
                            char c = q[k];
                            if (c == '*' || c == '#' || c == '-' || c == ' ')
                                { k++; continue; }
                            break;
                        }
                        size_t m = 0;
                        for (; k < L && m + 1 < sizeof(point); k++, m++) point[m] = q[k];
                        point[m] = 0;
                        break;
                    }
                }
                if (!e) break;
                q = e + 1;
            }
        }

        char pte[900] = {0};
        if (point[0]) jesc(point, pte, sizeof(pte));

        /* 硬性下限：3B 对"约 N 字"不敏感，实测要 266 只给 85；
         * 写"不少于 N 字，分 2~3 个自然段，每段 3~5 句"才有效。 */
        int floor_chars = each * 8 / 10;

        /* 一条 user 消息里把"任务 + 本段要求"写完。
         * 之前拆成两条 user，Qwen 的模板下会返回空内容（实测两段全空）。 */
        char plain[1200], pie[1800];
        if (i == 1)
            snprintf(plain, sizeof(plain),
                "%s\n\n请写第 1/%d 段，主题：%s。约 %d 字（不少于 %d 字），"
                "分 2~3 个自然段，每段 3~5 句，要有具体的地名、时间、数据。",
                task, nsec, point[0] ? point : "开头", each, floor_chars);
        else
            snprintf(plain, sizeof(plain),
                "%s\n\n接着上面写第 %d/%d 段，主题：%s。约 %d 字（不少于 %d 字），"
                "分 2~3 个自然段，不要重复前面的内容%s。",
                task, i, nsec, point[0] ? point : "继续", each, floor_chars,
                (i == nsec ? "，这一段做个收尾" : ""));
        jesc(plain, pie, sizeof(pie));

        if (i == 1)
            snprintf(body, BIG,
                "{\"model\":\"%s\",\"messages\":["
                "{\"role\":\"system\",\"content\":\"你是中文写作助手。直接输出正文，"
                "不要标题、不要解释、不要标注字数。\"},"
                "{\"role\":\"user\",\"content\":\"%s\"}],"
                "\"max_tokens\":%d,\"temperature\":0.7,\"stream\":false}",
                model, pie, each * 3 + 200);
        else
            snprintf(body, BIG,
                "{\"model\":\"%s\",\"messages\":["
                "{\"role\":\"system\",\"content\":\"你是中文写作助手。直接输出正文，"
                "不要标题、不要解释、不要标注字数。\"},"
                "{\"role\":\"assistant\",\"content\":\"…%s\"},"
                "{\"role\":\"user\",\"content\":\"%s\"}],"
                "\"max_tokens\":%d,\"temperature\":0.7,\"stream\":false}",
                model, tl, pie, each * 3 + 200);

        int to = each * 3 + 300;
        if (to < 240) to = 240;
        if (to > 900) to = 900;
        char *resp = http_post_chat(host, port, body, to);
        if (!resp) { printf("（第 %d 段超时/断连，跳过）\n", i); continue; }
        char *c = extract_content(resp);
        free(resp);
        if (!c) {
            printf("（第 %d 段返回空，跳过）\n", i);
            if (getenv("ART_DEBUG")) printf("  [body] %.500s\n", body);
            continue;
        }

        int n = count_chars(c);
        printf("（第 %d/%d 段：%d 字）\n", i, nsec, n);
        if (used + strlen(c) + 4 < 40000) {
            memcpy(all + used, c, strlen(c));
            used += strlen(c);
            all[used++] = '\n';
            all[used] = 0;
        }
        /* 记住结尾 200 字给下一段接续 */
        size_t L = strlen(c);
        size_t take = L > 200 ? 200 : L;
        const char *p = c + (L - take);
        while (take > 0 && ((unsigned char)*p & 0xC0) == 0x80) { p++; take--; }
        snprintf(tail, sizeof(tail), "%s", p);
        free(c);
    }

    *out_chars = count_chars(all);

    /* 3B 每段都会写短（要 300 只给 160~250）。收尾补一段，别让用户拿到
     * 一篇明显不够数的文章。只补一次，补不动就如实报字数。 */
    /* 写超了也要收：模型常把 150 字写成 240 字，两段就超 28%。
     * 超过目标 15% 就压一次 —— 比"补写"便宜（输出更短）。 */
    if (*out_chars > target + target / 6) {
        char te2[6000], ae[24000], body[BIG];
        jesc(task, te2, sizeof(te2));
        jesc(all, ae, sizeof(ae));
        snprintf(body, BIG,
            "{\"model\":\"%s\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"你是中文编辑。只输出压缩后的正文，"
            "不要解释、不要标注字数。\"},"
            "{\"role\":\"user\",\"content\":\"把下面这段压缩到 %d 字左右"
            "（不超过 %d 字），保留全部关键事实和结构，删掉重复和铺陈，"
            "直接输出压缩后的完整正文：\\n\\n%s\"}],"
            "\"max_tokens\":%d,\"temperature\":0.3,\"stream\":false}",
            model, target, target + target / 10, ae, target * 3 + 200);
        char *r = http_post_chat(host, port, body, target * 2 + 300);
        if (r) {
            char *c = extract_content(r);
            free(r);
            if (c) {
                int n = count_chars(c);
                if (n > 0 && n < *out_chars) {
                    printf("（压缩：%d 字 → %d 字）\n", *out_chars, n);
                    snprintf(all, 40000, "%s", c);
                    *out_chars = n;
                }
                free(c);
            }
        }
    }

    for (int tp = 0; tp < 2; tp++) {
    int lack = target - *out_chars;
    if (lack <= target / 8) break;
    {
        char te[6000], tl[1200], body[BIG];
        jesc(task, te, sizeof(te));
        jesc(tail, tl, sizeof(tl));
        int want = lack;
        if (want > 400) want = 400;
        snprintf(body, BIG,
            "{\"model\":\"%s\",\"messages\":["
            "{\"role\":\"system\",\"content\":\"你是中文写作助手。直接输出正文，"
            "不要标题、不要解释、不要标注字数。\"},"
            "{\"role\":\"user\",\"content\":\"%s\"},"
            "{\"role\":\"assistant\",\"content\":\"…%s\"},"
            "{\"role\":\"user\",\"content\":\"接着上面再写一段，主题换成这条：%s，"
            "写 %d 字（不少于 %d 字），分 2 个自然段，要有具体例子。\"}],"
            "\"max_tokens\":%d,\"temperature\":0.7,\"stream\":false}",
            model, te, tl, te, want, want * 8 / 10, want * 3 + 150);
        int to = want * 3 + 200;
        if (to > 900) to = 900;
        char *r = http_post_chat(host, port, body, to);
        if (r) {
            char *c = extract_content(r);
            free(r);
            if (c) {
                int n = count_chars(c);
                printf("（补写：%d 字）\n", n);
                if (used + strlen(c) + 4 < 40000) {
                    memcpy(all + used, c, strlen(c));
                    used += strlen(c);
                    all[used++] = '\n';
                    all[used] = 0;
                }
                *out_chars = count_chars(all);
                free(c);
            }
        }
    }
    }
    return all;
}

static int try_write_article(const char *task, const char *host, int port,
                             const char *model) {
    if (!task || task[0] == '{') return 0;
    if (!is_article_task(task)) return 0;

    int target = want_chars(task);
    if (target <= 0) return 0;          /* 没写字数就按普通对话走 */

    /* 统一走分段。为什么不保留"单次生成 + 多轮补足"：
     * 那条路最坏情况是一次请求生成 7000 token（1.5 tok/s ≈ 78 分钟），
     * 只能靠超时兜底，是不可控的。分段每次只生成约 300 字，
     * 单次请求 2~3 分钟必然返回，超时风险从"可能"变成"不可能"。
     * 实测：600 字 → 613 字（+2%），比单次路线更准也更稳。 */
    int got = 0;
    char *txt = write_by_sections(task, target, host, port, model, &got);
    if (!txt) return 0;                 /* 全失败 → 回落正常对话，不硬扛 */

    printf("\n最终回答: %s\n", txt);
    printf("\n【字数】实际 %d 字 / 目标 %d 字（偏差 %d%%）\n",
           got, target, target ? (int)((got - target) * 100 / target) : 0);
    free(txt);
    return 1;
}

/* ---------- 插件已给出答案就直接收 ----------
 *
 * 自然语言任务走两段：① 模型选插件 ② 模型把插件返回的 JSON 转成人话。
 * 第②段纯浪费 —— 我们的插件本来就返回 answer 字段（中文一句话），
 * 模型只是把它重说一遍。实测同一个抓包任务：第①段 62 秒，第②段 189 秒，
 * 而第②段产出的那句话跟插件的 answer 一模一样。
 *
 * 所以：插件返回 ok + answer 时，直接把结果给用户，不再问模型。
 * 只对"自成一体"的插件这么做 —— browser/batch/curl 常常是链条中的一环
 * （抓回来还要接着分析），跳过后面的步骤就断了。
 *
 * 这里只判断、不打印：调用方要先把插件原始结果（卡片）打出去，再打
 * "最终回答"。顺序反了的话，前端"最终回答"的收集器会把后面所有行都并进
 * 答案气泡，用户看到的就是一坨 {"value":"..."} 原文（math 解题就是）。
 */
static int direct_answer_of(const char *tool, const char *out, int step,
                            char *ans, size_t cap) {
    if (!g_direct_answer) return 0;
    if (step > 1) return 0;                  /* 多步任务里不抢 */
    if (!out || strstr(out, "\"error\"")) return 0;

    static const char *SELF[] = {"netcap", "sys", "owrt", "music", "math",
                                 "doc", "jq", "table", "sysmon", NULL};
    int ok = 0;
    for (int i = 0; SELF[i]; i++)
        if (!strcmp(tool, SELF[i])) { ok = 1; break; }
    if (!ok) return 0;

    /* 必须有 answer 且够像一句话（太短的"ok"不算答案） */
    ans[0] = 0;
    json_str_field(out, "answer", ans, cap);
    if (strlen(ans) < 6) return 0;
    if (!strstr(out, "\"ok\"") && !strstr(out, "\"value\"")) return 0;
    return 1;
}

/* ---------- JSON → 多 sheet xlsx 直连 ----------
 * jq 的 table op 会把 JSON 里每个子数组导成一份 *_sub_*.csv（article_brand、
 * statistics_data…），doc 的 xlsx 支持 csv_files 一个文件一个 sheet。
 * 但让 3B 自己串这两步基本串不对，所以这里直连：
 *   任务里同时提到 xlsx/Excel 和 json/子表/sheet，且 downloads 里有子表 CSV，
 *   就直接把它们合成一个多 sheet 的 xlsx。 */
static int try_json_xlsx_direct(const char *task) {
    if (!task || !task[0]) return 0;
    int want_xlsx = strstr(task, "xlsx") || strstr(task, "XLSX") || strstr(task, "Excel")
                 || strstr(task, "excel") || strstr(task, "工作表文件");
    int want_json = strstr(task, "json") || strstr(task, "JSON") || strstr(task, "子表")
                 || strstr(task, "sheet") || strstr(task, "Sheet") || strstr(task, "工作表");
    int act = strstr(task, "导出") || strstr(task, "生成") || strstr(task, "另存")
           || strstr(task, "转成") || strstr(task, "做成") || strstr(task, "保存");
    if (!(want_xlsx && want_json && act)) return 0;

    DIR *dp = opendir("/tmp/llama_agent/downloads");
    if (!dp) return 0;
    typedef struct { char path[900]; char name[64]; time_t mt; } cf_t;
    cf_t cf[16];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL && n < 15) {
        const char *base = e->d_name;
        if (strncmp(base, "resp_", 5) && strncmp(base, "jq_", 3)) continue;
        size_t L = strlen(base);
        if (L < 5 || strcasecmp(base + L - 4, ".csv")) continue;
        char full[900];
        snprintf(full, sizeof(full), "/tmp/llama_agent/downloads/%s", base);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        /* 只要最近 2 小时内导出的，避免拿上一次的旧数据 */
        if (time(NULL) - st.st_mtime > 7200) continue;
        snprintf(cf[n].path, sizeof(cf[n].path), "%s", full);
        const char *sub = strstr(base, "_sub_");
        if (sub) snprintf(cf[n].name, sizeof(cf[n].name), "%.*s", 31, sub + 5);
        else snprintf(cf[n].name, sizeof(cf[n].name), "主表");
        char *dot = strrchr(cf[n].name, '.');
        if (dot) *dot = 0;
        cf[n].mt = st.st_mtime;
        n++;
    }
    closedir(dp);
    if (n == 0) return 0;

    plugin_desc_t *dpl = find_plugin("doc");
    if (!dpl) return 0;

    char out[512];
    snprintf(out, sizeof(out), "/tmp/llama_agent/gen/json_%ld.xlsx", (long)time(NULL));
    size_t cap = 2048 + (size_t)n * 1100;
    char *call = malloc(cap);
    if (!call) return 0;
    size_t o = (size_t)snprintf(call, cap, "{\"op\":\"xlsx\",\"path\":\"%s\",\"csv_files\":[", out);
    for (int i = 0; i < n && o + 1200 < cap; i++)
        o += (size_t)snprintf(call + o, cap - o, "%s{\"path\":\"%s\",\"name\":\"%s\"}",
                              i ? "," : "", cf[i].path, cf[i].name);
    snprintf(call + o, cap - o, "]}");

    char *r = dpl->run(call);
    free(call);
    if (!r) return 0;
    if (strstr(r, "\"error\"")) { free(r); return 0; }
    printf("【插件 doc 开始工作】\n  [doc] %s\n", r);
    char ans[200] = {0};
    json_str_field(r, "answer", ans, sizeof(ans));
    if (ans[0]) printf("\n最终回答: %s\n", ans);
    free(r);
    return 1;
}

/* ---------- 文字转语音直连 ----------
 * "念：xxx" / "朗读 xxx" / "把这段读出来：xxx" —— 直接合成，不用等模型。
 * 3B 模型去调 voice 插件也行，但这一步是纯机械的，没必要烧几十秒推理。 */
static int try_tts_direct(const char *task) {
    if (!task || !task[0]) return 0;
    static const char *LEAD[] = {"用男声念", "用男声朗读", "男声念", "男声朗读", "用男声说",
                                 "朗读一下", "朗读", "念一下", "念出来", "念", "读出来",
                                 "读一下", "用语音说", "转成语音", "语音播报", "配音",
                                 "合成语音", "文字转语音", NULL};
    const char *txt = NULL;
    for (int i = 0; LEAD[i]; i++) {
        size_t L = strlen(LEAD[i]);
        if (strncmp(task, LEAD[i], L)) continue;
        const char *q = task + L;
        while (*q == ' ' || *q == '：' || *q == ':' || *q == '，' || *q == ',') q += (*q == '：' ? 3 : 1);
        if (*q) { txt = q; break; }
    }
    if (!txt) return 0;
    /* "用男声念：…" / "男声朗读 …" → 走 kokoro 男声（sid 60）。
     * 正文里把这些前缀词逐个剥掉，剩下的才是要念的内容。 */
    int want_male = (strstr(task, "男声") || strstr(task, "男音") || strstr(task, "male") != NULL);
    static char buf2[2400];
    snprintf(buf2, sizeof(buf2), "%s", txt);
    if (want_male) {
        static const char *DROP[] = {"请", "帮我", "用", "以", "男声", "男音", "male", "Male",
                                     "念", "朗读", "读", "说", "的", "声音", "语音",
                                     "：", ":", "，", ",", "、", " ", "\t", NULL};
        int changed = 1, guard = 0;
        while (changed && guard++ < 60) {
            changed = 0;
            for (int k = 0; DROP[k]; k++) {
                size_t L = strlen(DROP[k]);
                if (!strncmp(buf2, DROP[k], L)) {
                    memmove(buf2, buf2 + L, strlen(buf2 + L) + 1);
                    changed = 1;
                }
            }
        }
        /* 行尾残留的冒号也去掉 */
        for (size_t n2 = strlen(buf2); n2 > 0; n2--) {
            char c = buf2[n2 - 1];
            if (c == ' ' || c == '\t' || c == '\n' || c == ':' || c == ',') buf2[n2 - 1] = 0;
            else break;
        }
        if (buf2[0]) txt = buf2;
    }
    plugin_desc_t *pl = find_plugin("voice");
    if (!pl) return 0;

    /* 拼 JSON（正文里的引号/换行要转义） */
    size_t cap = strlen(txt) * 2 + 256;
    char *req = malloc(cap);
    if (!req) return 0;
    size_t o = (size_t)snprintf(req, cap, "{\"op\":\"tts\"%s,\"text\":\"",
                                want_male ? ",\"voice\":\"male\"" : "");
    for (const char *p = txt; *p && o + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { req[o++] = '\\'; req[o++] = (char)c; }
        else if (c == '\n') { req[o++] = '\\'; req[o++] = 'n'; }
        else if (c >= 0x20) req[o++] = (char)c;
    }
    snprintf(req + o, cap - o, "\"}");

    char *r = pl->run(req);
    free(req);
    if (!r) return 0;
    if (strstr(r, "\"error\"")) {
        printf("【插件 voice 开始工作】\n  [voice] %s\n", r);
        char m[300] = {0};
        json_str_field(r, "error", m, sizeof(m));
        if (m[0]) printf("\n最终回答: %s\n", m);
        free(r);
        return 1;
    }
    printf("【插件 voice 开始工作】\n  [voice] %s\n", r);
    char a[300] = {0};
    json_str_field(r, "answer", a, sizeof(a));
    if (a[0]) printf("\n最终回答: %s\n", a);
    free(r);
    return 1;
}

/* ---------- 五线谱图片 → MIDI 直连 ----------
 * 任务里同时出现"五线谱/乐谱/曲谱"和"midi/bgm/生成"时直接走 music 的 omr。
 * 不直连的话，3B 会把它当"要一段背景音乐"去调 op=bgm（风格模板随机生成），
 * 跟用户想的"照着这张谱子弹"完全不是一回事。 */
static int try_sheet_direct(const char *task) {
    if (!task || !task[0]) return 0;
    int sheet = strstr(task, "五线谱") || strstr(task, "乐谱") || strstr(task, "曲谱")
             || strstr(task, "谱面") || strstr(task, "简谱");
    int act = strstr(task, "midi") || strstr(task, "MIDI") || strstr(task, "bgm")
           || strstr(task, "BGM") || strstr(task, "生成") || strstr(task, "转成")
           || strstr(task, "识别") || strstr(task, "扒");
    if (!(sheet && act)) return 0;
    plugin_desc_t *pl = find_plugin("music");
    if (!pl) return 0;
    char *r = pl->run("{\"op\":\"omr\"}");     /* 不给 path：插件自动用最近上传的图片 */
    if (!r) return 0;
    if (strstr(r, "\"error\"")) {
        printf("【插件 music 开始工作】\n  [music] %s\n", r);
        char m[400] = {0};
        json_str_field(r, "error", m, sizeof(m));
        if (m[0]) printf("\n最终回答: %s\n", m);
        free(r);
        return 1;
    }
    printf("【插件 music 开始工作】\n  [music] %s\n", r);
    char a[400] = {0};
    json_str_field(r, "answer", a, sizeof(a));
    if (a[0]) printf("\n最终回答: %s\n", a);
    free(r);
    return 1;
}

/* ---------- 表格 TOP-N 直连 ----------
 *
 * "展示表格内 播放量 top10" 让模型去做，3B 只会回一句"直接转"或者干脆
 * 调 doc 把 xlsx 又打包一遍。这类请求的要素只有两个：按哪一列、取前几条 ——
 * 列名在任务里写着，条数也在任务里写着，不需要模型判断。
 */
static int top_n(const char *t) {
    /* top10 / TOP 10 / 前10 / 前 10 名 / 最高的 10 条 */
    for (const char *p = t; *p; p++) {
        const char *q = NULL;
        if (!strncasecmp(p, "top", 3)) q = p + 3;      /* top10 / TOP 10 */
        else if (!strncmp(p, "前", 3)) q = p + 3;      /* 前10 / 前 10 名 */
        if (!q) continue;
        while (*q == ' ') q++;
        if (*q < '0' || *q > '9') continue;
        int v = atoi(q);
        if (v >= 1 && v <= 200) return v;
    }
    return 0;
}

static int bottom_n(const char *t) {
    /* 倒数10 / 最低 10 / 最少5 / bottom 10 */
    for (const char *p = t; *p; p++) {
        const char *q = NULL;
        if (!strncmp(p, "倒数", 6)) q = p + 6;
        else if (!strncmp(p, "最低", 6) || !strncmp(p, "最少", 6)) q = p + 6;
        else if (!strncasecmp(p, "bottom", 6)) q = p + 6;
        else continue;
        while (*q == ' ' || !strncmp(q, "的", 3)) q += (*q == ' ') ? 1 : 3;
        if (*q < '0' || *q > '9') {
            /* "最低的" 后面没数字：当成取 10 条 */
            return 10;
        }
        int v = atoi(q);
        if (v >= 1 && v <= 200) return v;
    }
    return 0;
}

static int try_table_direct(const char *task) {
    if (!task || !task[0] || task[0] == '{') return 0;
    int n = top_n(task);
    int asc = 0;
    /* top/bottom 路径必须认到列才算表格任务：任务里写着列名（"播放量 top10"）
     * 才走 table，认不到列让 table 自己报错退回模型 —— 否则"中国前10大城市"
     * 也会被当成表格请求，随便拿一份表排一遍。 */
    int need_col = 1;
    if (n <= 0) {
        int b = bottom_n(task);
        if (b > 0) { n = b; asc = 1; }
    }
    if (n <= 0) {
        /* 没有 top/bottom：要么带筛选词，要么明确"展示表格"才接 */
        int tab_hint = strstr(task, "表格") || strstr(task, "表里") || strstr(task, "csv")
                    || strstr(task, "CSV") || strstr(task, "xlsx") || strstr(task, "excel")
                    || strstr(task, "Excel")
                    || strstr(task, "这个表") || strstr(task, "这张表")
                    || strstr(task, "该表") || strstr(task, "表中")
                    || strstr(task, "表有") || strstr(task, "表共");
        int filt = strstr(task, "筛选") || strstr(task, "满足") || strstr(task, "条件")
                || strstr(task, "大于") || strstr(task, "小于") || strstr(task, "包含")
                || strstr(task, "等于") || strstr(task, "不含") || strstr(task, "低于")
                || strstr(task, "高于");
        int show_hint = strstr(task, "展示") || strstr(task, "显示") || strstr(task, "列出")
                     || strstr(task, "看看") || strstr(task, "看下") || strstr(task, "读取")
                     || strstr(task, "内容") || strstr(task, "前几行") || strstr(task, "预览")
                     /* "这个表有多少条记录/多少行" 也是看表，别让它掉进纯对话 */
                     || strstr(task, "多少条") || strstr(task, "多少行")
                     || strstr(task, "几条") || strstr(task, "几行")
                     || strstr(task, "行数") || strstr(task, "记录数")
                     || strstr(task, "哪些字段") || strstr(task, "什么字段")
                     || strstr(task, "哪些列") || strstr(task, "什么列")
                     || strstr(task, "列名");
        /* 光有"等于/大于/包含"不算表格任务 —— "1加1等于几"里也有"等于"。
         * 必须同时带表格语境（表格/表里/csv/xlsx），或者明确说"筛选/满足条件"。 */
        if ((filt && tab_hint) || strstr(task, "筛选") || strstr(task, "满足条件")
            || (tab_hint && show_hint)) { n = 20; need_col = 0; }
    }
    if (n <= 0) return 0;

    plugin_desc_t *tp = find_plugin("table");
    if (!tp) return 0;

    size_t L = strlen(task);
    char *esc = malloc(L * 6 + 64);
    if (!esc) return 0;
    json_escape(task, esc, L * 6 + 64);
    char *call = malloc(L * 6 + 320);
    if (!call) { free(esc); return 0; }
    snprintf(call, L * 6 + 300,
             "{\"op\":\"top\",\"limit\":%d,\"order\":\"%s\",\"need_col\":%d,\"_task\":\"%s\"}",
             n, asc ? "asc" : "desc", need_col, esc);
    char *r = tp->run(call);
    free(call);
    free(esc);
    if (!r) return 0;

    if (strstr(r, "\"error\"")) {
        /* 数据/列名层面的问题（"没认出要按哪一列排""表里没有这一列"）不适合
         * 再丢给模型 —— 3B 只会重新推理几分钟然后给个错答案。直接把原因
         * 摆在气泡里，用户照着改一句话就行。 */
        int datamsg = strstr(r, "没认出要按哪一列") || strstr(r, "里没有「")
                   || strstr(r, "没有可用的表格数据") || strstr(r, "只有表头");
        if (datamsg) {
            printf("【插件 table 开始工作】\n  [table] %s\n", r);
            char m[400] = {0};
            json_str_field(r, "error", m, sizeof(m));
            if (m[0]) printf("\n最终回答: %s\n", m);
            free(r);
            return 1;
        }
        free(r);
        return 0;
    }
    if (!strstr(r, "\"value\"")) { free(r); return 0; }

    printf("【插件 table 开始工作】\n  [table] %s\n", r);
    char ans[200] = {0};
    json_str_field(r, "answer", ans, sizeof(ans));
    if (ans[0]) printf("\n最终回答: %s\n", ans);
    free(r);
    return 1;
}

/* ---------- 生成文档（PPT / Word / PDF）直连 ----------
 *
 * "根据 wireguard 技术写一个 10 页 PPT" 交给主循环必挂：模型要在一次
 * JSON 里塞下十页正文，240 token 直接截断；而 3B 往往干脆漏掉 path，
 * doc 落到读分支报"缺少 path"，重试两次就停 —— 用户看到的就是
 * "插件 doc 连续失败两次"。
 *
 * 这里改成：框架按页数分段问模型要 markdown（request 不带 JSON 约束），
 * 再把内容交给 doc 写文件。模型只负责写字，路径/格式/落盘由框架管。
 */
static int doc_pages(const char *t) {
    for (const char *p = t; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        if (p > t && (isdigit((unsigned char)p[-1]) || isalpha((unsigned char)p[-1]))) continue;
        long v = atol(p);
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        while (*q == ' ') q++;
        if ((!strncmp(q, "页", 3) || !strncmp(q, "张", 3)) && v >= 1 && v <= 60)
            return (int)v;
    }
    return 0;
}

static char *gen_doc_md(const char *topic, int parts, int ppt, int per_part,
                        int chars_each, const char *host, int port, const char *model) {
    char *all = calloc(1, 40000);
    if (!all) return NULL;
    size_t used = 0;

    for (int from = 1; from <= parts; from += per_part) {
        int to = from + per_part - 1;
        if (to > parts) to = parts;
        int n = to - from + 1;

        char te[6000], tv[20000], body[BIG];
        jesc(topic, te, sizeof(te));
        /* 只把最后 8KB 当续写上下文：既省 token，也保证截断落在字符边界上 */
        {
            char ctx[12000];
            size_t al = used > 8000 ? 8000 : used;
            memcpy(ctx, all + used - al, al);
            ctx[al] = 0;
            utf8_cut(ctx, 8000);
            jesc(ctx, tv, sizeof(tv));
        }

        /* 系统提示里不放引号：这段是直接拼进 JSON 字符串的，少一层转义少一个坑 */
        const char *sysp = ppt
            ? "你是文档撰写助手。只输出 Markdown：每页以一行「# 标题」开始，"
              "标题写实质内容，绝对不要写「第1页」这种占位标题；"
              "标题下面列 3 到 5 条「- 要点」，要点必须具体（术语、数字、步骤、配置项）。"
              "第 1 页是封面，标题就是主题本身。"
              "不要目录、不要解释、不要任何前后缀。"
            : "你是文档撰写助手。只输出 Markdown 正文：用「## 小标题」分节，"
              "小标题写实质内容，每节一个完整段落，不要目录、不要解释、不要标注字数。";

        if (all[0])
            snprintf(body, BIG,
                "{\"model\":\"%s\",\"messages\":["
                "{\"role\":\"system\",\"content\":\"%s\"},"
                "{\"role\":\"user\",\"content\":\"主题：%s。共 %d %s，接着写第 %d-%d %s。\"},"
                "{\"role\":\"assistant\",\"content\":\"%s\"},"
                "{\"role\":\"user\",\"content\":\"继续写第 %d-%d %s，格式完全一致，"
                "不要重复前面写过的内容。\"}],"
                "\"max_tokens\":%d,\"temperature\":0.6,\"stream\":false}",
                model, sysp, te, parts, ppt ? "页" : "节", from - per_part, from - 1,
                ppt ? "页" : "节", tv, from, to, ppt ? "页" : "节",
                n * (ppt ? 110 : chars_each) + 160);
        else
            snprintf(body, BIG,
                "{\"model\":\"%s\",\"messages\":["
                "{\"role\":\"system\",\"content\":\"%s\"},"
                "{\"role\":\"user\",\"content\":\"主题：%s。共 %d %s，先写第 %d-%d %s。\"}],"
                "\"max_tokens\":%d,\"temperature\":0.6,\"stream\":false}",
                model, sysp, te, parts, ppt ? "页" : "节", from, to,
                ppt ? "页" : "节", n * (ppt ? 110 : chars_each) + 160);

        printf("（生成第 %d-%d/%d 个…）\n", from, to, parts);
        fflush(stdout);
        char *resp = http_post_chat(host, port, body, 420);
        if (!resp) { printf("（这一段超时/断连，跳过）\n"); continue; }
        char *c = extract_content(resp);
        free(resp);
        if (!c || !c[0]) { free(c); printf("（这一段返回空，跳过）\n"); continue; }
        size_t L = strlen(c);
        if (used + L + 4 < 40000) {
            memcpy(all + used, c, L);
            used += L;
            all[used++] = '\n';
            all[used] = 0;
        }
        free(c);
    }
    if (!all[0]) { free(all); return NULL; }
    return all;
}

static int try_write_doc(const char *task, const char *host, int port, const char *model) {
    if (!task || !task[0] || task[0] == '{' || task[0] == '/') return 0;

    int ppt  = strstr(task, "ppt") || strstr(task, "PPT") || strstr(task, "幻灯片")
            || strstr(task, "演示文稿") || strstr(task, "演示稿");
    int word = strstr(task, "word") || strstr(task, "Word") || strstr(task, "WORD")
            || strstr(task, "docx");
    int pdf  = strstr(task, "pdf") || strstr(task, "PDF");
    if (!ppt && !word && !pdf) return 0;

    /* 只接"从零写一份"；带数据源的（表格/附件/文件路径）交给 doc 的读改写链 */
    int act = strstr(task, "写") || strstr(task, "做") || strstr(task, "生成")
           || strstr(task, "制作") || strstr(task, "整");
    if (!act) return 0;
    if (strstr(task, "表格") || strstr(task, "csv") || strstr(task, "CSV")
        || strstr(task, "附件") || strstr(task, "上传") || strstr(task, "uploads/")
        || strstr(task, ".xlsx") || strstr(task, ".docx") || strstr(task, ".csv"))
        return 0;
    /* "把这段文字/以下内容写成 pdf" 用的是现成文字，不是从零写 —— 这种
     * 交给模型走 doc 写操作，别让这条通道再编一份新的出来。 */
    if (strstr(task, "这段") || strstr(task, "以上") || strstr(task, "以下")
        || strstr(task, "如下") || strstr(task, "上面") || strstr(task, "翻译"))
        return 0;
    if (strstr(task, ".mp4") || strstr(task, ".mkv") || strstr(task, ".mov")) return 0;

    plugin_desc_t *pl = find_plugin("doc");
    if (!pl) return 0;

    /* PPT：一页一屏；Word/PDF：按节写，要了字数就按字数换算节数 */
    int chars = want_chars(task);
    int parts, per_part, chars_each = 300;
    if (ppt) {
        parts = doc_pages(task);
        if (parts <= 0) parts = 8;
        per_part = 4;
    } else {
        parts = doc_pages(task);
        if (parts > 0) per_part = 2;
        else if (chars > 0) { parts = (chars + 299) / 300; if (parts < 2) parts = 2; per_part = 2; }
        else { parts = 4; per_part = 2; }
        if (parts > 12) parts = 12;
    }

    printf("（文档生成：%s，%d 个%s）\n", ppt ? "PPT" : (pdf ? "PDF" : "Word"),
           parts, ppt ? "页面" : "小节");
    char *md = gen_doc_md(task, parts, ppt, per_part, chars_each, host, port, model);
    if (!md) { printf("（模型没给出内容，改走正常流程）\n"); return 0; }

    char outdir[256], outp[512];
    snprintf(outdir, sizeof(outdir), "%s/gen", WORK_DIR);
    mkdir(outdir, 0777);
    const char *ext = ppt ? "pptx" : (pdf ? "pdf" : "docx");
    snprintf(outp, sizeof(outp), "%s/%s_%ld.%s", outdir,
             ppt ? "slides" : (pdf ? "doc" : "doc"), (long)time(NULL), ext);

    size_t mcap = strlen(md) * 2 + strlen(outp) * 2 + 512;
    char *me = malloc(mcap);
    char *call = malloc(mcap + 256);
    if (!me || !call) { free(me); free(call); free(md); return 0; }
    jesc(md, me, mcap);
    free(md);
    snprintf(call, mcap + 256,
        "{\"op\":\"%s\",\"path\":\"%s\",\"markdown\":\"%s\"}", ext, outp, me);
    free(me);

    printf("【插件 doc 开始工作】\n");
    fflush(stdout);
    char *res = pl->run(call);
    free(call);
    if (!res) return 0;
    if (strstr(res, "\"error\"")) {
        printf("%s\n", res);
        free(res);
        return 1;               /* 已经给用户明确错误，别再让模型瞎猜一遍 */
    }
    printf("%s\n", res);
    free(res);
    return 1;
}

/* ---------- 视频类自然语言直连 ----------
 *
 * "把 X.mp4 转成黑白视频" 这种句式，3B 实测会把 .mp4 发给 doc（报
 * "扩展名 .mp4 与操作不符（应为 .xlsx）"），或者把路径编成 /opt/video/01.mp4。
 * 任务里已经写明了文件路径和动作，没必要让模型猜 —— 直接派发。
 *
 * 只处理"路径 + 明确动作词"的组合，认不出就回落给模型（不硬猜）。
 */
static void jesc_video(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (const char *p = in; *p && k + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[k++] = '\\'; out[k++] = *p; }
        else if (*p == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else out[k++] = *p;
    }
    out[k] = 0;
}

static int find_video_path(const char *task, char *out, size_t cap) {
    static const char *exts[] = {".mp4", ".mkv", ".mov", ".m4v", ".avi", ".flv", ".ts", ".webm", NULL};
    /* 从扩展名往回找路径起点：文件名里可能有中文（三国演义01.mp4），
     * 所以不能"遇到非 ASCII 就截断"，只能靠空格/引号/标点分界。 */
    for (const char *p = task; *p; p++) {
        for (int i = 0; exts[i]; i++) {
            size_t le = strlen(exts[i]);
            if (strncasecmp(p, exts[i], le) != 0) continue;
            /* p 指向扩展名开头，往回走到分界 */
            const char *b = p;
            while (b > task) {
                unsigned char c = (unsigned char)b[-1];
                if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' ||
                    c == '(' || c == ')' || c == '[' || c == ']' || c == ',' || c == ';')
                    break;
                b--;
            }
            size_t L = (size_t)(p + le - b);
            if (L < 6 || L >= 700) continue;
            if (*b != '/') continue;            /* 必须以 / 开头才算路径 */
            char cand[700];
            memcpy(cand, b, L);
            cand[L] = 0;
            struct stat st;
            if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(out, cap, "%s", cand);
                return 1;
            }
        }
    }
    return 0;
}

static int try_video_direct(const char *task) {
    if (!task || task[0] == '{') return 0;
    char path[700];
    if (!find_video_path(task, path, sizeof(path))) return 0;

    plugin_desc_t *pl = find_plugin("video");
    if (!pl) return 0;

    char op[24] = {0}, mode[16] = "fill";   /* 默认用背景色覆盖：不动画面位置，比 crop 的拉伸安全 */
    if (strstr(task, "竖屏") || strstr(task, "垂直")) {
        snprintf(op, sizeof(op), "portrait");
        if (strstr(task, "模糊") || strstr(task, "blur")) snprintf(mode, sizeof(mode), "blur");
        else if (strstr(task, "黑边") || strstr(task, "留边")) snprintf(mode, sizeof(mode), "pad");
    } else if (strstr(task, "去字幕") || strstr(task, "去除字幕") ||
               strstr(task, "去掉字幕") || strstr(task, "擦掉字幕") ||
               strstr(task, "字幕")) {
        snprintf(op, sizeof(op), "desub");
    } else if (strstr(task, "超分") || strstr(task, "画面优化") || strstr(task, "提升画质") ||
               strstr(task, "变清晰") || strstr(task, "清晰") || strstr(task, "清晰度") ||
               strstr(task, "放大")) {
        snprintf(op, sizeof(op), "sr");
    } else if (strstr(task, "黑白") || strstr(task, "灰度") || strstr(task, "单色")) {
        snprintf(op, sizeof(op), "effects");
    } else if (strstr(task, "压缩") || strstr(task, "变小") || strstr(task, "体积")) {
        snprintf(op, sizeof(op), "compress");
    } else if (strstr(task, "信息") || strstr(task, "分辨率") || strstr(task, "多长")) {
        snprintf(op, sizeof(op), "video_info");
    } else if (strstr(task, "抽帧") || strstr(task, "截一帧") || strstr(task, "缩略")) {
        snprintf(op, sizeof(op), "thumb");
    } else if (strstr(task, "gif") || strstr(task, "GIF")) {
        snprintf(op, sizeof(op), "gif");
    } else {
        return 0;                       /* 动作不明确，交给模型问清楚 */
    }

    char esc[1400], call[1800];
    jesc_video(task, esc, sizeof(esc));
    if (!strcmp(op, "portrait"))
        snprintf(call, sizeof(call),
            "{\"op\":\"portrait\",\"path\":\"%s\",\"mode\":\"%s\",\"async\":1}", path, mode);
    else
        /* effects 把原话带上，插件自己解析"黑白/淡入/变速"这些词 */
        snprintf(call, sizeof(call),
            "{\"op\":\"%s\",\"path\":\"%s\",\"_task\":\"%s\"}", op, path, esc);

    printf("（确定性命中：%s → video.%s）\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, op);
    printf("【插件 video 开始工作】\n");
    fflush(stdout);
    char *res = pl->run(call);
    if (!res) return 0;
    if (strstr(res, "\"error\"")) { free(res); return 0; }   /* 失败就让模型接手 */
    printf("%s\n", res);
    free(res);
    return 1;
}

int main(int argc, char **argv) {
    /* AGENT_DIRECT_ANSWER=0 关掉"插件给答案就直接收" */
    { const char *da = getenv("AGENT_DIRECT_ANSWER");
      if (da && !strcmp(da, "0")) g_direct_answer = 0; }
    const char *task = NULL;
    const char *model = "qwen2.5-3b-q4_k_m.gguf";
    const char *host = "127.0.0.1";
    const char *plugdir = "/opt/llama/agent/plugins";
    int port = 8090, max_steps = 4, env_only = 0, auto_list = 0, daemon_mode = 0;
    int auto_state_only = 0;
    const char *auto_name = NULL;
    const char *auto_stop_name = NULL;
    const char *auto_args = "{}";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--task") && i + 1 < argc)          task = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)     model = argv[++i];
        else if (!strcmp(argv[i], "--plugins") && i + 1 < argc)   plugdir = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)      port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-steps") && i + 1 < argc) max_steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host") && i + 1 < argc)     host = argv[++i];
        else if (!strcmp(argv[i], "--env")) env_only = 1;
        else if (!strcmp(argv[i], "--auto-list")) auto_list = 1;
        else if (!strcmp(argv[i], "--auto-state")) auto_state_only = 1;
        else if (!strcmp(argv[i], "--auto") && i + 1 < argc) auto_name = argv[++i];
        else if (!strcmp(argv[i], "--auto-stop") && i + 1 < argc) auto_stop_name = argv[++i];
        else if (!strcmp(argv[i], "--daemon")) daemon_mode = 1;
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) auto_args = argv[++i];
        else if (!strcmp(argv[i], "--memcheck")) g_memcheck = 1;
        else task = argv[i];
    }
    if (!task && !env_only && !auto_list && !auto_name && !auto_stop_name && !auto_state_only) {
        fprintf(stderr,
            "用法:\n"
            "  agent-c --task '任务' [--max-steps N] [--plugins DIR]   跑一次 agent 任务\n"
            "  agent-c --env                                          环境 + 插件清单\n"
            "  agent-c --auto-list                                    列出自动化能力\n"
            "  agent-c --auto NAME [--args '{...}'] [--daemon]        启动一个自动化流程\n"
            "  agent-c --auto-stop NAME                               停止正在运行的自动化\n");
        return 1;
    }

    read_plugoff();            /* 用户在页面上停用的插件名单 */
    reload_plugins(plugdir);   /* 每次启动重扫：新增/删除 .so 都生效，不用改框架 */

    /* 任务本身就是一条插件命令（{"op":"probe",...}）→ 直接执行，不惊动模型。
     * 3B 模型不认识这种输入，会当普通文本乱答。 */
    if (task && try_direct_plugin(task)) return 0;
    /* 意图判断只看用户自己写的那段（附件/上文提示块里有"转成纯文本"、
     * 文件名里的 .xlsx 之类，会把下面这些直连通道误触发）。 */
    const char *itask = intent_text(task);
    if (itask && try_math_direct(itask)) return 0;
    if (itask && try_tts_direct(itask)) return 0;
    if (itask && try_sheet_direct(itask)) return 0;
    if (itask && try_table_direct(itask)) return 0;
    if (itask && try_json_xlsx_direct(itask)) return 0;
    if (itask && try_music_direct(itask)) return 0;
    /* 文档生成要早于"写文章"：写 word/ppt 的诉求是拿到文件，不是一段正文 */
    if (itask && try_write_doc(itask, host, port, model)) return 0;
    if (itask && try_write_article(itask, host, port, model)) return 0;
    if (itask && try_csv_to_xlsx(itask)) return 0;
    if (task && try_video_direct(task)) return 0;
    if (auto_state_only) {
        /* 高频轮询专用：不探测环境、不输出描述，只回状态，几十字节。
         * 页面打开面板时每 2 秒拉一次，用不着重建整个面板。 */
        printf("{\"auto\":[");
        for (int i = 0; i < g_nauto; i++) {
            pid_t apid = 0;
            int running = auto_state(g_auto[i]->name, &apid);
            printf("%s{\"name\":\"%s\",\"running\":%d,\"pid\":%d}",
                   i ? "," : "", g_auto[i]->name,
                   running ? 1 : 0, running ? (int)apid : 0);
        }
        printf("]}\n");
        return 0;
    }

    if (auto_list) {
        printf("{\"auto\":[");
        for (int i = 0; i < g_nauto; i++) {
            /* params 本身是 JSON 片段，里面的引号必须转义，否则外层 JSON 就坏了 */
            char pe[512] = {0};
            size_t pi = 0;
            const char *ps = g_auto[i]->params ? g_auto[i]->params : "{}";
            for (const char *q = ps; *q && pi + 3 < sizeof(pe); q++) {
                if (*q == '"') { pe[pi++] = '\\'; pe[pi++] = '"'; }
                else pe[pi++] = *q;
            }
            pid_t apid = 0;
            int running = auto_state(g_auto[i]->name, &apid);
            printf("%s{\"name\":\"%s\",\"desc\":\"%s\",\"triggers\":\"%s\",\"params\":\"%s\","
                   "\"running\":%d,\"pid\":%d}",
                   i ? "," : "", g_auto[i]->name, g_auto[i]->description,
                   g_auto[i]->triggers ? g_auto[i]->triggers : "manual", pe,
                   running ? 1 : 0, running ? (int)apid : 0);
        }
        printf("]}\n");
        return 0;
    }

    if (auto_stop_name) {
        pid_t apid = 0;
        if (!auto_state(auto_stop_name, &apid)) {
            printf("{\"ok\":1,\"stopped\":0,\"note\":\"%s 当前没有在运行\"}\n", auto_stop_name);
            return 0;
        }
        if (kill(apid, SIGTERM) == 0) {
            /* 等它自己清理退出（最多 3 秒），插件收到 SIGTERM 会结束当前轮次 */
            for (int w = 0; w < 30; w++) {
                usleep(100000);
                if (kill(apid, 0) != 0) break;
            }
            auto_pid_clear(auto_stop_name);
            printf("{\"ok\":1,\"stopped\":1,\"pid\":%d}\n", (int)apid);
        } else {
            printf("{\"ok\":0,\"error\":\"发信号失败\"}\n");
        }
        return 0;
    }

    if (auto_name) {
        plugin_auto_t *a = find_auto(auto_name);
        if (!a) {
            printf("{\"error\":\"没有自动化能力: %s\"}\n", auto_name);
            return 1;
        }
        if (!a->start) {
            printf("{\"error\":\"%s 没有实现 start\"}\n", auto_name);
            return 1;
        }
        /* 已经在跑就别再起一个：否则新进程覆盖 pid 文件，
         * 老进程没人管，成了孤儿还在后台转。 */
        pid_t old = 0;
        if (auto_state(auto_name, &old)) {
            printf("{\"ok\":1,\"already\":1,\"running\":1,\"pid\":%d,"
                   "\"answer\":\"%s 已经在运行（pid %d）\"}\n",
                   (int)old, auto_name, (int)old);
            return 0;
        }
        /* 告诉插件这一次是"前台一次性跑"还是"后台守护"：
         * 一次性跑的插件应该自己收敛（比如巡检只跑几轮就结束），
         * 守护的才允许一直循环。用环境变量传，不污染参数 JSON。 */
        if (!daemon_mode) setenv("AGENT_AUTO_FG", "1", 1);
        else unsetenv("AGENT_AUTO_FG");

        /* 守护模式：把 pid 落盘，页面才知道在跑、才停得掉。
         * 非守护（一次性）也写，这样运行期间页面能显示"运行中"。 */
        auto_pid_write(auto_name);
        char *r = a->start(auto_args);
        auto_pid_clear(auto_name);
        printf("%s\n", r ? r : "{\"error\":\"start 返回空\"}");
        free(r);
        return 0;
    }

    if (env_only) {
        /* 10 个插件的描述加起来三四千字，4096 会把 JSON 截断 */
        char envbuf[16384];
        /* 缓存逻辑在 env_json 内部（只缓存库探测结果，插件状态永远实时）。
         * 这里不要再套一层缓存，否则页面拿到的是旧的 running 状态。 */
        env_json(envbuf, sizeof(envbuf));
        printf("%s\n", envbuf);
        return 0;
    }
    if (g_n == 0) {
        fprintf(stderr, "没有加载到插件，检查目录 %s\n", plugdir);
        return 1;
    }

    /* task 转义提前做：预执行（curl）和 prompt 都要用 */
    size_t tcap = strlen(task) * 6 + 16;
    char *task_esc = malloc(tcap);
    if (!task_esc) return 1;
    json_escape(task, task_esc, tcap);

    /* ===== 预处理：用户直接粘了一条 curl 命令 =====
     * 不让模型转述：1.7KB 的 curl 原文（十几条 -H、长 cookie、各种引号）
     * 一是每步都要重读一遍，3B 在 A55 上一步两分钟；二是模型转述必然走样。
     * 必须放在 tools_prompt 之前 —— 跑成功就置 g_prefetched，
     * 路由随后只带 curl/sys，不再把 doc/math 之类塞给模型去"取数据"。 */
    char *pre_result = NULL;
    if (g_n > 0 && strstr(task, "curl ") && strstr(task, "http")) {
        plugin_desc_t *cp = find_plugin("curl");
        if (cp) {
            size_t rcap = strlen(task) * 2 + 256;
            char *req = malloc(rcap);
            if (req) {
                snprintf(req, rcap, "{\"op\":\"send\",\"curl\":\"%s\"}", task_esc);
                printf("\n【框架直接执行了粘贴的 curl，不让模型转述】\n");
                pre_result = cp->run(req);
                if (pre_result) g_prefetched = 1;
                free(req);
            }
        }
    }

    /* ===== 预处理 2：按任务内容分块执行 =====
     * "输出表格，再转成 xlsx" 拆成两步：jq 出表格 → doc 打包 xlsx，
     * 上一步的 CSV 自动喂给下一步。以前这里是一个写死的 if（表格+xlsx），
     * 后一句常被忽略，加第三个动作还得改代码。 */
    /* 登录任务：框架直连（比模型两轮快得多，也不怕 3B 组不出参数） */
    if (!g_prefetched && g_n > 0) {
        if (run_login_flow(itask)) { free(task_esc); return 0; }
    }

    if (!g_prefetched && g_n > 0 && strlen(task) < 120) {
        int nran = run_task_chunks(itask);
        if (nran > 0) {
            free(task_esc);
            return 0;
        }
    }

    char *tools = tools_prompt(task);
    if (!tools) { free(task_esc); free(pre_result); return 1; }

    /* system prompt */
    char sys_raw[BIG];
    if (g_pure_chat_task) {
        /* 纯对话：不给工具表、不给 op 示例，否则它会挑一个工具去调 */
        snprintf(sys_raw, sizeof(sys_raw),
            "你是运行在路由器上的本地 AI 助手。用户问的是知识性 / 解释性问题，"
            "**不需要任何工具**，直接凭你的知识回答。\n"
            "回答用中文，简洁准确，不超过 200 字。\n"
            "只输出一个 JSON：{\"tool\":\"final\",\"answer\":\"你的回答\"}。"
            "answer 里就是最终答案本身，不要提工具、不要提 JSON。");
    } else
    snprintf(sys_raw, sizeof(sys_raw),
        "你只能输出一个 JSON 对象，不要解释、不要代码块。"
        "**每个 JSON 都必须有 tool 字段**（比如 {\"tool\":\"doc\",\"op\":\"xlsx\",...}），"
        "漏了 tool 就等于没调用。\n%s"
        /* 别写 \"插件名\" 这种占位符 —— 3B 会原样照抄，输出 {"tool":"插件名",参数} */
        "调用格式：{\"tool\":\"sys\",\"op\":\"list\",\"path\":\"/tmp\"}"
        "（tool 用上面列表里的名字）；答完：{\"tool\":\"final\",\"answer\":\"...\"}\n"
        "【op 归属表】op 放在哪个插件是固定的，别放错：\n"
        "  sys    : shell list du grep read write sysinfo http_get download\n"
        "  owrt   : uci_get uci_set uci_show uci_commit uci_revert iwinfo iw "
        "service logread passwd reboot opkg_list\n"
        "  net    : ping port resolve route arp stat link wifi trace hostname\n"
        "  math   : calc solve system stat num（应用题给 task 让插件自己判题型）\n"
        "  doc    : docx xlsx pptx pdf text（Office/PDF 读写）\n"
        "  jq     : probe get query csv table keys（JSON）\n"
        "  table  : sort（文本表格排序）\n"
        "  browser: probe autologin hunt drag captcha cookie shot\n"
        "  music  : midi render import\n"
        "  curl   : parse（吃浏览器 Copy as cURL 的整段原文，不是网址）\n"
        "注意：日志是 owrt 的 logread，不是 sys；OpenWrt 上没有 /var/log/syslog 这个文件。\n"
        "【示例】\n"
        "用户：写个 shell 脚本统计日志里 404 的数量\n"
        "→ {\"tool\":\"sys\",\"op\":\"write\",\"path\":\"/tmp/count404.sh\","
        "\"content\":\"#!/bin/sh\\nlogread | grep -c -- 404\\n\"}\n"
        "  然后 → {\"tool\":\"sys\",\"op\":\"shell\",\"cmd\":\"sh /tmp/count404.sh\"}\n"
        "用户：看看最近 20 行日志\n"
        "→ {\"tool\":\"owrt\",\"op\":\"logread\",\"lines\":20}\n"
        "用户：把 /opt/mp4/01.mp4 转成竖屏\n"
        "→ {\"tool\":\"video\",\"op\":\"portrait\",\"path\":\"/opt/mp4/01.mp4\"}\n"
        "用户：把这个视频的字幕去掉（保留画面）\n"
        "→ {\"tool\":\"video\",\"op\":\"desub\",\"path\":\"/opt/mp4/01.mp4\"}\n"
        "用户：把视频转成黑白\n"
        "→ {\"tool\":\"video\",\"op\":\"effects\",\"path\":\"/opt/mp4/01.mp4\",\"grayscale\":1}\n"
        "用户：抓5秒流量，看看访问了什么\n"
        "→ {\"tool\":\"netcap\",\"op\":\"urls\",\"secs\":5}\n"
        "用户：看看 /tmp 下最大的文件\n"
        "→ {\"tool\":\"sys\",\"op\":\"du\",\"path\":\"/tmp\",\"top\":10}\n"
        "用户：抓 http://x/y 看看是不是 404\n"
        "→ {\"tool\":\"sys\",\"op\":\"http_get\",\"url\":\"http://x/y\"}\n"
        "凡是算式、方程、方程组、统计、数论题，一律调用 math 插件，不要心算。\n"
        "应用题（工程量、行程、相遇追及）把原题文字直接传给 math（只给 task，不要自己指定 op，"
        "也不要自己列式子）——插件会自己判断题型；列错式子算得再准也是错答案。\n",
        tools);
    size_t syscap = strlen(sys_raw) * 6 + 16;
    char *sys_esc = malloc(syscap);
    if (!sys_esc) { free(tools); return 1; }
    json_escape(sys_raw, sys_esc, syscap);

    /* 对话记录：任务 + 每步结果 */
    char *log = malloc(BIG);
    if (!log) { free(sys_esc); free(tools); free(task_esc); free(pre_result); return 1; }
    char *last_result_init = NULL;      /* 预执行（curl）的结果，交给兜底收尾用 */

    if (pre_result) {
        char *cut = strdup(pre_result);
        if (cut) {
            utf8_cut(cut, 1200);       /* 3B 扛不住长上下文：只给摘要 */
            const char *cs = strstr(task, "curl ");
            size_t pre = cs ? (size_t)(cs - task) : 0;
            while (pre > 0 && (task[pre - 1] == ' ' || task[pre - 1] == '\n'
                               || task[pre - 1] == '\\'))
                pre--;
            if (pre > 200) pre = 200;
            if (pre > 3) {
                /* 用户除了 curl 还写了要求（"帮我找出前 5 条标题"）→ 保留他的话，走模型 */
                snprintf(log, BIG,
                    "任务：%.*s\n\n"
                    "【数据已经抓到了，直接用下面这份，不要再调工具去取】\n\n%s\n\n"
                    "要更多字段用 curl 的 op=jq 分批取；不要再发一次请求，"
                    "也不要把 URL 当文件路径去读。\n",
                    (int)pre, task, cut);
            } else {
                /* 只粘了一条 curl，没有别的指令 —— 用户要的就是
                 * "把数据取回来、整理好、能下载"。这活框架自己就干完了，
                 * 不需要模型（3B 只会把数据再"整理"一遍，白烧几分钟）。 */
                /* 必须带 [curl] 前缀：前端靠这个标记把结果渲染成表格和
                 * 代码块；直接打 JSON 原文，用户看到的就是一串转义字符。 */
                /* 前端拿完整的（要能 JSON.parse 才能渲染表格），
                 * 模型拿截断的（context 宝贵） */
                printf("  [curl] %s\n\n", pre_result);
                /* 抓到的是 JSON → 顺手让 jq 报一下结构（多少条、哪些字段） */
                {
                    char *nxo = pipe_followup("curl", pre_result, "prefetch");
                    if (nxo) {
                        char cut2[BIG];
                        snprintf(cut2, sizeof(cut2), "%s", nxo);
                        utf8_cut(cut2, 3000);
                        printf("  [jq] %s\n\n", cut2);
                        free(nxo);
                    }
                }
                free(cut);
                free(pre_result);
                free(log);
                free(sys_esc);
                free(tools);
                free(task_esc);
                return 0;
            }
            printf("  [curl] %s\n\n", pre_result);
            {
                char *nxo = pipe_followup("curl", pre_result, "prefetch");
                if (nxo) {
                    char cut2[BIG];
                    snprintf(cut2, sizeof(cut2), "%s", nxo);
                    utf8_cut(cut2, 3000);
                    printf("  [jq] %s\n\n", cut2);
                    free(nxo);
                }
            }
            free(cut);
            last_result_init = pre_result;    /* 所有权转走 */
            pre_result = NULL;
        }
    } else {
        snprintf(log, BIG, "任务：%s\n", task_esc);
    }
    /* 只在任务明确指代"上面那个文件"时才注入最近文件列表。
     * 无条件注入的话，模型看到一串 /tmp/xxx.log 会自作主张去列目录
     * （实测问"NPU 是什么"它先调了 sys list /tmp）。 */
    if (task && (strstr(task, "上面") || strstr(task, "上文") || strstr(task, "刚才")
                 || strstr(task, "那个文件") || strstr(task, "这个文件")
                 || strstr(task, "上面的") || strstr(task, "之前")))
        inject_recent_files(log, BIG);
    /* task_esc 不在这里释放：下面调 math 时要用它补 task 字段 */

    printf("任务: %s\n插件: %d 个\n", intent_text(task), g_n);

    int rc = 2;
    /* 插件返回的数值：模型 final 时如果没体现出来，由框架补一行。
     * 3B 经常把表达式原样当答案（问"根号3加2"就答 "sqrt(3)+2"），
     * 插件明明算出了 3.732 却不展示，用户会以为算错了。 */
    char last_val[64] = {0};
    char *last_result = last_result_init;   /* 最后一次插件返回的 JSON，兜底收尾用 */
    char last_ok_tool[64] = {0};            /* 产出 last_result 的那个插件 */
    /* 任务里出现网址 + "抓/看/404/状态码" 时，直接把该调的 JSON 贴给模型抄。
     * 0.5B/3B 自己选 op 很不可靠：实测会去调 curl（要整段 cURL，不是网址）
     * 或者 sys 的 list（列出 /tmp 目录），最后胡答一通。给它一条现成的调用
     * 最省事，也最不容易跑偏。 */
    if (task && (strstr(task, "http://") || strstr(task, "https://"))
        && (strstr(task, "404") || strstr(task, "状态码") || strstr(task, "抓")
            || strstr(task, "访问") || strstr(task, "打开") || strstr(task, "页面"))) {
        const char *u = strstr(task, "http");
        char url[600];
        size_t k = 0;
        while (u && *u && k + 1 < sizeof(url)
               && *u != ' ' && *u != '\n' && *u != '\t' && *u != '"' && *u != '\''
               && *u != '，' && *u != '。')
            url[k++] = *u++;
        url[k] = 0;
        if (k > 8) {
            char hint[800];
            snprintf(hint, sizeof(hint),
                     "（框架提示：任务里有网址 %s。要取状态码和页面内容请直接调用 "
                     "{\"op\":\"http_get\",\"url\":\"%s\"}，"
                     "不要用 curl（它要整段 cURL 命令），也不要用 list/du 看目录。）\n",
                     url, url);
            log_append(log, BIG, "%s", hint, "");
        }
    }

    /* 步数上限不是硬墙：超过 max_steps 后，只要还在推进（没触发熔断）、
     * 也没超出时间预算，就自动续跑。用户的原话是"4 步不够就把结果存下来
     * 继续跑"，而不是"4 步一到就报未完成"。 */
    time_t t_start = time(NULL);
    long budget = 900;                     /* 默认 15 分钟 */
    {
        const char *eb = getenv("AGENT_BUDGET_SEC");
        if (eb && atol(eb) > 0) budget = atol(eb);
    }
    int hard_cap = max_steps * 2;
    if (hard_cap < 8) hard_cap = 8;
    if (hard_cap > 16) hard_cap = 16;

    int last_step = 0;
    for (int step = 1; step <= hard_cap; step++) {
        last_step = step;
        if (step > max_steps) {
            long used = (long)(time(NULL) - t_start);
            if (used >= budget) {
                printf("\n⚠ 已跑 %ld 秒，超过时间预算（%ld 秒），停止续跑\n", used, budget);
                break;
            }
            /* 只提示一次，别每步都刷一长串 */
            if (step == max_steps + 1)
                printf("（已到设定 %d 步，自动续跑：最多 %d 步 / %ld 秒）\n",
                       max_steps, hard_cap, budget);
            printf("── 第 %d 步 ──\n", step);
        } else {
            printf("── 第 %d 步 ──\n", step);
        }
        MEMCHK("step-begin");

        size_t lcap = strlen(log) * 6 + 16;
        char *log_esc = malloc(lcap);
        if (!log_esc) break;
        json_escape(log, log_esc, lcap);

        time_t t0 = time(NULL);
        /* 一次性请求（非流式）。流式实测会让 llama-server 卡在写阻塞：
         * 唯一 slot 被占死、4 核满载不出结果，后面所有请求排队。
         * 前端有"运行中 XX 秒"的计时，能看出还在跑。 */
        printf("【模型 推理第 %d 步】\n", step);
        fflush(stdout);

        /* 发请求 + 截断重试。
         * 关键：撞到 max_tokens（finish_reason=length）时，拿到的内容是残缺的
         * —— 工具调用的 JSON 会被从中间劈开。以前这里不看 finish_reason，
         * 直接把这半截当结果往下走，前端只好把 {"tool":"doc","op":"write",
         * "content":"# ...  原样显示出来。现在一律丢弃 + 预算翻 4 倍重试。 */
        char *content = NULL;
        char fin[24] = {0};
        int tok = is_pure_chat(itask) ? 180 : 240;
        for (int att = 0; att < 3; att++) {
            char *body = malloc(BIG);
            if (!body) { free(log_esc); log_esc = NULL; break; }
            snprintf(body, BIG,
                "{\"model\":\"%s\",\"messages\":["
                "{\"role\":\"system\",\"content\":\"%s\"},"
                "{\"role\":\"user\",\"content\":\"%s\"}],"
                "\"max_tokens\":%d,\"temperature\":0.3,\"stream\":false}",
                model, sys_esc, log_esc, tok);
            if (getenv("AGENT_DEBUG")) {
                fprintf(stderr, "  [body] %.400s\n", body);
                fflush(stderr);
            }
            char *resp = http_post_chat(host, port, body, 300);
            free(body);
            fin[0] = 0;
            content = resp ? extract_content(resp) : NULL;
            if (resp) {
                extract_finish(resp, fin, sizeof(fin));
                free(resp);
            }
            if (content && !strcmp(fin, "length")) {
                printf("\n⚠ 输出撞到长度上限（%d token）被截断 —— 这段是残缺的，已丢弃并加倍重试\n", tok);
                free(content);
                content = NULL;
                if (tok >= 4000) break;      /* 到顶了，别无限翻 */
                tok *= 4;
                continue;
            }
            break;
        }
        free(log_esc);
        log_esc = NULL;
        if (!content) {
            if (!strcmp(fin, "length")) {
                printf("\n⚠ 连续 3 次都被长度上限截断。这个任务一次写不完："
                       "可以说「分几段写」或把目标字数降到 300 以内。\n");
                break;
            }
            fprintf(stderr, "llama-server 无有效响应（%s:%d）\n", host, port);
            printf("\n⚠ 模型服务没响应（%s:%d）——不是任务本身失败。"
                   "多半是 llama-server 正在加载模型（装包/重启后要 10~30 秒），"
                   "等它 health 返回 ok 再重试。\n", host, port);
            break;
        }
        if (!content[0]) {
            fprintf(stderr, "响应无 content（%.0f 秒）\n", difftime(time(NULL), t0));
            free(content);
            break;
        }
        /* 流式已经逐字打过了，这里只补一行耗时标记，不要再重复整段 */
        printf("\n模型(%.0fs) 生成完毕，共 %zu 字节\n", difftime(time(NULL), t0), strlen(content));
        if (getenv("AGENT_DEBUG")) {
            fprintf(stderr, "  [content] %.400s\n", content);
            fflush(stderr);
        }

        char err2[128];
        jval *reply = json_parse(content, err2, sizeof(err2));
        /* 注意空字符串：小模型会写出 tool:"" 这种，json_str 返回非 NULL 的 "" */
        const char *ck = reply ? json_str(json_get(reply, "tool")) : NULL;
        if (!ck || !ck[0]) {
            /* 小模型把 JSON 当字符串写了一次，会多出反斜杠 / 多余引号：
             *   {"limit\":3\"}  ／  {"limit":3"}
             * 两步链式修：先去掉不该有的反斜杠，再去掉数字后面的多余引号。 */
            char *s1 = malloc(strlen(content) + 8);
            char *s2 = malloc(strlen(content) + 8);
            if (s1 && s2) {
                size_t fi = 0;
                for (const char *q = content; *q; q++) {
                    if (q[0] == '\\' && q[1] == '"') {
                        char n = q[2];
                        if (n == ':' || n == ',' || n == '}' || n == ']' || n == 0
                            || n == '\n' || n == '\r' || n == ' ') {
                            s1[fi++] = '"';
                            q++;
                            continue;
                        }
                    }
                    s1[fi++] = *q;
                }
                s1[fi] = 0;

                fi = 0;
                for (const char *q = s1; *q; q++) {
                    if (*q == '"' && q > s1) {
                        char prev = q[-1];
                        char next = q[1];
                        /* 只看数字结尾：之前加了 prev=='e' 是为了科学计数法，
                         * 结果把 ".article_title" 的收尾引号也删了（e 结尾） */
                        if (isdigit((unsigned char)prev)
                            && (next == ',' || next == '}' || next == ']'
                                || next == '\n' || next == 0)) {
                            continue;
                        }
                    }
                    s2[fi++] = *q;
                }
                s2[fi] = 0;

                jval *r1 = json_parse(s1, err2, sizeof(err2));
                if (r1 && json_str(json_get(r1, "tool")) && json_str(json_get(r1, "tool"))[0]) {
                    fprintf(stderr, "  （JSON 多余反斜杠已修）\n");
                    reply = r1;
                } else {
                    jval *r2 = json_parse(s2, err2, sizeof(err2));
                    if (r2 && json_str(json_get(r2, "tool")) && json_str(json_get(r2, "tool"))[0]) {
                        fprintf(stderr, "  （JSON 多余反斜杠+引号已修）\n");
                        reply = r2;
                    }
                }
            }
            free(s1);
            free(s2);
        }
        const char *toolp = json_str(json_get(reply, "tool"));
        char tool[64] = {0};
        if (toolp) snprintf(tool, sizeof(tool), "%s", toolp);
        if (!toolp) {
            /* 模型偶尔漏掉 tool 字段，只写 {"op":"xlsx",...}。
             * 直接把整段 JSON 当最终回答等于白干 —— 按 op 猜一次插件。 */
            const char *opv = json_str(json_get(reply, "op"));
            if (opv && *opv) {
                static const char *docops[] = {
                    "text", "xlsx", "xlsx_edit", "xlsx_read",
                    "docx", "docx_append", "pptx", "pdf", NULL
                };
                for (int di = 0; docops[di]; di++) {
                    if (!strcmp(opv, docops[di])) {
                        snprintf(tool, sizeof(tool), "doc");
                        fprintf(stderr, "  （没给 tool，按 op=%s 路由到 doc）\n", opv);
                        break;
                    }
                }
            }
            if (!tool[0]) {
                json_free(reply);
                printf("\n最终回答: %s\n", content);
                free(content);
                rc = 0;
                break;
            }
        }

        if (!strcmp(tool, "final")) {
            const char *ans = json_str(json_get(reply, "answer"));
            printf("\n最终回答: %s\n", ans ? ans : content);
            /* 模型答案里没出现插件算出的数 → 补一行，别让用户以为没算 */
            if (last_val[0] && (!ans || !strstr(ans, last_val)))
                printf("（插件算得 %s）\n", last_val);
            json_free(reply);
            free(content);
            rc = 0;
            break;
        }

        plugin_desc_t *p = find_plugin(tool);

        /* 模型选错插件：op 其实属于别的插件。
         * 典型：让 sys 执行 logread / iwinfo / uci_get —— 这些是 owrt 的 op。
         * 框架本来就有 op→插件 的映射表（直连时用），这里复用它纠正，
         * 别白费一步去报"不支持的 op"。 */
        if (p) {
            char op2[48] = {0};
            json_str_field(content, "op", op2, sizeof(op2));

            /* 歧义 op 不能自动改投：probe 既是 browser 的（探网页）又是 jq 的
             * （探 JSON），keys/get/query/table/extract/page/csv 同理。
             * 之前"看看 /tmp 下最大的文件"被模型发成 tool=doc,op=probe，
             * 这里一改投就落到 jq 头上，jq 去读 JSON → 报"没指定 file"。
             * 歧义的交给原插件自己报错，比乱改投好。 */
            static const char *AMB[] = {"probe", "keys", "get", "query", "table",
                                        "extract", "page", "csv", "info", "jq", NULL};
            int ambiguous = 0;
            for (int ai = 0; AMB[ai]; ai++)
                if (!strcmp(op2, AMB[ai])) { ambiguous = 1; break; }

            if (op2[0] && !ambiguous) {
                const char *right = op_to_plugin(content, op2);
                if (right && strcmp(right, tool) != 0) {
                    plugin_desc_t *p2 = find_plugin(right);
                    if (p2) {
                        printf("【插件切换 %s → %s】%s 不是 %s 的 op\n", tool, right, op2, tool);
                fflush(stdout);
                        p = p2;
                    }
                }
            }
        }

        if (!p) {
            printf("  ! 未知工具 %s\n", tool);
            log_append(log, BIG, "%s%s", "（上一轮：没有该工具，可用工具见上）\n", "");
            free(content);
            continue;
        }

        printf("  调用插件 %s…\n", tool);
        /* 注入 llama 地址：插件（比如 batch）自己要调模型时用 */
        char enr[BIG];
        size_t cl = strlen(content);
        while (cl > 0 && (content[cl - 1] == ' ' || content[cl - 1] == '\n')) cl--;

        /* math 的机械题型要靠原题文字才能识别。模型经常只给
         * {"op":"solve","equation":"..."} 不带 task，结果应用题被当成
         * 纯方程解（还可能列错式子）。这里把用户原任务补进去。
         * 纯方程题不受影响：模板匹配不上会回落到 solve。 */
        char extra[2100] = {0};
        if ((!strcmp(p->name, "math") || !strcmp(tool, "math"))
            && !strstr(content, "\"task\"") && task && task[0]) {
            snprintf(extra, sizeof(extra), ",\"task\":\"%s\"", task_esc);
        }

        if (cl > 0 && content[cl - 1] == '}') {
            snprintf(enr, sizeof(enr), "%.*s%s,\"_host\":\"%s\",\"_port\":%d,"
                     "\"_model\":\"%s\",\"_task\":\"%s\"}",
                     (int)(cl - 1), content, extra, host, port, model, task_esc);
        } else {
            snprintf(enr, sizeof(enr), "%s", content);
        }
        printf("【插件 %s 开始工作】\n", p->name);
        fflush(stdout);
        char *out = p->run(enr);
        free(content);
        char dans[600] = {0};
        if (out && direct_answer_of(tool, out, step, dans, sizeof(dans))) {
            /* 插件已经把答案写清楚了，别再花 100~190 秒让模型复述。
             * 先打插件原始结果（前端渲染成卡片），再打"最终回答" ——
             * 反过来的话原始 JSON 会被并进答案气泡。 */
            char cutd[BIG];
            snprintf(cutd, sizeof(cutd), "%s", out);
            utf8_cut(cutd, 20000);
            printf("  [%s] %s\n", p->name, cutd);
            printf("\n最终回答: %s\n", dans);
            free(out);
            rc = 0;
            break;
        }
        if (!out) {
            printf("  ! 插件返回空\n");
            continue;
        }

        /* 同一个调用连续失败两次就别再试了。
         * 小模型遇到"缺少参数"这类错误会原样重发，把 step 全烧在同一个
         * 错误上（实测：grep 缺 pattern 连发 3 次，4 步一步没成）。
         * 与其耗到步数上限再报"未完成"，不如立刻停并说清缺什么。 */
        {
            static char last_sig[700];
            static char fail_tool[64];
            static int fail_cnt;
            if (strstr(out, "\"error\"")) {
                /* 前一步已经成功出过结果（比如 curl→jq 已经拿到 30 条 + CSV），
                 * 这一步模型又拿错文件/拿文本列去排序 —— 这是它在瞎试。
                 * 以前会再失败一次才停，最后给用户一串"没能完成"，把已经
                 * 拿到手的结果埋了。这里直接用上一步的结果收尾。 */
                int data_err = strstr(out, "不是 JSON") || strstr(out, "不是数字列")
                            || strstr(out, "没认出要按哪一列") || strstr(out, "里没有「");
                int same_tool = last_ok_tool[0] && !strcmp(last_ok_tool, tool);
                if (last_result && last_result[0] && (data_err || same_tool)) {
                    char ans[400] = {0};
                    json_str_field(last_result, "answer", ans, sizeof(ans));
                    char why[300];
                    snprintf(why, sizeof(why), "%s", out);
                    utf8_cut(why, 160);
                    printf("  ! 这一步没成（%s），但前面 %s 已经出结果了，按那个收尾\n",
                           why, last_ok_tool);
                    printf("\n最终回答: %s\n", ans[0] ? ans : "见上一步的结果");
                    free(out);
                    rc = 0;
                    break;
                }
                char sig[700];
                snprintf(sig, sizeof(sig), "%s|%.500s", tool, enr);
                /* 同一插件连续失败 2 次就停。小模型不只原样重发，还会换着
                 * 花样发错 op（logread 给 sys、grep 找不到 /var/log/syslog），
                 * 光比对"参数完全相同"会被它绕过去，一路烧到步数上限。 */
                if (!strcmp(fail_tool, tool)) fail_cnt++;
                else { snprintf(fail_tool, sizeof(fail_tool), "%s", tool); fail_cnt = 1; }
                if (!strcmp(sig, last_sig) || fail_cnt >= 2) {
                    char msg[400];
                    snprintf(msg, sizeof(msg), "%s", out);
                    utf8_cut(msg, 240);
                    printf("  ! 插件 %s 连续失败两次，停止重试\n", tool);
                    printf("\n最终回答: 没能完成。%s 返回的错误是：%s\n"
                           "（重复同一个调用只会得到同样的错误，需要补参数或换插件）\n",
                           tool, msg);
                    free(out);
                    rc = 0;
                    break;
                }
                snprintf(last_sig, sizeof(last_sig), "%s", sig);
            } else {
                last_sig[0] = 0;
                fail_cnt = 0;
                fail_tool[0] = 0;
            }
        }
        /* 记下完整结果：模型没给 final 时拿它收尾，别白算一场 */
        free(last_result);
        last_result = strdup(out);
        snprintf(last_ok_tool, sizeof(last_ok_tool), "%s", tool);
        /* 记下这一轮插件算出的数值：模型 final 时若没体现，由框架补一行。
         * 3B 常把表达式原样当答案（问"根号3加2"就答 "sqrt(3)+2"），
         * 插件明明算了 3.732 却不展示，用户会以为算错了。 */
        {
            char *pv = strstr(out, "\"value\":");
            if (pv) {
                pv += 8;
                while (*pv == ' ') pv++;
                size_t k = 0;
                if (*pv != '"' && *pv != '{' && *pv != '[') {
                    while (*pv && *pv != ',' && *pv != '}' && k + 1 < sizeof(last_val))
                        last_val[k++] = *pv++;
                    last_val[k] = 0;
                }
            }
        }
        {
            char cut[BIG];
            snprintf(cut, sizeof(cut), "%s", out);
            /* 不能截太短：math 的解题过程是 markdown 表格，1000+ 字节很正常。
             * 截断会把 JSON 砍坏，前端 JSON.parse 失败就只能显示原始文本，
             * 用户看到的就是"只有结果没有过程"。 */
            utf8_cut(cut, 20000);
            printf("  [%s] %s\n", p->name, cut);
            MEMCHK("after-plugin");
        }

        /* ===== 插件联动：curl 抓到 JSON → jq 探测结构 ===== */
        {
            char *nxo = pipe_followup(p->name, out, "model");
            if (nxo) {
                char cut2[BIG];
                snprintf(cut2, sizeof(cut2), "%s", nxo);
                utf8_cut(cut2, 3000);
                printf("  [%s] %s\n", PIPES[0].to, cut2);
                free(last_result);
                last_result = strdup(nxo);
                free(nxo);
            }
        }
        log_append(log, BIG, "工具 %s 返回：\n", tool, "");
        log_append(log, BIG, "%s\n", out, "");
        free(out);
    }

    if (rc != 0) {
        /* 模型没给出 final（3B 常见：拿到插件结果不知道怎么说），
         * 但插件其实已经算出来了 → 直接用它收尾，别丢结果 */
        char *ans = NULL;
        if (last_result) {
            char err3[64];
            jval *lv = json_parse(last_result, err3, sizeof(err3));
            if (lv) {
                const char *a = json_str(json_get(lv, "answer"));
                if (!a) a = json_str(json_get(lv, "value"));
                if (a) ans = strdup(a);
                json_free(lv);
            }
        }
        if (ans) {
            printf("\n最终回答: %s\n", ans);
            free(ans);
            rc = 0;
        } else {
            printf("\n⚠ 跑到第 %d 步仍未给出最终答案。\n"
                   "（已产出的文件都保存在 /tmp 或 /tmp/llama_agent/ 下，"
                   "上面每一步的输出里能看到路径；可以让我接着做，"
                   "或者把任务拆小一点重问）\n", last_step);
        }
    }
    free(last_result);
    printf("[统计] %d 个插件 / C 框架\n", g_n);
    free(task_esc);

    free(log);
    free(tools);
    free(sys_esc);
    return rc;
}
