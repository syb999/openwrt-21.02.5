/* curl.c — 解析浏览器"Copy as cURL"并发送，响应按类型整理
 *
 * 为什么单独做这个插件：从 DevTools 复制出来的 curl 带一堆参数
 * （-H 十几条、--data-raw、--compressed、cookie、$'...' 转义），
 * 模型自己拼 JSON 参数去调 URL 基本拼不对；把整段原文丢进来最省事。
 *
 * 解析器要能吃下这几种来源：
 *   bash      curl 'https://x' \  + 换行     （\ 续行，单引号）
 *   cmd.exe   curl "https://x" ^  + 换行     （^ 续行，双引号）
 *   PowerShell curl.exe 'https://x' ` + 换行 （` 续行）
 *   ANSI-C   $'{"a":"\n"}'                    （$'...' 转义）
 *
 * 安全：URL 只允许 http/https —— curl 支持 file://，那是本地任意文件读取。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#include "plugin.h"

#define OUTMAX    32768
#define WORK_DIR  "/tmp/llama_agent"
#define MAX_HDR   40
#define MAX_FORM  16
#define MAX_BODY  (512 * 1024)
#define MAX_RESP  (20 * 1024 * 1024)

typedef struct {
    char url[2048];
    char method[16];
    char headers[MAX_HDR][1024];
    int  nhdr;
    char body[MAX_BODY];
    int  has_body;
    char cookies[2048];
    char ua[512];
    char user[512];
    char referer[1024];
    char forms[MAX_FORM][1024];
    int  nform;
    int  insecure;
    int  follow;
    char parse_note[256];        /* 解析过程中的提示（比如忽略了 -o） */
} curlreq_t;

/* ---------------- JSON ---------------- */
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
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 'r': out[i++] = '\r'; break;
            case 't': out[i++] = '\t'; break;
            case '"': out[i++] = '"'; break;
            case '\\': out[i++] = '\\'; break;
            case '/': out[i++] = '/'; break;
            default: out[i++] = *p; break;
            }
            p++;
        } else out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

static long jget_int(const char *js, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atol(p + strlen(pat)) : dflt;
}

static void jesc_into(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    for (const char *p = src; *p && i + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { dst[i++] = '\\'; dst[i++] = *p; }
        else if (c == '\n') { dst[i++] = '\\'; dst[i++] = 'n'; }
        else if (c == '\r') { dst[i++] = '\\'; dst[i++] = 'r'; }
        else if (c == '\t') { dst[i++] = '\\'; dst[i++] = 't'; }
        else if (c < 0x20) { dst[i++] = ' '; }
        else dst[i++] = *p;
    }
    dst[i] = 0;
}

static char *jpair(const char *text, const char *answer, const char *extra) {
    size_t cap = strlen(text) * 2 + strlen(answer) * 2 + (extra ? strlen(extra) * 2 : 0) + 512;
    char *o = malloc(cap);
    if (!o) return NULL;
    char *te = malloc(strlen(text) * 2 + 8);
    char *ae = malloc(strlen(answer) * 2 + 8);
    if (!te || !ae) { free(o); free(te); free(ae); return NULL; }
    jesc_into(text, te, strlen(text) * 2 + 8);
    jesc_into(answer, ae, strlen(answer) * 2 + 8);
    if (extra) {
        /* extra 已经是 JSON 片段（"status":200,"file":"..."），直接拼；
         * 再转义一次会把 " 变成 \" ，整个 JSON 就废了 —— 前端 parse 失败，
         * 用户看到的就是一串转义字符。 */
        snprintf(o, cap, "{\"value\":\"%s\",\"answer\":\"%s\",%s}", te, ae, extra);
    } else {
        snprintf(o, cap, "{\"value\":\"%s\",\"answer\":\"%s\"}", te, ae);
    }
    free(te);
    free(ae);
    return o;
}

static char *jerr(const char *msg) {
    size_t cap = strlen(msg) * 2 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    char *e = malloc(strlen(msg) * 2 + 8);
    if (!e) { free(o); return NULL; }
    jesc_into(msg, e, strlen(msg) * 2 + 8);
    snprintf(o, cap, "{\"error\":\"%s\"}", e);
    free(e);
    return o;
}

/* ---------------- 分词：处理引号 / 续行 / ANSI-C ---------------- */
static void skip_ws_and_cont(const char **pp) {
    const char *p = *pp;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        /* 续行：\ 换行 / ^ 换行 / ` 换行 */
        if ((*p == '\\' || *p == '^' || *p == '`') && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
            p += (p[1] == '\r') ? 3 : 2;
            continue;
        }
        if (*p == '\n') { p++; continue; }
        break;
    }
    *pp = p;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 读一个 token。返回 0 成功，-1 结束 */
