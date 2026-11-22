#define _GNU_SOURCE
/* cgi.c — agent 的 HTTP 接口层（纯 C，去掉 python 版 CGI）
 *
 * 路由（和 python 版行为一致，页面不用改）：
 *   POST ?task       {"task":"..."}            → 起 agent-c 子进程，返回 job id
 *   GET  ?job=xxx                               → 读日志（done 靠 "[统计]" 判断）
 *   GET  ?env=1                                 → 环境能力 + 插件清单
 *   GET  ?file=xxx                              → 下载生成物
 *   POST ?upload=1   {"name":"x.pdf","data":"base64"} → 存文件 + 转文本
 *
 * 任务按日期分目录：/tmp/agent-jobs/YYYYMMDD/<jid>.log
 * 轮询时跨日查找（今天→昨天→保留期），23:59 起、00:01 查也能找到。
 */
#include <stdio.h>
#include <dlfcn.h>
#include "plugin.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <strings.h>
#include <ctype.h>

/* ===== 模型管理：查状态 + 在线下载 =====
 * 为什么放在 CGI：LuCI 的 CBI 是服务端渲染，页面自己没法下 2GB 文件；
 * 放在这里 htm 页面也能复用同一套接口。
 * 下载用 curl 写 <目标>.part，下完改名；进度 = .part 大小 / 预设总长（实测字节数）。 */
typedef struct { const char *key, *url, *dest; long long total; } mdl_t;

static const mdl_t MDLS[] = {
    {"qwen2.5-3b-q4_k_m",
     "https://modelscope.cn/models/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/master/qwen2.5-3b-instruct-q4_k_m.gguf",
     "/opt/llama/qwen2.5-3b-q4_k_m.gguf", 2104932768LL},
    {"qwen2.5-1.5b-q4_k_m",
     "https://modelscope.cn/models/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/master/qwen2.5-1.5b-instruct-q4_k_m.gguf",
     "/opt/llama/qwen2.5-1.5b-q4_k_m.gguf", 1117320672LL},
    {"qwen2.5-0.5b-q4_k_m",
     "https://modelscope.cn/models/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/master/qwen2.5-0.5b-instruct-q4_k_m.gguf",
     "/opt/llama/qwen2.5-0.5b-q4_k_m.gguf", 491400032LL},
    {NULL, NULL, NULL, 0}
};

static long long fsize_ll(const char *p) {
    struct stat st;
    return (stat(p, &st) == 0) ? (long long)st.st_size : -1;
}

static int pid_alive(long p) {
    if (p <= 0) return 0;
    return kill((pid_t)p, 0) == 0;
}

#define JOBS        "/tmp/agent-jobs"
#define WORK_DIR    "/tmp/llama_agent"
#define AGENT_C     "/opt/llama/agent/agent-c"
#define PLUGIN_DIR  "/opt/llama/agent/plugins"
#define KEEP_DAYS   3
#define MAX_BODY    (24 * 1024 * 1024)
/* 任务日志的尾部窗口。插件结果（表格 JSON）很容易超过 6KB，切在中间时
 * 前端 JSON.parse 必然失败，卡片就退化成"一条连在一起的文本"。 */
#define MAX_OUTPUT  32000

/* ---------------- 输出 ---------------- */
static void out_json(const char *fmt, ...);

/* 直接吐一段已经成形的 JSON（比如从 agent-c 代理过来的结果） */
static void out_raw(const char *json) {
    if (!json) json = "{}";
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
}

#include <stdarg.h>
static void out_json(const char *fmt, ...) {
    char *body = malloc(MAX_OUTPUT + 4096);
    if (!body) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, MAX_OUTPUT + 4096, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("Access-Control-Allow-Origin: *\r\n");
    printf("Content-Length: %d\r\n\r\n", n);
    fwrite(body, 1, (size_t)n, stdout);
    fflush(stdout);
    free(body);
}

/* 转义成 JSON 字符串内容（写进 buf） */
static size_t jesc(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (i + 8 >= cap) break;
        switch (*p) {
        case '"':  dst[i++] = '\\'; dst[i++] = '"';  break;
        case '\\': dst[i++] = '\\'; dst[i++] = '\\'; break;
        case '\n': dst[i++] = '\\'; dst[i++] = 'n';  break;
        case '\r': dst[i++] = '\\'; dst[i++] = 'r';  break;
        case '\t': dst[i++] = '\\'; dst[i++] = 't';  break;
        default:
            if (*p < 0x20) i += (size_t)snprintf(dst + i, cap - i, "\\u%04x", *p);
            else dst[i++] = (char)*p;
        }
    }
    dst[i] = 0;
    return i;
}

/* ---------------- 读请求体 ---------------- */
static char *read_body(size_t *len) {
    const char *cl = getenv("CONTENT_LENGTH");
    long n = cl ? atol(cl) : 0;
    if (n <= 0 || n > MAX_BODY) { *len = 0; return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { *len = 0; return NULL; }
    /* 管道上 fread 可能短读（大文件上传尤其明显），要读到读不动为止，
     * 否则 base64 只拿到前半截 → 解出来的文件是坏的 → 解析失败且没提示。 */
    size_t got = 0;
    while (got < (size_t)n) {
        size_t r = fread(buf + got, 1, (size_t)n - got, stdin);
        if (r == 0) break;
        got += r;
    }
    buf[got] = 0;
    *len = got;
    return buf;
}

/* 从扁平 JSON 取字符串 */
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

/* ---------------- 小工具 ---------------- */
static void today_str(char *buf, size_t cap, int delta_days) {
    time_t t = time(NULL) - (time_t)delta_days * 86400;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, cap, "%Y%m%d", &tm);
}

static int mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* URL 解码：中文文件名在 query 里是 %E4%B8%AD 这种，不解码就找不到文件 */
static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]
            && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

/* 按 job id 找日志（跨日） */
static int find_job(const char *jid, const char *ext, char *out, size_t cap) {
    for (int d = 0; d <= KEEP_DAYS; d++) {
        char day[16];
        today_str(day, sizeof(day), d);
        snprintf(out, cap, "%s/%s/%s%s", JOBS, day, jid, ext);
        if (access(out, F_OK) == 0) return 0;
    }
    return -1;
}

/* 清理：删超过保留期的日期目录 + 旧格式平铺文件 */
static void prune_jobs(void) {
    DIR *dp = opendir(JOBS);
    if (!dp) return;
    struct dirent *e;
    char today[16];
    today_str(today, sizeof(today), 0);
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", JOBS, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISREG(st.st_mode)) {          /* 旧格式平铺 → 删 */
            size_t L = strlen(e->d_name);
            if ((L > 4 && !strcmp(e->d_name + L - 4, ".log"))
                || (L > 5 && !strcmp(e->d_name + L - 5, ".task"))) {
                unlink(path);
            }
            continue;
        }
        if (!S_ISDIR(st.st_mode) || strlen(e->d_name) != 8) continue;
        if (strcmp(e->d_name, today) >= 0) continue;   /* 今天及以后不动 */
        /* 目录名是 yyyymmdd：算天数差 */
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (strptime(e->d_name, "%Y%m%d", &tm) == NULL) continue;
        time_t dt = mktime(&tm);
        if (dt <= 0) continue;
        if ((time(NULL) - dt) / 86400 > KEEP_DAYS) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
            if (system(cmd) != 0) { /* 忽略 */ }
        }
    }
    closedir(dp);
}

