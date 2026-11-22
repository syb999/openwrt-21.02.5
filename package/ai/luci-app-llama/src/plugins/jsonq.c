/* jsonq.c — 本地 JSON 处理（独立插件）
 *
 * 为什么不并进 curl：curl 是"发请求"，这里是"处理已经落盘的 json"，
 * 两件事。拆开之后 doc 导出的 json、sys 下载的文件、用户上传的 json
 * 都能直接用，不用伪装成 curl 调用。
 *
 * op：
 *   probe  探测结构（数组/长度/每条字段）
 *   get    分批取值（path + offset/limit）
 *   table  导出表格 + 全字段 CSV
 *   query  自定义 jq 表达式
 * 不指定 file 时用 downloads 里最新的 json（"上面的那个文件"）。
 *
 * 有 jq 就优先用 jq：嵌套对象/数组、转义这些手写扫描器处理不对。
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

static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    /* 必须找到「后面紧跟冒号」的那个匹配。
     * 直接 strstr 会踩坑：{"op":"query","file":"x","query":"..."} 里找 key
     * "query"，第一个命中是 "op" 的【值】"query"，接着往后找冒号就摸到
     * "file" 上去了 —— 于是 query 拿到的是文件路径，整个 jq 表达式全错。
     * 遍历所有匹配，只认后面（允许空白）是 ':' 的那个。 */
    const char *p = js;
    const char *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return -1;
    p = found + 1;
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

static void utf8_trunc(char *s, size_t maxbytes) {
    size_t len = strlen(s);
    if (len <= maxbytes) return;
    size_t i = maxbytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s[i] = 0;
}

static int run_jq(const char *expr, const char *file, char *out, size_t cap) {
    int fd[2];
    if (pipe(fd) != 0) { snprintf(out, cap, "pipe 失败"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); snprintf(out, cap, "fork 失败"); return -1; }
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[0]); close(fd[1]);
        alarm(30);
        char *argv[] = {
            (char *)"jq", (char *)"-c", (char *)expr, (char *)file, NULL
        };
        execv("/usr/bin/jq", argv);
        _exit(127);
    }
    close(fd[1]);
    size_t n = 0;
    ssize_t r;
    while ((r = read(fd[0], out + n, cap - 1 - n)) > 0) {
        n += (size_t)r;
        if (n + 2 >= cap) break;
    }
    out[n] = 0;
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        snprintf(out, cap, "找不到 /usr/bin/jq");
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_jq_raw(const char *expr, const char *file, char *out, size_t cap) {
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[0]); close(fd[1]);
        alarm(60);
        char *argv[] = { (char *)"jq", (char *)"-r", (char *)expr, (char *)file, NULL };
        execv("/usr/bin/jq", argv);
        _exit(127);
    }
    close(fd[1]);
    size_t n = 0;
    ssize_t r;
    while ((r = read(fd[0], out + n, cap - 1 - n)) > 0) {
        n += (size_t)r;
        if (n + 2 >= cap) break;
    }
    out[n] = 0;
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}


/* 找文件：给了就用给的，没给就用 downloads 里最新的 json */
static char g_auto_file[900];
static long g_auto_age;

/* 自动选中的文件超过 30 分钟就不用：宁可让用户补参数，也别拿旧数据编答案 */
#define AUTO_MAX_AGE 1800L

static int auto_too_old(const char *path, size_t cap) {
    (void)path; (void)cap;
    return g_auto_file[0] && g_auto_age > AUTO_MAX_AGE;
}

static const char *auto_too_old_msg(void) {
    static char m[600];
    snprintf(m, sizeof(m),
             "没指定 file。downloads 里最新的是 %ld 分钟前的 `%s`，"
             "多半不是你要的数据 —— 请明确给 file 参数（或先重新抓数据）",
             g_auto_age / 60, g_auto_file);
    return m;
}

static const char *auto_note(void) {
    static char m[400];
    if (!g_auto_file[0]) return "";
    snprintf(m, sizeof(m), "\n\n> 数据源：`%s`（%ld 分钟前，你没指定 file，自动选的）",
             g_auto_file, g_auto_age / 60);
    return m;
}