static int next_token(const char **pp, char *out, size_t cap) {
    const char *p = *pp;
    skip_ws_and_cont(&p);
    if (!*p) { *pp = p; return -1; }

    size_t i = 0;

    /* $'...' ANSI-C 转义 */
    if (p[0] == '$' && p[1] == '\'') {
        p += 2;
        while (*p && *p != '\'') {
            if (*p == '\\' && p[1]) {
                p++;
                char c = 0;
                switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '\'': c = '\''; break;
                case '"': c = '"'; break;
                case '0': c = 0; break;
                case 'x': {
                    int h1 = hexval(p[1]), h2 = hexval(p[2]);
                    if (h1 >= 0 && h2 >= 0) { c = (char)(h1 * 16 + h2); p += 2; }
                    else c = 'x';
                    break;
                }
                default: c = *p; break;
                }
                if (c && i + 1 < cap) out[i++] = c;
                p++;
            } else {
                if (i + 1 < cap) out[i++] = *p;
                p++;
            }
        }
        if (*p == '\'') p++;
        out[i] = 0;
        *pp = p;
        return 0;
    }

    /* '...' 单引号：原样（bash 里单引号内没有任何转义） */
    if (*p == '\'') {
        p++;
        while (*p && *p != '\'') {
            if (i + 1 < cap) out[i++] = *p;
            p++;
        }
        if (*p == '\'') p++;
        out[i] = 0;
        *pp = p;
        return 0;
    }

    /* "..." 双引号：处理 \" \\ \$ \` */
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                if (i + 1 < cap) out[i++] = *p;
                p++;
            } else {
                if (i + 1 < cap) out[i++] = *p;
                p++;
            }
        }
        if (*p == '"') p++;
        out[i] = 0;
        *pp = p;
        return 0;
    }

    /* 裸 token */
    while (*p && !isspace((unsigned char)*p)) {
        if (*p == '\\' && p[1] == '\n') break;      /* 续行不该出现在词中间 */
        if (i + 1 < cap) out[i++] = *p;
        p++;
    }
    out[i] = 0;
    *pp = p;
    return i ? 0 : -1;
}

/* ---------------- 解析 curl 命令 ---------------- */
/* 一次比对任意多个候选：opt_any(tok, "--a", "-b", NULL) */
static int opt_any(const char *tok, ...) {
    va_list ap;
    va_start(ap, tok);
    const char *a;
    int hit = 0;
    while ((a = va_arg(ap, const char *)) != NULL) {
        if (!strcmp(tok, a)) { hit = 1; break; }
    }
    va_end(ap);
    return hit;
}

static int opt_is(const char *tok, const char *a, const char *b, const char *c) {
    if (a && !strcmp(tok, a)) return 1;
    if (b && !strcmp(tok, b)) return 1;
    if (c && !strcmp(tok, c)) return 1;
    return 0;
}

/* 把 --header=xxx 形式拆出来 */
static int split_eq(const char *tok, char *name, size_t ncap, char *val, size_t vcap) {
    const char *eq = strchr(tok, '=');
    if (!eq) return 0;
    size_t L = (size_t)(eq - tok);
    if (L >= ncap) return 0;
    memcpy(name, tok, L);
    name[L] = 0;
    snprintf(val, vcap, "%s", eq + 1);
    /* 值的引号可能在 tok 内 */
    size_t vl = strlen(val);
    if (vl >= 2 && ((val[0] == '\'' && val[vl - 1] == '\'') || (val[0] == '"' && val[vl - 1] == '"'))) {
        memmove(val, val + 1, vl - 2);
        val[vl - 2] = 0;
    }
    return 1;
}