/* ---------------- 起任务 ---------------- */
static int job_running(pid_t *pid_out) {
    FILE *f = fopen(JOBS "/.running", "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) { fclose(f); return 0; }
    fclose(f);
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) { if (pid_out) *pid_out = (pid_t)pid; return 1; }
    return 0;
}

/* 停掉当前 agent 任务：读 .running 里的 pid，先 SIGTERM，
 * 2 秒还在就 SIGKILL（卡在 socket read 里的进程走不到清理路径）。
 * 前端"停止"按钮以前只 abort 了自己的 fetch，后端照跑 —— 用户点了没反应，
 * 任务还一直占着 .running，后面所有任务都被"已有任务在跑"挡住。 */
/* ?set_plugoff=music,batch → 写 /etc/llama.plugoff（一行一个）
 * 被停用的插件下次 agent 启动时就不加载了：prompt 里的工具表变短，
 * 3B 选错率下降，每步也快。 */
static void do_set_plugoff(const char *val) {
    FILE *f = fopen("/etc/llama.plugoff", "w");
    if (!f) {
        out_json("{\"ok\":0,\"error\":\"写不了 /etc/llama.plugoff（只读文件系统？）\"}");
        return;
    }
    int n = 0;
    const char *p = val;
    while (*p) {
        const char *c = strchr(p, ',');
        size_t L = c ? (size_t)(c - p) : strlen(p);
        /* 只收字母数字下划线，长度合理；其它一律丢掉，避免写进脏东西 */
        int ok = (L > 0 && L < 32);
        for (size_t i = 0; ok && i < L; i++) {
            char ch = p[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                  || (ch >= '0' && ch <= '9') || ch == '_'))
                ok = 0;
        }
        if (ok) {
            fwrite(p, 1, L, f);
            fputc('\n', f);
            n++;
        }
        if (!c) break;
        p = c + 1;
    }
    fclose(f);
    out_json("{\"ok\":1,\"disabled\":%d}", n);
}