/* 没给 file 时会退到"downloads 里最新的 json"。这很危险：用户问一个新问题，
 * 它会拿半小时前抓的无关数据当源，输出一张看着很像样的表 —— 比报错更糟。
 * 这里把"自动选中了哪个、多久以前的"记下来，调用方据此拒绝或明确标注。 */
static int resolve_file(const char *in, char *out, size_t cap) {
    g_auto_file[0] = 0;
    g_auto_age = 0;
    if (in && in[0]) {
        if (in[0] == '/') snprintf(out, cap, "%s", in);
        else snprintf(out, cap, "%s/downloads/%s", WORK_DIR, in);
        struct stat st;
        if (stat(out, &st) == 0) return 0;
        /* 模型常把绝对路径写成 "./tmp/..."：去掉那个点再试一次 */
        if (in[0] == '.' && in[1] == '/') {
            snprintf(out, cap, "%s", in + 1);
            if (stat(out, &st) == 0) return 0;
        }
        return -1;
    }
    DIR *d = opendir(WORK_DIR "/downloads");
    if (!d) return -1;
    time_t best = 0;
    out[0] = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!strstr(e->d_name, ".json")) continue;
        char p[900];
        snprintf(p, sizeof(p), "%s/downloads/%s", WORK_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (st.st_mtime >= best) { best = st.st_mtime; snprintf(out, cap, "%s", p); }
    }
    if (out[0]) {
        struct stat st2;
        snprintf(g_auto_file, sizeof(g_auto_file), "%s", out);
        g_auto_age = (stat(out, &st2) == 0) ? (long)(time(NULL) - st2.st_mtime) : 0;
    }
    closedir(d);
    return out[0] ? 0 : -1;
}

/* 文件是不是 JSON（看头几个字节） */
static int looks_json(const char *path) {
    char head[16] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof(head) - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++)
        if (head[i] == '{' || head[i] == '[') return 1;
    return 0;
}