static int parse_curl(const char *src, curlreq_t *r, char *err, size_t errcap) {
    memset(r, 0, sizeof(*r));
    const char *p = src;
    char tok[8192];
    int saw_curl = 0;

    while (next_token(&p, tok, sizeof(tok)) == 0) {
        if (!tok[0]) continue;

        /* 开头的 curl / curl.exe */
        if (!saw_curl && (!strcmp(tok, "curl") || !strcmp(tok, "curl.exe")
                          || !strcmp(tok, "CURL") || !strcmp(tok, "/usr/bin/curl"))) {
            saw_curl = 1;
            continue;
        }

        if (tok[0] == '-' && tok[1] && !isdigit((unsigned char)tok[1])) {
            /* ---- 选项 ---- */
            char name[64], inline_val[8192];
            int has_inline = 0;
            if (tok[1] == '-' && split_eq(tok, name, sizeof(name), inline_val, sizeof(inline_val)))
                has_inline = 1;
            const char *opt = tok;

            #define NEED_VAL(dst, cap) do { \
                if (has_inline) snprintf((dst), (cap), "%s", inline_val); \
                else if (next_token(&p, tok, sizeof(tok)) == 0) snprintf((dst), (cap), "%s", tok); \
            } while (0)

            if (opt_is(opt, "-X", "--request", NULL)) {
                NEED_VAL(r->method, sizeof(r->method));
            } else if (opt_is(opt, "-H", "--header", NULL)) {
                char hb[1024] = {0};
                NEED_VAL(hb, sizeof(hb));
                /* accept-encoding 会把响应压成 gzip，我们不解压就别要 */
                if (strncasecmp(hb, "accept-encoding", 15) != 0) {
                    if (r->nhdr < MAX_HDR) snprintf(r->headers[r->nhdr++], 1024, "%s", hb);
                } else {
                    snprintf(r->parse_note, sizeof(r->parse_note),
                             "已丢掉 accept-encoding 头（避免收到压缩数据）");
                }
            } else if (opt_is(opt, "-d", "--data", "--data-raw")) {
                NEED_VAL(r->body, sizeof(r->body));
                r->has_body = 1;
                if (!r->method[0]) snprintf(r->method, sizeof(r->method), "POST");
            } else if (opt_is(opt, "--data-binary", "--data-ascii", "-T")) {
                NEED_VAL(r->body, sizeof(r->body));
                r->has_body = 1;
                if (!r->method[0]) snprintf(r->method, sizeof(r->method), "POST");
            } else if (opt_is(opt, "-b", "--cookie", NULL)) {
                NEED_VAL(r->cookies, sizeof(r->cookies));
            } else if (opt_is(opt, "-A", "--user-agent", NULL)) {
                NEED_VAL(r->ua, sizeof(r->ua));
            } else if (opt_is(opt, "-u", "--user", NULL)) {
                NEED_VAL(r->user, sizeof(r->user));
            } else if (opt_is(opt, "-e", "--referer", NULL)) {
                NEED_VAL(r->referer, sizeof(r->referer));
            } else if (opt_is(opt, "-F", "--form", NULL)) {
                if (r->nform < MAX_FORM) NEED_VAL(r->forms[r->nform++], 1024);
                else { char skip[1024]; NEED_VAL(skip, sizeof(skip)); }
                if (!r->method[0]) snprintf(r->method, sizeof(r->method), "POST");
            } else if (opt_is(opt, "-k", "--insecure", NULL)) {
                r->insecure = 1;
            } else if (opt_is(opt, "-L", "--location", NULL)) {
                r->follow = 1;
            } else if (opt_is(opt, "-o", "--output", "-O")) {
                /* 丢弃：输出统一由插件管，不许它往任意路径写 */
                char skip[1024];
                NEED_VAL(skip, sizeof(skip));
                snprintf(r->parse_note, sizeof(r->parse_note), "已忽略原命令里的 -o/-O，输出由插件统一保存");
            } else if (opt_is(opt, "--output-document", "--remote-name", NULL)) {
                char skip[1024];
                NEED_VAL(skip, sizeof(skip));
            } else if (opt_any(opt, "--compressed", "--silent", "-s", "-S", "-i", "-v",
                               "--insecure-skip-verify", NULL)) {
                /* 无值或已处理 */
            } else if (opt_is(opt, "--url", NULL, NULL)) {
                NEED_VAL(r->url, sizeof(r->url));
            } else if (opt_any(opt, "-m", "--max-time", "--connect-timeout", "-w", "--write-out",
                               "--retry", "--limit-rate", "-x", "--proxy", "--cert", "--key",
                               "-c", "--cookie-jar", "--resolve", "--interface", NULL)) {
                char skip[1024];
                NEED_VAL(skip, sizeof(skip));
            } else {
                /* 未知选项：以 -- 开头且没 inline 值的不带参数，短选项可能带 */
                if (strncmp(opt, "--", 2) == 0 && !has_inline) {
                    /* 保守起见不吞下一个 token（它可能是 URL） */
                } else if (strncmp(opt, "--", 2) != 0) {
                    char skip[1024];
                    NEED_VAL(skip, sizeof(skip));
                }
            }
            #undef NEED_VAL
        } else {
            /* 位置参数 = URL */
            if (!r->url[0]) snprintf(r->url, sizeof(r->url), "%s", tok);
        }
    }

    if (!r->url[0]) {
        snprintf(err, errcap, "没解析出 URL。粘贴的应该是浏览器 Copy as cURL 的整段内容（以 curl 开头）");
        return -1;
    }
    if (strncmp(r->url, "http://", 7) && strncmp(r->url, "https://", 8)) {
        snprintf(err, errcap,
            "URL 协议不允许：%s。只支持 http/https —— curl 支持 file:// 会读本地文件，"
            "这里必须挡掉", r->url);
        return -1;
    }
    /* Chrome 粘贴时常带多行 --data，把换行压掉（JSON 里不会有多余换行是安全的） */
    if (!r->method[0]) snprintf(r->method, sizeof(r->method), r->has_body ? "POST" : "GET");
    return 0;
}

/* ---------------- 执行 ---------------- */
static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);          /* 补最后一级：只建中间层的话 -o 写不进去 */
}

/* 跑 curl，响应写文件，meta 拿状态码等信息 */
static int run_curl(curlreq_t *r, const char *outfile, char *meta, size_t metacap,
                    char *errbuf, size_t errcap) {
    char *argv[192];
    int n = 0;
    char codebuf[256], maxbuf[32], tbuf[32], fbuf[32];

    snprintf(codebuf, sizeof(codebuf),
        "%%{http_code}|%%{content_type}|%%{size_download}|%%{time_total}|%%{url_effective}");
    snprintf(maxbuf, sizeof(maxbuf), "20971520");
    snprintf(tbuf, sizeof(tbuf), "60");
    snprintf(fbuf, sizeof(fbuf), "1");        /* --fail 不用：4xx/5xx 也要看响应体 */

    argv[n++] = (char *)"curl";
    argv[n++] = (char *)"-s";
    argv[n++] = (char *)"-S";
    argv[n++] = (char *)"--max-time";
    argv[n++] = tbuf;
    argv[n++] = (char *)"--max-filesize";
    argv[n++] = maxbuf;
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)outfile;
    argv[n++] = (char *)"-w";
    argv[n++] = codebuf;
    argv[n++] = (char *)"-X";
    argv[n++] = r->method;

    for (int i = 0; i < r->nhdr; i++) {
        argv[n++] = (char *)"-H";
        argv[n++] = r->headers[i];
    }
    if (r->has_body && n < 170) {
        argv[n++] = (char *)"--data-binary";
        argv[n++] = r->body;
    }
    for (int i = 0; i < r->nform && n < 170; i++) {
        argv[n++] = (char *)"-F";
        argv[n++] = r->forms[i];
    }
    if (r->cookies[0] && n < 170) { argv[n++] = (char *)"-b"; argv[n++] = r->cookies; }
    if (r->ua[0] && n < 170)      { argv[n++] = (char *)"-A"; argv[n++] = r->ua; }
    if (r->user[0] && n < 170)    { argv[n++] = (char *)"-u"; argv[n++] = r->user; }
    if (r->referer[0] && n < 170) { argv[n++] = (char *)"-e"; argv[n++] = r->referer; }
    if (r->insecure)  argv[n++] = (char *)"-k";
    if (r->follow)    argv[n++] = (char *)"-L";
    /* 刻意不发 --compressed：板上的 libcurl 未必编了 zlib，而且我们主动声明
     * Accept-Encoding 只会让自己拿到需要解压的二进制。明文最省心。 */
    argv[n++] = r->url;
    argv[n] = NULL;
    (void)fbuf;

    int fd[2];
    if (pipe(fd) != 0) { snprintf(errbuf, errcap, "pipe 失败"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); snprintf(errbuf, errcap, "fork 失败"); return -1; }
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[0]); close(fd[1]);
        alarm(90);
        execv("/usr/bin/curl", argv);
        _exit(127);
    }
    close(fd[1]);
    size_t k = 0;
    ssize_t rd;
    while ((rd = read(fd[0], meta + k, metacap - 1 - k)) > 0) {
        k += (size_t)rd;
        if (k + 2 >= metacap) break;
    }
    meta[k] = 0;
    close(fd[0]);

    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        snprintf(errbuf, errcap, "找不到 /usr/bin/curl");
        return -1;
    }
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) {
        snprintf(errbuf, errcap, "请求超时（90 秒）");
        return -1;
    }
    /* curl 非 0 一般是网络错误：把 stderr 带出去 */
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        snprintf(errbuf, errcap, "curl 退出码 %d：%.300s", WEXITSTATUS(st),
                 meta[0] ? meta : "(无输出)");
        return -1;
    }
    return 0;
}