/* 模型状态。fmt=text 时给 LuCI 页面用（省得页面里解 JSON），默认输出 JSON。 */
static void do_model_status(int as_text) {
    /* 必须先发 CGI 头，否则 uhttpd 直接回 "Bad Gateway / The process did not
     * produce any response"（内容本身是对的，只是缺 Content-Type）。 */
    printf(as_text ? "Content-Type: text/plain; charset=utf-8\r\n"
                   : "Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("Access-Control-Allow-Origin: *\r\n\r\n");

    char mpath[512] = {0};
    FILE *pp = popen("uci -q get llama.main.model 2>/dev/null", "r");
    if (pp) {
        if (fgets(mpath, sizeof(mpath), pp)) {
            size_t L = strcspn(mpath, "\r\n");
            mpath[L] = 0;
        }
        pclose(pp);
    }
    long long ms = mpath[0] ? fsize_ll(mpath) : -1;

    if (as_text) {
        printf("model=%s\n", mpath[0] ? mpath : "(未配置)");
        printf("exists=%d\n", ms > 0 ? 1 : 0);
        printf("size=%lld\n", ms > 0 ? ms : 0);
        for (int i = 0; MDLS[i].key; i++) {
            char part[600], pidf[640];
            snprintf(part, sizeof(part), "%s.part", MDLS[i].dest);
            snprintf(pidf, sizeof(pidf), "%s.pid", part);
            long long have = fsize_ll(MDLS[i].dest);
            long long ps = fsize_ll(part);
            long pid = 0;
            FILE *f = fopen(pidf, "r");
            if (f) { if (fscanf(f, "%ld", &pid) != 1) pid = 0; fclose(f); }
            printf("dl=%s total=%lld bytes=%lld pct=%d running=%d done=%d\n",
                   MDLS[i].key, MDLS[i].total, ps > 0 ? ps : 0,
                   MDLS[i].total ? (int)((ps > 0 ? ps : 0) * 100 / MDLS[i].total) : 0,
                   (ps >= 0 && pid_alive(pid)) ? 1 : 0, have > 0 ? 1 : 0);
        }
        return;
    }

    printf("{\"ok\":1,\"model\":{\"path\":\"%s\",\"exists\":%d,\"size\":%lld},\"downloads\":[",
           mpath, ms > 0 ? 1 : 0, ms > 0 ? ms : 0);
    for (int i = 0; MDLS[i].key; i++) {
        char part[600], pidf[640];
        snprintf(part, sizeof(part), "%s.part", MDLS[i].dest);
        snprintf(pidf, sizeof(pidf), "%s.pid", part);
        long long have = fsize_ll(MDLS[i].dest);
        long long ps = fsize_ll(part);
        long pid = 0;
        FILE *f = fopen(pidf, "r");
        if (f) { if (fscanf(f, "%ld", &pid) != 1) pid = 0; fclose(f); }
        printf("%s{\"key\":\"%s\",\"dest\":\"%s\",\"total\":%lld,\"bytes\":%lld,"
               "\"pct\":%d,\"running\":%d,\"done\":%d}",
               i ? "," : "", MDLS[i].key, MDLS[i].dest, MDLS[i].total,
               ps > 0 ? ps : 0,
               MDLS[i].total ? (int)((ps > 0 ? ps : 0) * 100 / MDLS[i].total) : 0,
               (ps >= 0 && pid_alive(pid)) ? 1 : 0, have > 0 ? 1 : 0);
    }
    printf("]}");
}

/* 启动下载（后台）。key 必须是 MDLS 里的名字。 */
static void do_model_dl(const char *key) {
    const mdl_t *m = NULL;
    for (int i = 0; MDLS[i].key; i++)
        if (!strcmp(MDLS[i].key, key)) { m = &MDLS[i]; break; }
    if (!m) { out_json("{\"ok\":0,\"error\":\"未知模型: %s\"}", key); return; }

    if (fsize_ll(m->dest) > 0) {
        out_json("{\"ok\":1,\"already\":1,\"dest\":\"%s\"}", m->dest);
        return;
    }
    char part[600], pidf[640], cmd[1600];
    snprintf(part, sizeof(part), "%s.part", m->dest);
    snprintf(pidf, sizeof(pidf), "%s.pid", part);

    /* 已经在跑就别再起一个：两个 curl 同时往同一个 .part 追加，下完就是
     * 一个大小对、内容坏的 GGUF（llama-server 加载直接失败，还得自己删）。
     * 重新点"开始下载"以前是静默再起一个进程，看不出来。 */
    {
        long rp = 0;
        FILE *rf = fopen(pidf, "r");
        if (rf) { if (fscanf(rf, "%ld", &rp) != 1) rp = 0; fclose(rf); }
        if (pid_alive(rp)) {
            out_json("{\"ok\":0,\"error\":\"这个模型正在下载中（pid %ld），别重复启动\",\"running\":1}", rp);
            return;
        }
    }

    /* 空间检查：2GB 模型写的是 overlay，塞满会把系统搞挂（overlay 写满后
     * 连 /etc/config 都存不进去）。已下了一部分的话按剩余量算。 */
    {
        long long have = fsize_ll(part);
        long long need = m->total - (have > 0 ? have : 0) + (256LL << 20);
        struct statfs sf;
        if (statfs("/opt/llama", &sf) == 0) {
            long long freeb = (long long)sf.f_bavail * (long long)sf.f_frsize;
            if (freeb < need) {
                out_json("{\"ok\":0,\"error\":\"空间不够：还需要 %.1f GB，/opt/llama 只剩 %.1f GB\"}",
                         need / 1073741824.0, freeb / 1073741824.0);
                return;
            }
        }
    }

    unlink(pidf);
    if (!strstr(m->dest, "/opt/llama/")) {
        out_json("{\"ok\":0,\"error\":\"目标路径异常\"}");
        return;
    }
    /* 直接 & 后台：busybox 没有 setsid 命令（用了会 not found 静默失败），
     * uhttpd 也不会回收 cgi 的子进程，所以普通后台就够。
     * 下完才改名，避免半截文件被当成模型加载。 */
    snprintf(cmd, sizeof(cmd),
             "sh -c 'echo $$ > %s; "
             "curl -L --retry 5 --retry-delay 3 --connect-timeout 20 -C - -o %s %s "
             "&& mv %s %s' >/dev/null 2>&1 &",
             pidf, part, m->url, part, m->dest);
    if (system(cmd) != 0) {
        out_json("{\"ok\":0,\"error\":\"启动下载失败\"}");
        return;
    }
    out_json("{\"ok\":1,\"started\":1,\"dest\":\"%s\",\"total\":%lld}", m->dest, m->total);
}

static void do_stop_job(void) {
    char pf[600];
    snprintf(pf, sizeof(pf), "%s/.running", JOBS);
    FILE *f = fopen(pf, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid <= 0 || kill((pid_t)pid, 0) != 0) {
        unlink(pf);
        out_json("{\"ok\":1,\"stopped\":0,\"note\":\"没有在跑的任务\"}");
        return;
    }
    kill((pid_t)pid, SIGTERM);
    for (int i = 0; i < 20; i++) {          /* 最多等 2 秒 */
        usleep(100000);
        if (kill((pid_t)pid, 0) != 0) break;
    }
    int hard = 0;
    if (kill((pid_t)pid, 0) == 0) { kill((pid_t)pid, SIGKILL); hard = 1; }
    unlink(pf);
    out_json("{\"ok\":1,\"stopped\":1,\"pid\":%d,\"hard\":%d}", pid, hard);
}

static void do_task(const char *body) {
    char task[8192] = {0};
    if (jget(body, "task", task, sizeof(task)) != 0 || !task[0]) {
        out_json("{\"ok\":0,\"error\":\"任务为空\"}");
        return;
    }
    if (access(AGENT_C, X_OK) != 0) {
        out_json("{\"ok\":0,\"error\":\"agent-c 不在 %s\"}", AGENT_C);
        return;
    }
    pid_t busy = 0;
    if (job_running(&busy)) {
        out_json("{\"ok\":0,\"error\":\"已有 agent 任务在跑（pid %d），llama-server 单 slot 并发只会互相排队\"}",
                 (int)busy);
        return;
    }

    mkdir_p(JOBS);
    char day[16], daydir[512];
    today_str(day, sizeof(day), 0);
    snprintf(daydir, sizeof(daydir), "%s/%s", JOBS, day);
    mkdir_p(daydir);

    unsigned int r = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, &r, sizeof(r)) != sizeof(r)) r = (unsigned)time(NULL); close(fd); }
    else r = (unsigned)time(NULL);
    char jid[16];
    snprintf(jid, sizeof(jid), "%08x", r);

    char taskf[600], logf[600];
    snprintf(taskf, sizeof(taskf), "%s/%s.task", daydir, jid);
    snprintf(logf, sizeof(logf), "%s/%s.log", daydir, jid);

    FILE *tf = fopen(taskf, "w");
    if (!tf) { out_json("{\"ok\":0,\"error\":\"写任务文件失败\"}"); return; }
    fputs(task, tf);
    fclose(tf);

    pid_t pid = fork();
    if (pid < 0) { out_json("{\"ok\":0,\"error\":\"fork 失败\"}"); return; }
    if (pid == 0) {
        /* 子进程：stdout/stderr 都进日志 */
        int lf = open(logf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); if (lf > 2) close(lf); }
        /* stderr 进单独的 .err：主日志保持干净（[mem]/加载信息不该被前端
         * 当成插件结果），出问题时还能翻 .err */
        {
            char erf[620];
            snprintf(erf, sizeof(erf), "%s.err", logf);
            int ef = open(erf, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (ef >= 0) { dup2(ef, 2); if (ef > 2) close(ef); }
        }
        int nf = open("/dev/null", O_RDONLY);
        if (nf >= 0) { dup2(nf, 0); if (nf > 2) close(nf); }
        char steps[16];
        snprintf(steps, sizeof(steps), "%d", 4);
        /* 记账开关：默认开，AGENT_MEMCHECK=0 关。
         * 每次任务在日志里留几行 [mem]，攒起来能看出同一类任务的分配是否漂移。 */
        const char *mc = getenv("AGENT_MEMCHECK");
        int memcheck = !(mc && !strcmp(mc, "0"));
        if (memcheck) {
            execl(AGENT_C, AGENT_C, "--task", task, "--plugins", PLUGIN_DIR,
                  "--max-steps", steps, "--memcheck", (char *)NULL);
        } else {
            execl(AGENT_C, AGENT_C, "--task", task, "--plugins", PLUGIN_DIR,
                  "--max-steps", steps, (char *)NULL);
        }
        _exit(127);
    }
    /* 父进程记录 pid（供互斥判断） */
    FILE *rf = fopen(JOBS "/.running", "w");
    if (rf) { fprintf(rf, "%d\n", (int)pid); fclose(rf); }

    out_json("{\"ok\":1,\"job\":\"%s\",\"task\":\"%s\",\"pid\":%d}", jid, "", (int)pid);
}