/* probe：结构探测 —— 是数组还是对象，多长，第一条有哪些字段 */
static char *op_probe(const char *json) {
    char fname[768] = {0}, qpath[512] = {0};
    jget(json, "file", fname, sizeof(fname));
    jget(json, "path", qpath, sizeof(qpath));

    /* 模型经常把**文件名**塞进 path（path 本来是 jq 路径表达式，如 .data.list）。
     * 以前直接把 path 拼进 jq 程序，于是 jq 收到
     *   "./tmp/xx.txt | if type==\"array\" …"  →  syntax error。
     * 看到像路径的（带 / 且以 / . ~ 开头）就当 file 用，jq 路径退回默认。 */
    /* 只接受真正的 jq 路径（以 . 开头，如 .data.rows）。模型有时会把
     * "200 OK · text/html · 102 KB · rows" 这种摘要塞进 path，以前直接拼进
     * jq 程序 → syntax error。不认识的就不当路径用，交给自动探测。 */
    if (qpath[0] && qpath[0] != '.') {
        if ((qpath[0] == '/' || qpath[0] == '~') && strchr(qpath, '/')) {
            if (!fname[0]) snprintf(fname, sizeof(fname), "%s", qpath);
        }
        qpath[0] = 0;
    }
    if (qpath[0] == '.' && !strchr(qpath, '/')) {
        /* 真正的 jq 路径，保留 */
    } else if (qpath[0] == '.' && strchr(qpath, '/')) {
        if (!fname[0]) snprintf(fname, sizeof(fname), "%s", qpath);
        qpath[0] = 0;
    }

    char path[900];
    if (resolve_file(fname, path, sizeof(path)) != 0)
        return jerr("没有可用的 json 文件（先抓数据，或给 file 参数）");
    if (auto_too_old(path, sizeof(path))) return jerr(auto_too_old_msg());
    /* 指到非 JSON 文件上（最常见：模型拿了上传的 xlsx.txt 来 jq）时，
     * 直接改用本次会话最近的 JSON —— 那通常就是刚才 curl 抓回来的响应。
     * 以前这里报"不是 JSON，请走 table"，模型照着去调 table，
     * 又拿 JSON 的字段名去排一张 xlsx 表，越跑越离谱。 */
    char cnote[400] = {0};
    if (!looks_json(path)) {
        char recent[900] = {0};
        resolve_file("", recent, sizeof(recent));
        if (recent[0] && looks_json(recent)) {
            const char *bn1 = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            const char *bn2 = strrchr(recent, '/') ? strrchr(recent, '/') + 1 : recent;
            snprintf(cnote, sizeof(cnote), "（%s 不是 JSON，已改用最近的 JSON `%s`）\n\n", bn1, bn2);
            snprintf(path, sizeof(path), "%s", recent);
        } else {
            return jerr("这个文件不是 JSON，也没有别的 JSON 可用：先把 JSON 抓下来（curl 一个接口）"
                        "或给我一个 .json 文件的路径。jq 只处理 JSON。");
        }
    }

    char *out = malloc(OUTMAX);
    if (!out) return NULL;
    char expr[1024];
    char root[512] = {0};          /* 选定的数组路径，导出 CSV 时用 */
    char dl[900] = {0};            /* 导出的 CSV 路径（给页面下载按钮） */
    long cnt = 0;

    if (qpath[0]) {
        snprintf(root, sizeof(root), "%s", qpath);
        /* 先把长度算出来：输出要写成列表，不是一行挤在一起 */
        snprintf(expr, sizeof(expr), "%s | if type==\"array\" then length else 0 end", qpath);
        char tmp[128] = {0};
        if (run_jq_raw(expr, path, tmp, sizeof(tmp)) == 0) cnt = atol(tmp);
    } else {
        /* 没给 path：自动找最长的数组 */
        static const char *cands[] = {"rows", "list", "items", "records", "feeds", "data", NULL};
        char best[512] = {0};
        long blen = 0;
        for (int i = 0; cands[i] && blen == 0; i++) {
            char expr2[300], tmp[256];
            snprintf(expr2, sizeof(expr2), ".data.%s | if type==\"array\" then length else 0 end", cands[i]);
            if (run_jq_raw(expr2, path, tmp, OUTMAX) == 0 && atol(tmp) > 0) {
                snprintf(best, sizeof(best), ".data.%s", cands[i]);
                blen = atol(tmp);
            }
            if (blen == 0) {
                snprintf(expr2, sizeof(expr2), ".%s | if type==\"array\" then length else 0 end", cands[i]);
                if (run_jq_raw(expr2, path, tmp, OUTMAX) == 0 && atol(tmp) > 0) {
                    snprintf(best, sizeof(best), ".%s", cands[i]);
                    blen = atol(tmp);
                }
            }
        }
        if (best[0]) { snprintf(root, sizeof(root), "%s", best); cnt = blen; }
    }

    if (!root[0]) {
        /* 连数组都没找到：给个总体类型 + 顶层字段 */
        snprintf(expr, sizeof(expr), "if type==\"array\" then \"数组 / \"+(length|tostring)+\" 条\" "
                 "elif type==\"object\" then \"对象 / 顶层字段：\"+(keys_unsorted|join(\", \")) "
                 "else type end");
        if (run_jq_raw(expr, path, out, OUTMAX) != 0) {
            char m[600];
            snprintf(m, sizeof(m), "jq 失败：%.400s", out);
            free(out);
            return jerr(m);
        }
        char extra2[1300];
        snprintf(extra2, sizeof(extra2), "\"file\":\"%s\"", path);
        char *r2 = jpair(out, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, extra2);
        free(out);
        return r2;
    }

    /* ---- 统一格式化：主数组 + 条数 + 字段列表（按 JSON 里的原始顺序） ---- */
    char fields[16384] = {0};
    /* 关键：不能用 `unique`（会按字母排序），要用 reduce 保序去重 ——
     * 用户看到的就是 JSON 响应里的字段顺序。 */
    snprintf(expr, sizeof(expr),
        "[%s[] | keys_unsorted[]] | reduce .[] as $k ([]; if (index($k) != null) then . else . + [$k] end)"
        " | join(\"\\n\")", root);
    run_jq_raw(expr, path, fields, sizeof(fields));

    char keys[160][96];
    int nk = 0;
    {
        char tmpk[16384];
        snprintf(tmpk, sizeof(tmpk), "%s", fields);
        char *sv = NULL;
        for (char *ln = strtok_r(tmpk, "\n", &sv); ln && nk < 160; ln = strtok_r(NULL, "\n", &sv))
            snprintf(keys[nk++], sizeof(keys[0]), "%s", ln);
    }

    size_t off = 0;
    if (cnote[0]) off += (size_t)snprintf(out + off, OUTMAX - off, "%s", cnote);
    off += (size_t)snprintf(out + off, OUTMAX - off,
        "**主数组** `%s` · %ld 条 · %d 个字段\n\n", root, cnt, nk);
    if (nk > 0) {
        off += (size_t)snprintf(out + off, OUTMAX - off, "**字段**（按 JSON 里的顺序）\n\n");
        for (int i = 0; i < nk && i < 40 && off < OUTMAX - 200; i++)
            off += (size_t)snprintf(out + off, OUTMAX - off, "- `%s`\n", keys[i]);
        if (nk > 40)
            off += (size_t)snprintf(out + off, OUTMAX - off, "- …共 %d 个字段（完整列表看导出的 CSV 表头）\n", nk);
        off += (size_t)snprintf(out + off, OUTMAX - off, "\n");
    }

    /* ---- 导出 CSV：表头 + 每行都按同一个键顺序取值，列绝不会错位 ---- */
    if (nk > 0) {
        char dir[600];
        snprintf(dir, sizeof(dir), "/tmp/llama_agent/downloads");
        mkdir(dir, 0755);
        snprintf(dl, sizeof(dl), "%s/jq_%ld.csv", dir, (long)time(NULL));

        char prog[32768];
        size_t po = (size_t)snprintf(prog, sizeof(prog), "(%s[] | [", root);
        for (int i = 0; i < nk && po + 120 < sizeof(prog); i++) {
            char kk[160];
            size_t ko = 0;
            for (const char *q = keys[i]; *q && ko + 4 < sizeof(kk); q++) {
                if (*q == '"' || *q == '\\') kk[ko++] = '\\';
                kk[ko++] = *q;
            }
            kk[ko] = 0;
            /* 每个元素必须自带括号：不然 "|" 会把后一个 .["k"] 作用到前一个的结果上 */
            /* 缺字段 → 空单元格。以前 tostring 会把 null 写成字符串 "null"，
             * 30 行 × 90 列里有 1254 格是 "null"，导出的 xlsx 看着就是一坨乱码。 */
            po += (size_t)snprintf(prog + po, sizeof(prog) - po,
                                   "%s(.[\"%s\"] | if . == null then \"\" "
                                   "elif type==\"string\" then . else tostring end)",
                                   i ? ", " : "", kk);
        }
        snprintf(prog + po, sizeof(prog) - po, "] | @csv)");

        static char buf[1 << 20];
        if (run_jq_raw(prog, path, buf, sizeof(buf)) == 0) {
            FILE *fp = fopen(dl, "w");
            if (fp) {
                for (int i = 0; i < nk; i++)          /* 表头顺序 = JSON 顺序 */
                    fprintf(fp, "%s\"%s\"", i ? "," : "", keys[i]);
                fputc('\n', fp);
                fwrite(buf, 1, strlen(buf), fp);
                fclose(fp);
                off += (size_t)snprintf(out + off, OUTMAX - off,
                    "已导出 CSV（%ld 行 × %d 列，列序与 JSON 一致），点下面的「下载文件」拿走。\n", cnt, nk);
            } else dl[0] = 0;
        } else dl[0] = 0;
    }

    char ans[300];
    snprintf(ans, sizeof(ans), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    char extra[1600];
    if (dl[0])
        snprintf(extra, sizeof(extra), "\"file\":\"%s\",\"path\":\"%s\"", path, dl);
    else
        snprintf(extra, sizeof(extra), "\"file\":\"%s\"", path);
    char *r = jpair(out, ans, extra);
    free(out);
    return r;
}

/* get：分批取值 */
static char *op_get(const char *json) {
    char fname[768] = {0}, qpath[512] = {0}, query[2048] = {0};
    jget(json, "file", fname, sizeof(fname));
    jget(json, "path", qpath, sizeof(qpath));
    jget(json, "query", query, sizeof(query));
    long off = jget_int(json, "offset", 0);
    long lim = jget_int(json, "limit", 10);
    if (off < 0) off = 0;
    if (lim <= 0) lim = 10;
    if (lim > 50) lim = 50;

    char path[900];
    if (resolve_file(fname, path, sizeof(path)) != 0)
        return jerr("没有可用的 json 文件");
    if (!looks_json(path))
        return jerr("这个文件不是 JSON（多半是 xlsx/doc 转出来的纯文本或 TSV）。"
                    "要看它的内容/排序/统计，直接说「展示表格…」走 table 插件；jq 只处理 JSON。");

    char expr[2600];
    if (query[0]) snprintf(expr, sizeof(expr), "%s", query);
    else if (qpath[0]) snprintf(expr, sizeof(expr), "%s[%ld:%ld][]", qpath, off, off + lim);
    else return jerr("需要 path（数组路径）或 query（jq 表达式）");

    char *out = malloc(OUTMAX);
    if (!out) return NULL;
    if (run_jq_raw(expr, path, out, OUTMAX) != 0) {
        char m[600];
        snprintf(m, sizeof(m), "jq 失败：%.400s", out);
        free(out);
        return jerr(m);
    }

    int lines = 0;
    for (const char *p = out; *p; p++) if (*p == '\n') lines++;
    if (out[0] && out[strlen(out) - 1] != '\n') lines++;

    char *text = malloc(OUTMAX);
    if (!text) { free(out); return NULL; }
    snprintf(text, OUTMAX, "%s\n\n```json\n%s\n```\n", expr, out);

    char ans[300];
    snprintf(ans, sizeof(ans), "%s · %d 条", expr, lines);
    char extra[1200];
    snprintf(extra, sizeof(extra), "\"file\":\"%s\",\"offset\":%ld,\"limit\":%ld,\"lines\":%d",
             path, off, lim, lines);
    char *r = jpair(text, ans, extra);
    free(text);
    free(out);
    return r;
}

#include "json_table.inc"

/* ---------------- 入口 ---------------- */
static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));

    if (!strcmp(op, "probe") || !strcmp(op, "info") || !strcmp(op, "keys"))
        return op_probe(json_in);
    if (!strcmp(op, "get") || !strcmp(op, "jq") || !strcmp(op, "extract") || !strcmp(op, "page"))
        return op_get(json_in);
    if (!strcmp(op, "table") || !strcmp(op, "csv"))
        return op_table(json_in);
    if (!strcmp(op, "query"))
        return op_get(json_in);
    /* 没写 op：有 query/offset 当取值，只有 path 当探测 */
    if (!op[0]) {
        if (strstr(json_in, "\"query\"") || strstr(json_in, "\"offset\""))
            return op_get(json_in);
        return op_probe(json_in);
    }
    /* 写了 op 但不认识：不要静默当成 probe —— 那会去读一份无关的旧 json，
     * 输出一张看着很正常的表，比报错更难发现。 */
    {
        char m[400];
        snprintf(m, sizeof(m),
                 "不支持的 op：%s。jq 只认 probe / get / query / table / csv / keys / extract / page。"
                 "取字段用 {\"op\":\"query\",\"file\":\"x.json\",\"query\":\".name\"}；"
                 "看结构用 {\"op\":\"probe\",\"file\":\"x.json\"}", op);
        return jerr(m);
    }
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "jq",
    "本地 JSON 处理（板上 jq）：probe 探测结构(数组/条数/字段)、get 分批取值(path+offset/limit)、"
    "table 导出表格+全字段CSV、query 自定义表达式。不指定 file 就用最近抓到的 json",
    "{\"op\":\"probe\"}  |  {\"op\":\"get\",\"path\":\".data.rows\",\"offset\":0,\"limit\":5}"
    "  |  {\"op\":\"table\",\"limit\":15}  |  {\"op\":\"query\",\"query\":\".data.rows|map(.title)\"}",
    run,
    "jq,json,extract,probe,query,表格,csv,提取,字段"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