/* 按 UTF-8 边界截断：按字节硬切会把中文切成半个字，整个输出就成乱码了 */
static void utf8_trunc(char *s, size_t maxbytes) {
    size_t len = strlen(s);
    if (len <= maxbytes) return;
    size_t i = maxbytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s[i] = 0;
}

/* ---------------- \uXXXX 解码 ----------------
 * 很多接口把中文写成 \u88c5\u673a（"装机"），直接丢给模型等于乱码。
 */
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

static void unesc_u(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 8 < cap; p++) {
        if (*p == '\\' && p[1] == '\\') {          /* 字面反斜杠，别当转义 */
            out[o++] = '\\';
            out[o++] = '\\';
            p++;
            continue;
        }
        if (*p == '\\' && p[1] == 'u') {
            unsigned cp = 0;
            int ok = 1;
            for (int k = 2; k <= 5; k++) {
                int h = hexval((unsigned char)p[k]);
                if (h < 0) { ok = 0; break; }
                cp = cp * 16 + (unsigned)h;
            }
            if (ok) {
                p += 5;
                /* 代理对：\ud83d\ude00 → 一个 emoji */
                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    unsigned lo = 0;
                    int ok2 = 1;
                    for (int k = 3; k <= 6; k++) {
                        int h = hexval((unsigned char)p[k]);
                        if (h < 0) { ok2 = 0; break; }
                        lo = lo * 16 + (unsigned)h;
                    }
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                o += (size_t)utf8_emit(out + o, cp);
                continue;
            }
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* ---------------- JSON 美化 ----------------
 * 只做缩进和换行，不重排内容 —— 大响应故意保留原样结构，
 * 免得"美化"把超长数组展开成几千行。
 */
static void json_pretty(const char *in, char *out, size_t cap, int max_out) {
    size_t o = 0;
    int indent = 0, in_str = 0, esc = 0;
    char last_sig = 0;

    #define PUT(ch) do { if (o + 1 < cap && (int)o < max_out) out[o++] = (ch); } while (0)
    #define NL() do { \
        if (o + 1 < cap && (int)o < max_out) out[o++] = '\n'; \
        for (int q = 0; q < indent && o + 1 < cap && (int)o < max_out; q++) { \
            out[o++] = ' '; out[o++] = ' '; \
        } \
    } while (0)

    for (const char *p = in; *p; p++) {
        char c = *p;
        if (in_str) {
            PUT(c);
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        switch (c) {
        case '"': in_str = 1; PUT(c); last_sig = '"'; break;
        case '{': case '[':
            PUT(c);
            indent++;
            NL();
            last_sig = c;
            break;
        case '}': case ']':
            indent = indent > 0 ? indent - 1 : 0;
            NL();
            PUT(c);
            last_sig = c;
            break;
        case ',':
            PUT(c);
            NL();
            last_sig = ',';
            break;
        case ':':
            PUT(c);
            PUT(' ');
            last_sig = ':';
            break;
        case ' ': case '\t': case '\n': case '\r':
            break;                      /* 原有空白丢掉，重新排版 */
        default:
            PUT(c);
            last_sig = 'v';
            break;
        }
    }
    out[o] = 0;
    (void)last_sig;
    #undef PUT
    #undef NL
}

/* 顶层字段名（给个"里面有什么"的速览） */
static void json_top_keys(const char *js, char *out, size_t cap) {
    size_t i = 0;
    int depth = 0, in_str = 0, esc = 0;
    const char *p = js;
    out[0] = 0;
    while (*p && i + 40 < cap) {
        char c = *p;
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') {
                in_str = 0;
                if (depth == 1) {
                    /* 看后面是不是冒号 → 是 key */
                    const char *q = p + 1;
                    while (*q == ' ') q++;
                    if (*q == ':') {
                        /* 回找字符串开头 */
                        const char *e = p, *s = p;
                        while (s > js && s[-1] != '"') s--;
                        size_t L = (size_t)(e - s);
                        if (L > 0 && L < 40) {
                            if (i) i += (size_t)snprintf(out + i, cap - i, ", ");
                            i += (size_t)snprintf(out + i, cap - i, "%.*s", (int)L, s);
                        }
                    }
                }
            }
            p++;
            continue;
        }
        if (c == '"') in_str = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        p++;
    }
    if (!out[0]) snprintf(out, cap, "(非对象或没有顶层字段)");
}