/* ---------------- 轮询 ---------------- */
static void do_poll(const char *jid) {
    char logf[600];
    if (find_job(jid, ".log", logf, sizeof(logf)) != 0) {
        out_json("{\"ok\":0,\"error\":\"任务不存在或已过期（保留 %d 天）\"}", KEEP_DAYS);
        return;
    }
    FILE *f = fopen(logf, "r");
    if (!f) { out_json("{\"ok\":0,\"error\":\"读日志失败\"}"); return; }
    char *txt = malloc(1 << 20);
    if (!txt) { fclose(f); out_json("{\"ok\":0,\"error\":\"内存不足\"}"); return; }
    size_t n = fread(txt, 1, (1 << 20) - 1, f);
    txt[n] = 0;
    fclose(f);

    /* 完成判定：agent 任务会打 [统计]；自动化插件不一定打，
     * 所以再加一条——进程已经退出就算完成。
     * 否则自动化流程跑完了页面还在转圈。 */
    int done = strstr(txt, "[统计]") != NULL;
    if (!done) {
        pid_t alive = 0;
        if (!job_running(&alive)) done = 1;
    }
    const char *tail = txt;
    if (n > MAX_OUTPUT) {
        tail = txt + n - MAX_OUTPUT;
        /* 退到 UTF-8 字符边界，否则开头会露出半个汉字（显示成 "�"） */
        while (*tail && ((unsigned char)*tail & 0xC0) == 0x80) tail++;
    }
    char *esc = malloc(strlen(tail) * 6 + 64);
    if (!esc) { free(txt); out_json("{\"ok\":0,\"error\":\"内存不足\"}"); return; }
    jesc(esc, strlen(tail) * 6 + 64, tail);
    out_json("{\"ok\":1,\"done\":%s,\"output\":\"%s\"}", done ? "true" : "false", esc);
    free(esc);
    free(txt);
}

/* ---------------- 环境 ---------------- */
static void do_env(void) {
    char buf[32768] = {0};      /* 插件多了之后 8K 会截断 */
    FILE *p = popen(AGENT_C " --env --plugins " PLUGIN_DIR " 2>/dev/null", "r");
    if (!p) { out_json("{\"ok\":0,\"error\":\"agent-c 执行失败\"}"); return; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    buf[n] = 0;
    pclose(p);
    /* agent-c 会先打插件加载信息（stderr），stdout 是 JSON 单行 */
    char *line = NULL, *save = NULL;
    for (char *t = strtok_r(buf, "\n", &save); t; t = strtok_r(NULL, "\n", &save)) {
        if (t[0] == '{') line = t;
    }
    if (!line) { out_json("{\"ok\":0,\"error\":\"环境信息解析失败\"}"); return; }
    out_json("{\"ok\":1,\"env\":%s}", line);
}

/* ---------------- 启动自动化插件 ---------------- */
/* 和提交任务同样的 job 机制：fork 一个 agent-c --auto，日志进 job 文件，
 * 前端用同一个 ?job= 轮询进度。自动化插件自己在 stdout 打印进度。 */
static void do_auto(const char *name, const char *args_json, int daemon_mode) {
    if (!name || !name[0]) { out_json("{\"ok\":0,\"error\":\"缺少 auto 名称\"}"); return; }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            out_json("{\"ok\":0,\"error\":\"auto 名称不合法\"}");
            return;
        }
    }
    if (!args_json || !args_json[0]) args_json = "{}";

    /* 已经在跑就直接回"已在运行"：必须在 fork 之前判断，
     * 否则会起出第二个进程，把 pid 文件覆盖掉，老的变孤儿还在后台转。 */
    {
        char pf[256];
        snprintf(pf, sizeof(pf), "/tmp/agent-auto/%s.pid", name);
        FILE *f = fopen(pf, "r");
        if (f) {
            int old = 0;
            int ok = (fscanf(f, "%d", &old) == 1);
            fclose(f);
            if (ok && old > 0 && kill((pid_t)old, 0) == 0) {
                out_json("{\"ok\":1,\"already\":1,\"running\":1,\"pid\":%d,"
                         "\"note\":\"%s 已经在运行（pid %d）\"}", old, name, old);
                return;
            }
        }
    }

    pid_t busy = 0;
    if (!daemon_mode && job_running(&busy)) {
        out_json("{\"ok\":0,\"error\":\"已有任务在跑（pid %d）\"}", (int)busy);
        return;
    }
    if (access(AGENT_C, X_OK) != 0) {
        out_json("{\"ok\":0,\"error\":\"agent-c 不可执行\"}");
        return;
    }

    mkdir_p(JOBS);
    char day[16], daydir[512];
    today_str(day, sizeof(day), 0);
    snprintf(daydir, sizeof(daydir), "%s/%s", JOBS, day);
    mkdir_p(daydir);

    unsigned int r = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, &r, sizeof(r)) != sizeof(r)) r = (unsigned)time(NULL); close(fd); }
    else r = (unsigned)time(NULL);
    char jid[16];
    snprintf(jid, sizeof(jid), "%08x", r);

    char logf[600];
    snprintf(logf, sizeof(logf), "%s/%s.log", daydir, jid);

    pid_t pid = fork();
    if (pid < 0) { out_json("{\"ok\":0,\"error\":\"fork 失败\"}"); return; }
    if (pid == 0) {
        /* 守护模式脱离 CGI 会话：CGI 进程退出后它继续跑，
         * 不受 uhttpd 的请求生命周期影响 */
        if (daemon_mode) setsid();
        int lf = open(logf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); if (lf > 2) close(lf); }
        /* stderr 进单独的 .err：主日志保持干净（[mem]/加载信息不该被前端
         * 当成插件结果），出问题时还能翻 .err */
        {
            char erf[620];
            snprintf(erf, sizeof(erf), "%s.err", logf);
            int ef = open(erf, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (ef >= 0) { dup2(ef, 2); if (ef > 2) close(ef); }
        }
        int nf = open("/dev/null", O_RDONLY);
        if (nf >= 0) { dup2(nf, 0); if (nf > 2) close(nf); }
        if (daemon_mode)
            execl(AGENT_C, AGENT_C, "--auto", name, "--args", args_json, "--daemon",
                  "--plugins", PLUGIN_DIR, (char *)NULL);
        else
            execl(AGENT_C, AGENT_C, "--auto", name, "--args", args_json,
                  "--plugins", PLUGIN_DIR, (char *)NULL);
        _exit(127);
    }
    /* 守护模式不占任务互斥锁：它长期在跑，不该把页面锁成"忙碌"、
     * 也不该让用户提交不了普通任务。它自己的 pid 在 /tmp/agent-auto/ */
    if (!daemon_mode) {
        FILE *rf = fopen(JOBS "/.running", "w");
        if (rf) { fprintf(rf, "%d\n", (int)pid); fclose(rf); }
    }
    out_json("{\"ok\":1,\"job\":\"%s\",\"auto\":\"%s\",\"pid\":%d,\"daemon\":%d}",
             jid, name, (int)pid, daemon_mode ? 1 : 0);
}

/* ---------------- 停止自动化 ---------------- */
static void do_auto_stop(const char *name) {
    if (!name || !name[0]) { out_json("{\"ok\":0,\"error\":\"缺少 auto 名称\"}"); return; }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            out_json("{\"ok\":0,\"error\":\"auto 名称不合法\"}");
            return;
        }
    }
    char cmd[600];
    snprintf(cmd, sizeof(cmd), AGENT_C " --auto-stop '%s' --plugins " PLUGIN_DIR " 2>/dev/null", name);
    char buf[1024] = {0};
    FILE *p = popen(cmd, "r");
    if (p) { size_t n = fread(buf, 1, sizeof(buf) - 1, p); buf[n] = 0; pclose(p); }
    char *line = NULL, *save = NULL, *last = NULL;
    for (char *t = strtok_r(buf, "\n", &save); t; t = strtok_r(NULL, "\n", &save))
        if (t[0] == '{') last = t;
    if (last) out_raw(last);
    else out_json("{\"ok\":0,\"error\":\"停止失败\"}");
}

/* ---------------- 忙闲状态 ---------------- */
/* 扫 /proc 找真正在跑的 agent-c。
 * 为什么不能只看 .running：只有 CGI 提交任务才写那个文件，
 * 从命令行直接跑 agent-c --task 时不写 → 明明在跑，页面却显示空闲。 */
static int find_agent_proc(pid_t *out) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    pid_t self = getpid();
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        pid_t p = (pid_t)atoi(e->d_name);
        if (p == self) continue;
        char path[64], buf[1024];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)p);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        /* 先按 argv[0] 的 basename 判断：命令行直接跑的叫 agent，
         * CGI 起的是 agent-c，两个都要认。用 basename 比 strstr 准，
         * 否则 "grep agent" 之类的进程也会被误判成忙。 */
        {
            const char *b = strrchr(buf, '/');
            b = b ? b + 1 : buf;
            if (strcmp(b, "agent") != 0 && strcmp(b, "agent-c") != 0) continue;
        }
        for (ssize_t i = 0; i < n; i++) if (buf[i] == '\0') buf[i] = ' ';
        if (strstr(buf, "--env") || strstr(buf, "--auto-list")) continue;  /* 查询类，不算忙 */
        if (strstr(buf, "--daemon")) continue;   /* 后台守护，不是用户等的任务 */
        if (out) *out = p;
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

/* 页面左上角那个（忙碌）/（空闲）就是问这个 */
static void do_status(void) {
    pid_t pid = 0;
    int busy = job_running(&pid);            /* 先看 .running（CGI 提交的任务） */
    if (!busy) busy = find_agent_proc(&pid); /* 再看真实进程（命令行直接跑的） */
    out_json("{\"ok\":1,\"busy\":%d,\"pid\":%d}", busy ? 1 : 0, busy ? (int)pid : 0);
}

/* ---------------- base64 解码 ---------------- */
static int b64d(const char *in, unsigned char *out, size_t cap) {
    static signed char tab[256];
    static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) tab[i] = -1;
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) tab[(unsigned char)a[i]] = (signed char)i;
        init = 1;
    }
    size_t n = 0;
    int acc = 0, bits = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && *p != '"'; p++) {
        int v = tab[*p];
        if (v < 0) continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) out[n++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return (int)n;
}

/* ---------------- 上传：存原文件 + 转文本 ---------------- */
/* ---------- 上传能力分层 ----------
 * 内置只认纯文本，完全不依赖任何外部库。
 * PDF/Word/Excel/PPT 属于"额外能力"：要 doc 插件装好，并且它依赖的
 * python 库也在。缺哪一层就说清缺哪一层，不要笼统报"上传失败"。 */
static int is_plain_text_ext(const char *ext) {
    static const char *e[] = {
        ".txt", ".md", ".markdown", ".csv", ".tsv", ".json", ".log",
        ".yaml", ".yml", ".xml", ".ini", ".conf", ".cfg", ".env",
        ".sh", ".py", ".c", ".h", ".js", ".css", ".html", ".htm",
        ".sql", ".m3u", ".m3u8", ".list", ".toml", ".properties", NULL
    };
    if (!ext) return 1;                        /* 没扩展名，当文本试 */
    for (int i = 0; e[i]; i++)
        if (!strcasecmp(ext, e[i])) return 1;
    return 0;
}

/* 扩展名 → 需要的 python 库；NULL 表示这个格式根本不支持 */
static const char *lib_for_ext(const char *ext) {
    if (!ext) return NULL;
    if (!strcasecmp(ext, ".pdf"))  return "pypdf";
    if (!strcasecmp(ext, ".docx")) return "docx";
    if (!strcasecmp(ext, ".xlsx") || !strcasecmp(ext, ".xlsm")) return "openpyxl";
    if (!strcasecmp(ext, ".pptx")) return "pptx";
    return NULL;
}

/* 库在不在：查 site-packages 目录，不起 python 进程 */
static int cgi_have_pymod(const char *mod) {
    static const char *dirs[] = {
        "/usr/lib/python3.13/site-packages", "/usr/lib/python3.12/site-packages",
        "/usr/lib/python3.11/site-packages", "/usr/lib/python3.10/site-packages",
        "/usr/lib/python3.9/site-packages",  "/usr/lib/python3.8/site-packages",
        "/usr/lib/python3/dist-packages",    "/usr/lib/python3/site-packages",
        "/usr/lib/python3.9/dist-packages",  "/usr/lib/python3.10/dist-packages",
        "/usr/lib/python3.11/dist-packages",
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
        snprintf(p, sizeof(p), "%s/%s.py", dirs[i], mod);
        if (access(p, F_OK) == 0) return 1;
    }
    return 0;
}