/* HTML 剥标签 */
static void strip_html(const char *in, char *out, size_t cap, int max_out) {
    size_t o = 0;
    int tag = 0, skip_script = 0;
    for (const char *p = in; *p && (int)o < max_out && o + 8 < cap; p++) {
        if (!strncasecmp(p, "<script", 7)) skip_script = 1;
        if (!strncasecmp(p, "<style", 6)) skip_script = 1;
        if (!strncasecmp(p, "</script", 8) || !strncasecmp(p, "</style", 7)) skip_script = 0;
        if (*p == '<') { tag = 1; continue; }
        if (*p == '>') { tag = 0; continue; }
        if (tag || skip_script) continue;
        if (*p == '\n' || *p == '\r') {
            if (o && out[o - 1] != '\n') out[o++] = '\n';
            continue;
        }
        if (*p == '&') {
            /* 只处理最常见的几个实体 */
            if (!strncmp(p, "&nbsp;", 6)) { out[o++] = ' '; p += 5; continue; }
            if (!strncmp(p, "&amp;", 5)) { out[o++] = '&'; p += 4; continue; }
            if (!strncmp(p, "&lt;", 4)) { out[o++] = '<'; p += 3; continue; }
            if (!strncmp(p, "&gt;", 4)) { out[o++] = '>'; p += 3; continue; }
            if (!strncmp(p, "&quot;", 6)) { out[o++] = '"'; p += 5; continue; }
        }
        if (*p == ' ' || *p == '\t') {
            if (o && out[o - 1] != ' ' && out[o - 1] != '\n') out[o++] = ' ';
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* 从 URL / content-type 定扩展名 */
static void pick_ext(const char *ctype, const char *url, char *out, size_t cap) {
    const char *semi;
    if (ctype && strstr(ctype, "json")) { snprintf(out, cap, "json"); return; }
    if (ctype && strstr(ctype, "html")) { snprintf(out, cap, "html"); return; }
    if (ctype && strstr(ctype, "xml"))  { snprintf(out, cap, "xml"); return; }
    if (ctype && strstr(ctype, "csv"))  { snprintf(out, cap, "csv"); return; }
    if (ctype && strstr(ctype, "pdf"))  { snprintf(out, cap, "pdf"); return; }
    if (ctype && strstr(ctype, "png"))  { snprintf(out, cap, "png"); return; }
    if (ctype && strstr(ctype, "jpeg")) { snprintf(out, cap, "jpg"); return; }
    if (ctype && strstr(ctype, "zip"))  { snprintf(out, cap, "zip"); return; }
    if (ctype && strstr(ctype, "javascript")) { snprintf(out, cap, "js"); return; }
    if (ctype && strstr(ctype, "plain")) { snprintf(out, cap, "txt"); return; }
    /* 退一步看 URL 后缀 */
    semi = url ? strrchr(url, '.') : NULL;
    if (semi && strlen(semi) >= 2 && strlen(semi) <= 8) {
        const char *q = semi + 1;
        int ok = 1;
        for (const char *t = q; *t; t++) if (!isalnum((unsigned char)*t)) { ok = 0; break; }
        if (ok) { snprintf(out, cap, "%s", q); return; }
    }
    snprintf(out, cap, "txt");
}

static char *read_file_head(const char *path, char *buf, size_t cap, long *total) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (total) *total = sz;
    if (sz > (long)cap - 1) sz = (long)cap - 1;
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

/* 找 "key" : [ 的数组起点（摘要用；容忍冒号前后空白） */
static const char *find_arr_start(const char *json, const char *key) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == '[') return q + 1;
        p = q;
    }
    return NULL;
}