static void do_upload(const char *body) {
    char name[256] = {0};
    if (jget(body, "name", name, sizeof(name)) != 0 || !name[0]) {
        out_json("{\"ok\":0,\"error\":\"缺少文件名\"}");
        return;
    }
    const char *dp = strstr(body, "\"data\"");
    if (!dp) { out_json("{\"ok\":0,\"error\":\"缺少 data\"}"); return; }
    dp = strchr(dp, ':');
    if (!dp) { out_json("{\"ok\":0,\"error\":\"data 格式错\"}"); return; }
    dp = strchr(dp, '"');
    if (!dp) { out_json("{\"ok\":0,\"error\":\"data 格式错\"}"); return; }
    dp++;

    size_t cap = strlen(dp) / 4 * 3 + 16;
    unsigned char *raw = malloc(cap);
    if (!raw) { out_json("{\"ok\":0,\"error\":\"内存不足\"}"); return; }
    int rn = b64d(dp, raw, cap);
    if (rn <= 0) { free(raw); out_json("{\"ok\":0,\"error\":\"base64 解码失败\"}"); return; }

    char updir[512], path[768];
    snprintf(updir, sizeof(updir), "%s/uploads", WORK_DIR);
    mkdir_p(updir);
    snprintf(path, sizeof(path), "%s/%s", updir, name);

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); out_json("{\"ok\":0,\"error\":\"写文件失败\"}"); return; }
    fwrite(raw, 1, (size_t)rn, f);
    fclose(f);
    free(raw);

    /* 转文本：交给 doc 插件（它自己会报缺库），没有 doc 插件就直说 */
    char txtpath[800], esc_txt[4096];
    snprintf(txtpath, sizeof(txtpath), "%s.txt", path);
    snprintf(esc_txt, sizeof(esc_txt), "（未转换）");
    int txt_chars = 0;      /* 真正转出来的正文字节数；转不出就是 0，不是"（未转换）"的长度 */

    const char *dot = strrchr(name, '.');

    /* 第一层：内置纯文本。不需要任何插件、任何库，直接当文本用。 */
    if (is_plain_text_ext(dot)) {
        char esc_path0[2048], esc_name0[1024];
        jesc(esc_path0, sizeof(esc_path0), path);
        jesc(esc_name0, sizeof(esc_name0), name);
        /* 字段名要跟页面一致：页面读 j.name / j.file / j.chars */
        out_json("{\"ok\":1,\"name\":\"%s\",\"file\":\"%s\",\"path\":\"%s\","
                 "\"chars\":%d,\"bytes\":%d,\"kind\":\"text\",\"builtin\":1}",
                 esc_name0, esc_path0, esc_path0, rn, rn);
        return;
    }

    /* 第二层：二进制文档。必须有 doc 插件，且它依赖的库也在。 */
    const char *need = lib_for_ext(dot);
    if (!need) {
        unlink(path);
        char esc_ext[64] = {0};
        jesc(esc_ext, sizeof(esc_ext), dot ? dot : "");
        out_json("{\"ok\":0,\"error\":\"不支持 %s 这种格式。内置只支持纯文本"
                 "（txt/md/csv/json/log/yaml/xml/代码等）；"
                 "PDF/Word/Excel/PPT 需要 doc 插件\"}", esc_ext);
        return;
    }
    if (access("/opt/llama/agent/plugins/doc.so", R_OK) != 0) {
        unlink(path);
        out_json("{\"ok\":0,\"error\":\"不支持 %s：没装 doc 插件。"
                 "纯文本可以直接传；要解析这类文档请先装上 doc 插件\"}",
                 dot);
        return;
    }
    if (!cgi_have_pymod(need)) {
        unlink(path);
        out_json("{\"ok\":0,\"error\":\"不支持 %s：doc 插件已装，但缺 Python 库 %s。"
                 "板上装：pip install %s\"}", dot, need, need);
        return;
    }

    /* 提取正文：直接调 doc 插件（dlopen），不再走 extract.py。
     * 插件返回 {"text":"...","chars":N}，取 text 写成 .txt 伴生文件。 */
    {
        const char *ph = "/opt/llama/agent/plugins/doc.so";
        void *dh = dlopen(ph, RTLD_NOW);
        if (dh) {
            plugin_desc_t *(*dentry)(void) =
                (plugin_desc_t *(*)(void))dlsym(dh, "plugin_entry");
            if (dentry) {
                plugin_desc_t *dp = dentry();
                char dreq[800];
                snprintf(dreq, sizeof(dreq), "{\"path\":\"%s\"}", path);
                char *dres = dp ? dp->run(dreq) : NULL;
                if (dres) {
                    char derr[128];
                    jval *dj = json_parse(dres, derr, sizeof(derr));
                    const char *dtxt = dj ? json_str(json_get(dj, "text")) : NULL;
                    if (dtxt && dtxt[0]) {
                        FILE *of = fopen(txtpath, "w");
                        if (of) { fwrite(dtxt, 1, strlen(dtxt), of); fclose(of); }
                        txt_chars = (int)strlen(dtxt);
                    }
                    if (dj) json_free(dj);
                    free(dres);
                }
            }
            dlclose(dh);
        }
    }

    /* 输出：字段名跟页面一致（name/file/chars），并标明这次是插件解出来的 */
    char esc_path[2048], esc_txtp[2048], esc_name[1024], esc_ext[64] = {0};
    jesc(esc_path, sizeof(esc_path), path);
    jesc(esc_txtp, sizeof(esc_txtp), txtpath);
    jesc(esc_name, sizeof(esc_name), name);
    if (dot) jesc(esc_ext, sizeof(esc_ext), dot + 1);
    int chars = txt_chars;      /* 转出来的文本长度（字节，转不出=0） */
    out_json("{\"ok\":1,\"name\":\"%s\",\"file\":\"%s\",\"path\":\"%s\","
             "\"chars\":%d,\"bytes\":%d,\"kind\":\"%s\",\"plugin\":\"doc\"}",
             esc_name, esc_txtp, esc_path, chars, rn, esc_ext);
}

/* ---------------- 下载 ---------------- */
static void do_file(const char *fname) {
    /* 只取文件名，防目录穿越 */
    const char *base = strrchr(fname, '/');
    base = base ? base + 1 : fname;

    char cand[24][700];
    int nc = 0;
    for (int d = 0; d <= KEEP_DAYS; d++) {
        char day[16];
        today_str(day, sizeof(day), d);
        snprintf(cand[nc++], 700, "%s/%s/%s", WORK_DIR, day, base);
    }
    snprintf(cand[nc++], 700, "%s/%s", WORK_DIR, base);
    snprintf(cand[nc++], 700, "%s/uploads/%s", WORK_DIR, base);
    /* curl 抓回来的响应、doc 生成的文件都在这些子目录里，
     * 少列一条就是"有下载按钮但点了说文件不存在"。 */
    snprintf(cand[nc++], 700, "%s/downloads/%s", WORK_DIR, base);
    snprintf(cand[nc++], 700, "%s/gen/%s", WORK_DIR, base);
    /* 视频产物在磁盘上（/tmp 是 tmpfs，放不下大文件） */
    snprintf(cand[nc++], 700, "/opt/llama/out/%s", base);
    /* browser 插件失败时截的现场图 */
    snprintf(cand[nc++], 700, "%s/shots/%s", WORK_DIR, base);
    for (int d = 0; d <= KEEP_DAYS; d++) {
        char day[16];
        today_str(day, sizeof(day), d);
        snprintf(cand[nc++], 700, "%s/downloads/%s/%s", WORK_DIR, day, base);
        snprintf(cand[nc++], 700, "%s/gen/%s/%s", WORK_DIR, day, base);
    }

    const char *hit = NULL;
    for (int i = 0; i < nc; i++) {
        if (access(cand[i], R_OK) == 0) { hit = cand[i]; break; }
    }
    if (!hit) {
        out_json("{\"ok\":0,\"error\":\"文件不存在: %s\"}", base);
        return;
    }

    const char *ext = strrchr(base, '.');
    const char *ct = "application/octet-stream";
    int is_img = 0;
    if (ext) {
        if (!strcasecmp(ext, ".mid") || !strcasecmp(ext, ".midi")) ct = "audio/midi";
        else if (!strcasecmp(ext, ".mp3")) ct = "audio/mpeg";
        else if (!strcasecmp(ext, ".wav")) ct = "audio/wav";
        else if (!strcasecmp(ext, ".pptx")) ct = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
        else if (!strcasecmp(ext, ".png")) { ct = "image/png"; is_img = 1; }
        else if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) { ct = "image/jpeg"; is_img = 1; }
        else if (!strcasecmp(ext, ".gif")) { ct = "image/gif"; is_img = 1; }
        else if (!strcasecmp(ext, ".webp")) { ct = "image/webp"; is_img = 1; }
        else if (!strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".m4v")) { ct = "video/mp4"; is_img = 1; }
        else if (!strcasecmp(ext, ".webm")) { ct = "video/webm"; is_img = 1; }
        else if (!strcasecmp(ext, ".mov")) { ct = "video/quicktime"; is_img = 1; }
        else if (!strcasecmp(ext, ".mkv")) { ct = "video/x-matroska"; is_img = 1; }
        else if (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".md") || !strcasecmp(ext, ".py"))
            ct = "text/plain; charset=utf-8";
    }
    struct stat st;
    if (stat(hit, &st) != 0) { out_json("{\"ok\":0,\"error\":\"stat 失败\"}"); return; }
    /* ---------- Range 支持 ----------
     * <video> 拖动进度条、Safari 播 mp4 都要求服务器能回 206 Partial Content。
     * 之前只会整文件 200，表现是"能点开但拖不动 / 手机上黑屏"。 */
    long long start = 0, end = st.st_size - 1;
    int partial = 0;
    const char *range = getenv("HTTP_RANGE");
    if (range && !strncmp(range, "bytes=", 6)) {
        const char *r = range + 6;
        long long s2 = -1, e2 = -1;
        if (*r == '-') {                       /* bytes=-N 最后 N 字节 */
            long long n2 = atoll(r + 1);
            if (n2 > 0) { s2 = st.st_size - n2; if (s2 < 0) s2 = 0; e2 = st.st_size - 1; }
        } else {
            s2 = atoll(r);
            const char *d = strchr(r, '-');
            if (d && d[1]) e2 = atoll(d + 1);
            else e2 = st.st_size - 1;
        }
        if (s2 >= 0 && s2 < st.st_size && e2 >= s2) {
            if (e2 >= st.st_size) e2 = st.st_size - 1;
            start = s2;
            end = e2;
            partial = 1;
        }
    }

    if (partial)
        printf("Status: 206 Partial Content\r\n");
    printf("Content-Type: %s\r\n", ct);
    /* 图片/视频给 inline —— 页面里要直接 <img>/<video src>；其它类型仍走下载 */
    printf("Content-Disposition: %s; filename=\"%s\"\r\n",
           is_img ? "inline" : "attachment", base);
    printf("Accept-Ranges: bytes\r\n");
    if (partial)
        printf("Content-Range: bytes %lld-%lld/%lld\r\n",
               start, end, (long long)st.st_size);
    printf("Content-Length: %lld\r\n", end - start + 1);
    printf("Cache-Control: no-store\r\n\r\n");
    fflush(stdout);

    FILE *f = fopen(hit, "rb");
    if (!f) return;
    if (fseeko(f, (off_t)start, SEEK_SET) != 0) { fclose(f); return; }
    long long left = end - start + 1;
    char buf[32768];
    while (left > 0) {
        size_t want = (size_t)(left > (long long)sizeof(buf) ? (long long)sizeof(buf) : left);
        size_t n = fread(buf, 1, want, f);
        if (n == 0) break;
        fwrite(buf, 1, n, stdout);
        left -= (long long)n;
    }
    fclose(f);
    fflush(stdout);
}