/* ---------------- 主流程：发送 + 整理响应 ---------------- */
static char *do_send(curlreq_t *r, int show_body) {
    char dir[512], base[128], outfile[768];
    mkdirs(WORK_DIR "/downloads");
    snprintf(dir, sizeof(dir), "%s/downloads", WORK_DIR);

    char ext[16];
    /* 先用 URL 后缀猜，拿到真实 content-type 后会重命名 */
    snprintf(ext, sizeof(ext), "bin");
    snprintf(base, sizeof(base), "resp_%ld", (long)time(NULL));
    snprintf(outfile, sizeof(outfile), "%s/%s.%s", dir, base, ext);

    char meta[2048] = {0}, errb[1024] = {0};
    if (run_curl(r, outfile, meta, sizeof(meta), errb, sizeof(errb)) != 0)
        return jerr(errb[0] ? errb : "请求失败");

    /* meta = code|content_type|size|time|url */
    int code = 0;
    char ctype[256] = {0}, effurl[1024] = {0};
    double size = 0, tm = 0;
    {
        char *fields[5] = {0};
        int nf = 0;
        char *save = NULL;
        for (char *t = strtok_r(meta, "|", &save); t && nf < 5; t = strtok_r(NULL, "|", &save))
            fields[nf++] = t;
        if (nf > 0) code = atoi(fields[0]);
        if (nf > 1) snprintf(ctype, sizeof(ctype), "%s", fields[1]);
        if (nf > 2) size = atof(fields[2]);
        if (nf > 3) tm = atof(fields[3]);
        if (nf > 4) snprintf(effurl, sizeof(effurl), "%s", fields[4]);
    }

    /* 扩展名先按 content-type 猜，等看清内容再纠（见下面 rename） */
    char ext2[16];
    pick_ext(ctype, r->url, ext2, sizeof(ext2));
    char finalfile[800];
    snprintf(finalfile, sizeof(finalfile), "%s/%s.%s", dir, base, ext2);
    if (strcmp(outfile, finalfile) != 0) rename(outfile, finalfile);

    /* 读响应 */
    static char raw[MAX_RESP];
    long total = 0;
    if (!read_file_head(finalfile, raw, sizeof(raw), &total)) {
        /* 空响应（204 之类）也可能 fopen 成功但为 0 字节 */
        snprintf(raw, sizeof(raw), "");
        total = 0;
    }

    /* 先看内容长相：很多接口（实测 smzdm）返回 text/html 但内容其实是 JSON。
     * 只看 content-type 会把 JSON 当网页剥标签，结构全毁。 */
    int is_json = 0, is_html = 0;
    {
        const char *q = raw;
        while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
        if (*q == '{' || *q == '[') is_json = 1;
        else if (*q == '<') is_html = 1;
        else if (strstr(ctype, "json")) is_json = 1;
        else if (strstr(ctype, "html")) is_html = 1;
    }

    /* 内容说了算，纠正扩展名：smzdm 那种 text/html 里装 JSON 的，
     * 存成 .html 会让后面 jq 直接拒读。 */
    if (is_json && !strstr(finalfile, ".json")) {
        char nf[820];
        snprintf(nf, sizeof(nf), "%s/%s.json", dir, base);
        if (rename(finalfile, nf) == 0) snprintf(finalfile, sizeof(finalfile), "%s", nf);
    } else if (is_html && !strstr(finalfile, ".html")) {
        char nf[820];
        snprintf(nf, sizeof(nf), "%s/%s.html", dir, base);
        if (rename(finalfile, nf) == 0) snprintf(finalfile, sizeof(finalfile), "%s", nf);
    }

    char *text = malloc(OUTMAX);
    if (!text) return NULL;
    size_t ti = 0;

    ti += (size_t)snprintf(text + ti, OUTMAX - ti,
        "**%d %s** · %s · %.1f KB · %.2f 秒\n\n"
        "| 项 | 值 |\n|---|---|\n| 方法 | %s |\n| 状态 | %d |\n"
        "| 类型 | %s |\n| 大小 | %.0f 字节 |\n| 耗时 | %.2f 秒 |\n",
        code, code == 200 ? "OK" : (code >= 400 ? "错误" : ""),
        ctype[0] ? ctype : "(无 content-type)",
        size / 1024.0, tm,
        r->method, code,
        ctype[0] ? ctype : "-", size, tm);
    if (effurl[0] && strcmp(effurl, r->url) != 0)
        ti += (size_t)snprintf(text + ti, OUTMAX - ti, "| 最终地址 | %s |\n", effurl);

    int shown = 0;
    char *decoded = NULL;                 /* 解过 \uXXXX 的副本，后面 jq 也要用 */
    if (is_json && raw[0]) {
        decoded = malloc(strlen(raw) * 2 + 64);
        if (decoded) unesc_u(raw, decoded, strlen(raw) * 2 + 64);

        char keys[512] = {0};
        json_top_keys(decoded ? decoded : raw, keys, sizeof(keys));
        ti += (size_t)snprintf(text + ti, OUTMAX - ti, "\n**顶层字段**：%s\n\n", keys);

        /* 小 JSON 才整份美化；大的只靠下面"主要数组 + 前几条"，
         * 否则截断的 JSON 结构不全，看起来就是一堆没意义的字符 */
        if (total <= 3000) {
            char *pretty = malloc(OUTMAX);
            if (pretty) {
                json_pretty(decoded ? decoded : raw, pretty, OUTMAX, 3000);
                ti += (size_t)snprintf(text + ti, OUTMAX - ti, "```json\n%s\n```\n", pretty);
                free(pretty);
                shown = 1;
            }
        } else {
            shown = 1;        /* 下面会用数组摘要填 */
        }
    } else if (is_html && raw[0]) {
        char *plain = malloc(OUTMAX);
        if (plain) {
            strip_html(raw, plain, OUTMAX, show_body ? 5000 : 2500);
            ti += (size_t)snprintf(text + ti, OUTMAX - ti, "**正文**（已剥 HTML）：\n\n```\n%s\n```\n", plain);
            free(plain);
            shown = 1;
        }
    } else if (raw[0]) {
        size_t take = strlen(raw);
        if (take > (size_t)(show_body ? 4000 : 2000)) take = show_body ? 4000 : 2000;
        ti += (size_t)snprintf(text + ti, OUTMAX - ti, "```\n%.*s\n```\n", (int)take, raw);
        shown = 1;
    }
    if (!shown && total == 0)
        ti += (size_t)snprintf(text + ti, OUTMAX - ti, "\n（响应体为空）\n");
    else if (total > (long)(show_body ? 6000 : 3000))
        ti += (size_t)snprintf(text + ti, OUTMAX - ti,
            "\n（响应共 %ld 字节，上面只显示开头；完整内容见下载文件）\n", total);

    /* 大 JSON：别只给开头 —— 找出"条目数组"（rows/list/items/data…），
     * 报总数 + 前 3 条。这才是"分类整理展示"；直接甩 JSON 开头等于没整理。
     * 纯 C 按括号深度切，不依赖 jq（jq 表达式嵌在 C 字符串里转义太脆）。 */
    if (is_json && total > 8000) {
        char akey[32] = {0};
        const char *astart = NULL;
        static const char *cands[] = {"rows", "list", "items", "records", "feeds", "data", NULL};
        for (int ci = 0; cands[ci] && !astart; ci++) {
            char pat[64];
            snprintf(pat, sizeof(pat), "\"%s\":[", cands[ci]);
            const char *q = strstr(raw, pat);
            if (q) {
                astart = strchr(q, '[');
                if (astart) { snprintf(akey, sizeof(akey), "%s", cands[ci]); astart++; }
            }
        }
        if (astart) {
            /* 按 {} 深度切前 3 个元素 */
            ti += (size_t)snprintf(text + ti, OUTMAX - ti,
                "\n**条目数组是 `%s`，前 3 条如下**\n\n", akey);
            const char *q = astart;
            int shown_items = 0;
            long total_items = 0;
            while (*q && shown_items < 3 && ti < OUTMAX - 3000) {
                while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t' || *q == ',') q++;
                if (*q == ']') break;
                const char *elem = q;
                int depth = 0, in_str = 0, esc2 = 0;
                while (*q) {
                    char c2 = *q;
                    if (in_str) {
                        if (esc2) esc2 = 0;
                        else if (c2 == '\\') esc2 = 1;
                        else if (c2 == '"') in_str = 0;
                    } else {
                        if (c2 == '"') in_str = 1;
                        else if (c2 == '{' || c2 == '[') depth++;
                        else if (c2 == '}' || c2 == ']') {
                            depth--;
                            if (depth == 0) { q++; break; }
                        }
                    }
                    q++;
                }
                long elen = (long)(q - elem);
                if (elen <= 0) break;
                total_items++;
                /* 每条：先把 \uXXXX 解成中文，再截短。
                 * 不解码的话用户看到的是 \u6652\u7269 这种，等于乱码。 */
                char dec[4096];
                long take0 = elen < (long)sizeof(dec) / 2 ? elen : (long)sizeof(dec) / 2;
                char tmpel[4096];
                snprintf(tmpel, sizeof(tmpel), "%.*s", (int)take0, elem);
                unesc_u(tmpel, dec, sizeof(dec));
                size_t dlen = strlen(dec);
                int was_trunc = (dlen > 500);
                if (was_trunc) utf8_trunc(dec, 500);
                ti += (size_t)snprintf(text + ti, OUTMAX - ti,
                    "```json\n%s%s\n```\n", dec,
                    was_trunc ? " …(截断，完整内容见下载文件)" : "");
                shown_items++;
            }
            /* 数一下总共有多少条（继续扫，不输出） */
            {
                const char *q2 = q;
                int depth = 0, in_str = 0, esc3 = 0;
                while (*q2) {
                    char c3 = *q2;
                    if (in_str) {
                        if (esc3) esc3 = 0;
                        else if (c3 == '\\') esc3 = 1;
                        else if (c3 == '"') in_str = 0;
                    } else {
                        if (c3 == '"') in_str = 1;
                        else if (c3 == '{' || c3 == '[') {
                            depth++;
                            if (c3 == '{' && depth == 1) total_items++;
                        } else if (c3 == '}' || c3 == ']') {
                            depth--;
                            if (depth < 0) break;
                        }
                    }
                    q2++;
                }
            }
            ti += (size_t)snprintf(text + ti, OUTMAX - ti,
                "\n共约 %ld 条；要更多：`{\"op\":\"jq\",\"file\":\"%s\","
                "\"path\":\".%s\",\"offset\":3,\"limit\":5}`\n",
                total_items > 0 ? total_items : shown_items,
                strrchr(finalfile, '/') ? strrchr(finalfile, '/') + 1 : finalfile, akey);
        }
    }


    char extra[1200];
    snprintf(extra, sizeof(extra),
        "\"status\":%d,\"type\":\"%s\",\"file\":\"%s\",\"bytes\":%ld",
        code, is_json ? "json" : (is_html ? "html" : "text"), finalfile, total);

    char ans[400];
    snprintf(ans, sizeof(ans), "%s %s → %d（%s，%ld 字节）%s",
             r->method, r->url, code,
             is_json ? "JSON" : (is_html ? "HTML" : (ctype[0] ? ctype : "文本")),
             total, code >= 400 ? " ⚠ 错误响应" : "");

    char *res = jpair(text, ans, extra);
    free(text);

    /* 顺手给个 .txt 伴生（只对文本类，图片/压缩包生成 .txt 是垃圾） */
    int is_texty = (!ctype[0] || strstr(ctype, "text/") || strstr(ctype, "xml")
                    || strstr(ctype, "csv") || strstr(ctype, "javascript"));
    if (is_texty && !is_json && !is_html && raw[0]) {
        char side[820];
        snprintf(side, sizeof(side), "%s.txt", finalfile);
        FILE *f = fopen(side, "w");
        if (f) { fwrite(raw, 1, strlen(raw), f); fclose(f); }
    }
    return res;
}