/* ---------------- main ---------------- */
int main(void) {
    const char *method = getenv("REQUEST_METHOD");
    const char *qs = getenv("QUERY_STRING");
    if (!method) method = "GET";
    if (!qs) qs = "";

    /* ?vprog=<id> —— 查视频后台任务的进度。
     * 这是给页面进度条用的轻量接口：不经过模型、不占 agent 单任务锁，
     * 每秒轮询一次也就读两个小文件。 */
    if (strncmp(qs, "vprog=", 6) == 0) {
        char id[80];
        snprintf(id, sizeof(id), "%s", qs + 6);
        char *amp = strchr(id, '&');
        if (amp) *amp = 0;

        char jp[256], pp[256];
        snprintf(jp, sizeof(jp), "/tmp/llama_agent/vjobs/%s.json", id);
        snprintf(pp, sizeof(pp), "/tmp/llama_agent/vjobs/%s.prog", id);

        FILE *f = fopen(jp, "r");
        if (!f) { out_json("{\"ok\":0,\"error\":\"no such job\"}"); return 0; }
        char jb[600] = {0};
        size_t jn = fread(jb, 1, sizeof(jb) - 1, f);
        jb[jn] = 0;
        fclose(f);

        int pid = 0;
        double total = 0, done = 0;
        int ended = 0;
        char out[512] = {0};
        const char *q;
        if ((q = strstr(jb, "\"pid\":"))) pid = atoi(q + 6);
        if ((q = strstr(jb, "\"total\":"))) total = atof(q + 8);
        if ((q = strstr(jb, "\"out\":\""))) {
            q += 7;
            size_t i = 0;
            while (*q && *q != '"' && i + 1 < sizeof(out)) out[i++] = *q++;
            out[i] = 0;
        }
        /* desub-rk3566 不写 ffmpeg 的 -progress 文件，它每 25 帧往 stderr 打
         * 一行 "  N frames"。这里退化成解析日志换算进度。 */
        double fps = 25.0;
        if ((q = strstr(jb, "\"fps\":"))) fps = atof(q + 6);
        if (fps < 1) fps = 25.0;

        f = fopen(pp, "r");
        if (!f) {
            char lp[256];
            snprintf(lp, sizeof(lp), "/tmp/llama_agent/vjobs/%s.log", id);
            FILE *lf = fopen(lp, "r");
            if (lf) {
                char line[256];
                long last = 0;
                while (fgets(line, sizeof(line), lf)) {
                    char *e;
                    long v = strtol(line, &e, 10);
                    if (e != line && v > 0 && strstr(e, "frames")) last = v;
                    if (strstr(line, "done:")) ended = 1;   /* desub-rk3566 的完成标记 */
                }
                fclose(lf);
                if (last > 0) done = (double)last / fps;
            }
        }
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "out_time_us=", 12)) done = atof(line + 12) / 1000000.0;
                else if (!strncmp(line, "out_time_ms=", 12)) done = atof(line + 12) / 1000000.0;
                if (strstr(line, "progress=end")) ended = 1;
            }
            fclose(f);
        }
        int alive = 0;
        if (pid > 0) {
            char kp[64];
            snprintf(kp, sizeof(kp), "/proc/%d", pid);
            alive = (access(kp, F_OK) == 0);
        }
        struct stat st;
        long long sz = (stat(out, &st) == 0) ? (long long)st.st_size : 0;
        int pct = (total > 0) ? (int)(done * 100.0 / total) : (ended ? 100 : 0);
        if (pct > 100) pct = 100;
        const char *state = ended ? "done" : (alive ? "running" : "stopped");
        char src[512] = {0};
        if ((q = strstr(jb, "\"src\":\""))) {
            q += 7;
            size_t i = 0;
            while (*q && *q != '"' && i + 1 < sizeof(src)) src[i++] = *q++;
            src[i] = 0;
        }
        out_json("{\"ok\":1,\"pct\":%d,\"state\":\"%s\",\"done_sec\":%.1f,"
                 "\"total_sec\":%.1f,\"size\":%lld,\"out\":\"%s\",\"src\":\"%s\"}",
                 pct, state, done, total, sz, out, src);
        return 0;
    }

    /* 临时诊断：?envdump=1 打出所有 CGI 环境变量 —— 用来确认 uhttpd 到底
     * 有没有把 Range 这类请求头传给 CGI */
    if (strstr(qs, "envdump=1")) {
        printf("Content-Type: text/plain\r\n\r\n");
        extern char **environ;
        for (char **e = environ; *e; e++)
            if (!strncmp(*e, "HTTP_", 5) || !strncmp(*e, "REQUEST_", 8))
                printf("%s\n", *e);
        return 0;
    }

    mkdir_p(JOBS);
    prune_jobs();

    /* 解析 query（只用一层 ?a=b&c=d） */
    char q[1024];
    snprintf(q, sizeof(q), "%s", qs);
    char *pjob = NULL, *penv = NULL, *pfile = NULL, *pupload = NULL, *pstat = NULL;
    char *pauto = NULL, *pargs = NULL, *pautolist = NULL, *pdaemon = NULL, *pautostop = NULL;
    char *pautostate = NULL;
    char *save = NULL;
    for (char *t = strtok_r(q, "&", &save); t; t = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        url_decode(v);
        if (!strcmp(t, "job")) pjob = v;
        else if (!strcmp(t, "env")) penv = v;
        else if (!strcmp(t, "file")) pfile = v;
        else if (!strcmp(t, "upload")) pupload = v;
        else if (!strcmp(t, "status")) pstat = v;
        else if (!strcmp(t, "auto")) pauto = v;
        else if (!strcmp(t, "args")) pargs = v;
        else if (!strcmp(t, "auto_list")) pautolist = v;
        else if (!strcmp(t, "daemon")) pdaemon = v;
        else if (!strcmp(t, "auto_stop")) pautostop = v;
        else if (!strcmp(t, "auto_state")) pautostate = v;
    }

    if (pautolist && !strcmp(pautolist, "1")) {
        /* 代理给 agent-c --auto-list */
        char buf[8192] = {0};
        FILE *p = popen(AGENT_C " --auto-list --plugins " PLUGIN_DIR " 2>/dev/null", "r");
        if (p) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, p);
            buf[n] = 0;
            pclose(p);
        }
        char *line = NULL, *save2 = NULL, *last = NULL;
        for (char *t2 = strtok_r(buf, "\n", &save2); t2; t2 = strtok_r(NULL, "\n", &save2))
            if (t2[0] == '{') last = t2;
        if (last) {
            printf("Content-Type: application/json; charset=utf-8\r\n"
                   "Cache-Control: no-store\r\nContent-Length: %zu\r\n\r\n%s",
                   strlen(last), last);
            fflush(stdout);
        } else {
            out_json("{\"ok\":0,\"error\":\"取自动化清单失败\"}");
        }
        return 0;
    }
    if (pautostate && !strcmp(pautostate, "1")) {
        char buf[2048] = {0};
        FILE *p = popen(AGENT_C " --auto-state --plugins " PLUGIN_DIR " 2>/dev/null", "r");
        if (p) { size_t n = fread(buf, 1, sizeof(buf) - 1, p); buf[n] = 0; pclose(p); }
        char *line = NULL, *save2 = NULL, *last = NULL;
        for (char *t2 = strtok_r(buf, "\n", &save2); t2; t2 = strtok_r(NULL, "\n", &save2))
            if (t2[0] == '{') last = t2;
        if (last) out_raw(last);
        else out_json("{\"ok\":0,\"error\":\"取状态失败\"}");
        return 0;
    }
    if (pautostop && pautostop[0]) { do_auto_stop(pautostop); return 0; }
    if (pauto && pauto[0]) {
        do_auto(pauto, pargs, (pdaemon && !strcmp(pdaemon, "1")) ? 1 : 0);
        return 0;
    }
    if (pstat && !strcmp(pstat, "1")) { do_status(); return 0; }
    if (penv && !strcmp(penv, "1")) { do_env(); return 0; }
    if (pfile && pfile[0])          { do_file(pfile); return 0; }
    if (pjob && pjob[0])            { do_poll(pjob); return 0; }
    {
        const char *pj = getenv("QUERY_STRING");
        if (pj && strstr(pj, "stop_job=")) { do_stop_job(); return 0; }
        if (pj && strstr(pj, "set_plugoff=")) {
            const char *v = strstr(pj, "set_plugoff=") + 12;
            char pv[512];
            size_t pl = strcspn(v, "&");
            if (pl >= sizeof(pv)) pl = sizeof(pv) - 1;
            memcpy(pv, v, pl);
            pv[pl] = 0;
            /* 前端用 encodeURIComponent 拼名单时逗号会变成 %2C；不解码的话
             * "jq%2Cbatch%2Cmusic" 会被当成一个非法名字整条丢掉 —— 表现就是
             * 只能关一个插件，关第二个时文件被清空。 */
            url_decode(pv);
            do_set_plugoff(pv);
            return 0;
        }
        /* 模型管理（LuCI 设置页和 htm 都用）：
         *   ?model_status=1        查当前模型 + 各预设的下载状态（加 &fmt=text 给 LuCI）
         *   ?model_dl=<key>        后台下载某个预设模型 */
        if (pj && strstr(pj, "model_status=")) {
            do_model_status(strstr(pj, "fmt=text") != NULL);
            return 0;
        }
        if (pj && strstr(pj, "model_dl=")) {
            const char *v = strstr(pj, "model_dl=") + 9;
            char mk[80];
            size_t ml = strcspn(v, "&");
            if (ml >= sizeof(mk)) ml = sizeof(mk) - 1;
            memcpy(mk, v, ml);
            mk[ml] = 0;
            url_decode(mk);
            do_model_dl(mk);
            return 0;
        }
    }

    if (!strcasecmp(method, "POST")) {
        size_t blen = 0;
        char *body = read_body(&blen);
        if (!body) { out_json("{\"ok\":0,\"error\":\"请求体为空或过大\"}"); return 0; }
        if (pupload && !strcmp(pupload, "1")) do_upload(body);
        else do_task(body);
        free(body);
        return 0;
    }

    out_json("{\"ok\":0,\"error\":\"不支持的方法\"}");
    return 0;
}