/* 从结构化字段拼一个 curlreq_t（模型自己有参数时用） */
static int build_request(const char *json, curlreq_t *r, char *err, size_t errcap) {
    memset(r, 0, sizeof(*r));
    jget(json, "url", r->url, sizeof(r->url));
    if (!r->url[0]) {
        snprintf(err, errcap, "解析出来的 cURL 里没有 URL。确认粘贴的是完整命令"
                              "（要包含 curl + 网址），例如：curl 'https://example.com/api' -H 'A: b'");
        return -1;
    }
    if (strncmp(r->url, "http://", 7) && strncmp(r->url, "https://", 8)) {
        snprintf(err, errcap, "只支持 http/https");
        return -1;
    }
    jget(json, "method", r->method, sizeof(r->method));
    jget(json, "body", r->body, sizeof(r->body));
    if (r->body[0]) r->has_body = 1;
    if (!r->method[0]) snprintf(r->method, sizeof(r->method), r->has_body ? "POST" : "GET");

    jget(json, "cookie", r->cookies, sizeof(r->cookies));
    jget(json, "user_agent", r->ua, sizeof(r->ua));

    /* headers 支持两种写法：
     *   "headers":"a: 1\nb: 2"        （模型好写）
     *   "headers":{"a":"1"}           （也行） */
    char hraw[4096] = {0};
    jget(json, "headers", hraw, sizeof(hraw));
    if (hraw[0]) {
        char *save = NULL;
        for (char *line = strtok_r(hraw, "\n", &save); line && r->nhdr < MAX_HDR;
             line = strtok_r(NULL, "\n", &save)) {
            while (*line == ' ') line++;
            if (!*line) continue;
            /* 冒号必须是第一个（避免把 URL 里的 :// 当分隔） */
            char *colon = strchr(line, ':');
            if (!colon) continue;
            snprintf(r->headers[r->nhdr++], 1024, "%s", line);
        }
    }
    return 0;
}

/* ---------------- 入口 ---------------- */
static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));

    /* JSON 处理是 jq 插件的事，这里只管发请求 */
    curlreq_t *r = calloc(1, sizeof(curlreq_t));
    if (!r) return NULL;
    char err[512] = {0};

    /* curl 原文优先：这是这个插件的核心用法 */
    char raw[16384] = {0};
    if (!jget(json_in, "curl", raw, sizeof(raw)) && raw[0]) {
        if (parse_curl(raw, r, err, sizeof(err)) != 0) {
            free(r);
            return jerr(err);
        }
    } else if (!strcmp(op, "parse")) {
        free(r);
        return jerr("缺少 curl 字段。这个 op 要的是浏览器「Copy as cURL」的【整段原文】，"
                    "不是 URL。写法：{\"op\":\"parse\",\"curl\":\"curl 'https://x' -H '...'\"}。"
                    "只想要一个网址的内容，用 sys 插件的 http_get："
                    "{\"op\":\"http_get\",\"url\":\"https://x\"}");
    } else {
        if (build_request(json_in, r, err, sizeof(err)) != 0) {
            free(r);
            return jerr(err);
        }
    }

    if (!strcmp(op, "parse")) {
        /* 只解析：让模型/人确认解析结果对不对 */
        char text[6000];
        size_t i = (size_t)snprintf(text, sizeof(text),
            "**解析结果**\n\n| 项 | 值 |\n|---|---|\n| 方法 | %s |\n| URL | %s |\n| 请求头 | %d 条 |\n"
            "| 请求体 | %d 字节 |\n| Cookie | %s |\n| Form | %d 项 |\n",
            r->method, r->url, r->nhdr, (int)strlen(r->body),
            r->cookies[0] ? "有" : "无", r->nform);
        if (r->nhdr) {
            i += (size_t)snprintf(text + i, sizeof(text) - i, "\n**请求头**\n\n```\n");
            for (int k = 0; k < r->nhdr && i < sizeof(text) - 300; k++)
                i += (size_t)snprintf(text + i, sizeof(text) - i, "%s\n", r->headers[k]);
            i += (size_t)snprintf(text + i, sizeof(text) - i, "```\n");
        }
        char ans[300];
        snprintf(ans, sizeof(ans), "%s %s（%d 个请求头）", r->method, r->url, r->nhdr);
        char *o = jpair(text, ans, NULL);
        free(r);
        return o;
    }

    int show_body = (int)jget_int(json_in, "full", 0);
    char *res = do_send(r, show_body);
    free(r);
    return res;
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "curl",
    "把浏览器 DevTools「Copy as cURL」的整段内容丢进来，直接发出去并整理响应："
    "JSON 自动缩进 + 把 Unicode 转义还原成中文 + 找出条目数组报总数和前几条，"
    "HTML 剥标签取正文，二进制/大响应存文件。落盘后的字段提取/表格交给 jq 插件。"
    "支持 bash/cmd/PowerShell 的续行和各种引号写法。"
    "注意：只给 curl 原文本身，不要在后面接 | jq / | wc 这类管道——响应已经自动整理好了",
    "{\"curl\":\"curl 'https://api.example.com/v1' -H 'accept: application/json'\"}"
    "  |  {\"op\":\"parse\",\"curl\":\"...\"}                       只解析不发送"
    "  |  {\"op\":\"send\",\"url\":\"https://...\",\"method\":\"POST\",\"headers\":\"content-type: application/json\",\"body\":\"{}\"}"
    "  |  {\"op\":\"jq\",\"file\":\"<send 返回的 file>\",\"path\":\".data.rows\"}          探测：类型/长度/首项字段"
    "  |  {\"op\":\"jq\",\"file\":\"...\",\"path\":\".data.rows\",\"offset\":0,\"limit\":5}  取一批"
    "  |  {\"op\":\"jq\",\"file\":\"...\",\"query\":\".data.rows|map(.title)\"}            自定义表达式"
    "  |  可选 \"full\":1 显示更多响应内容",
    run,
    "curl,http,request,api,fetch,请求,接口,抓取"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
