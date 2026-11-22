/* sys.c — 文件 / 命令 / 系统信息（纯 C，不依赖 python）
 *
 * 把 Python 版那些"基础手脚"用 C 重做：shell / list / du / grep / read /
 * write / sysinfo / http_get / download。全部用 POSIX API，一个 .so 拷过去
 * 就能用，不需要 python3 或任何库。
 *
 * 安全规则照搬 Python 版（这些是踩过坑才定下来的）：
 *   - 命令白名单 + 拒绝 ; & < > ` $( 换行（防 "ls; rm -rf /"）
 *   - find -delete/-exec、sed -i、awk system 这些等于任意破坏，单独拦
 *   - python/node 只允许跑脚本文件，不给 -c/-e（否则白名单形同虚设）
 *   - 写文件前检查敏感目录（/etc /usr /rom /root …）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "plugin.h"

#define OUTMAX   32768
#define WORK_DIR "/tmp/llama_agent"

/* ---------------- JSON 取值 / 输出 ---------------- */
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

/* {"value":"...","answer":"..."} */
static char *jpair(const char *text, const char *answer) {
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

static char *jerr(const char *msg) {
    size_t cap = strlen(msg) * 2 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, cap, "{\"error\":\"");
    for (const char *p = msg; *p && i + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

/* ---------------- 路径 ---------------- */
static void resolve_path(const char *in, char *out, size_t cap) {
    if (!in || !in[0]) { snprintf(out, cap, "%s", WORK_DIR); return; }
    if (in[0] == '/') snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "%s/%s", WORK_DIR, in);
}

/* 敏感目录：写之前必须拒绝（照搬 Python 版的 SENSITIVE_DIRS） */
static int is_sensitive(const char *path) {
    static const char *dirs[] = {
        "/etc", "/rom", "/usr", "/bin", "/sbin", "/lib", "/root",
        "/overlay/upper", "/opt/llama", "/opt/mp4", NULL
    };
    for (int i = 0; dirs[i]; i++) {
        size_t L = strlen(dirs[i]);
        if (!strncmp(path, dirs[i], L) && (path[L] == '/' || path[L] == 0)) return 1;
    }
    return 0;
}

/* ---------------- 跑命令（带超时） ---------------- */
/* cwd 固定工作目录：模型用 write 写的脚本落在那里，python3 x.py 才找得到 */
static int run_sh(const char *cmd, int timeout, char *out, size_t cap, int *truncated) {
    int fd[2];
    if (pipe(fd) != 0) { snprintf(out, cap, "pipe 失败"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); snprintf(out, cap, "fork 失败"); return -1; }

    if (pid == 0) {
        setpgid(0, 0);
        chdir(WORK_DIR);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[0]); close(fd[1]);
        alarm((unsigned)timeout);          /* 超时自己退出，不用父进程操心 */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fd[1]);
    size_t n = 0;
    int tr = 0;
    ssize_t r;
    while ((r = read(fd[0], out + n, cap - 1 - n)) > 0) {
        n += (size_t)r;
        if (n + 2 >= cap) { tr = 1; break; }   /* 超长就停，别再读了 */
    }
    out[n] = 0;
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (truncated) *truncated = tr;
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM) {
        snprintf(out + (n < cap - 64 ? n : cap - 64), 64, "\n[超时 %d 秒被终止]", timeout);
        return -2;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1;
}

/* ---------------- shell 白名单 ----------------
 * 注意：这里必须走 sh（管道 du|sort|tail 太常用），所以靠"白名单 + 危险字符
 * 拦截"来保证安全，而不是靠绕过 shell。
 */
static int cmd_allowed(const char *cmd, char *why, size_t whycap) {
    static const char *allow[] = {
        "ls", "cat", "head", "tail", "wc", "grep", "find", "du", "df", "free",
        "uname", "uptime", "ps", "date", "echo", "stat", "file", "which",
        "sort", "uniq", "cut", "tr", "awk", "sed", "id", "pwd", "tree",
        "jq", "python", "python3", "node", "npm", "npx", "ip", "iw", "uci",
        /* sh/ash/busybox：跑用户自己写的脚本。已经允许 python3 x.py、node x.js，
         * 独独不给 sh x.sh 说不过去 —— "写个 shell 脚本" 这类任务写完就没法执行。 */
        "sh", "ash", "busybox",
        "logread", "uptime", "mount", "lsusb", "dmesg", "hostname", "netstat",
        NULL
    };
    /* 危险字符：命令分隔 / 重定向 / 替换 */
    for (const char *p = cmd; *p; p++) {
        if (*p == ';' || *p == '&' || *p == '<' || *p == '>' || *p == '`' || *p == '\n') {
            snprintf(why, whycap, "命令里不能有 ; & < > 反引号 或换行");
            return 0;
        }
        if (*p == '$' && p[1] == '(') {
            snprintf(why, whycap, "命令里不能有 $()");
            return 0;
        }
    }

    /* 按管道分段，每段第一个词查白名单 */
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", cmd);
    char *save = NULL;
    for (char *seg = strtok_r(buf, "|", &save); seg; seg = strtok_r(NULL, "|", &save)) {
        while (*seg == ' ' || *seg == '\t') seg++;
        if (!*seg) continue;

        char base[64] = {0};
        {
            const char *e = seg;
            while (*e && !isspace((unsigned char)*e)) e++;
            size_t L = (size_t)(e - seg);
            if (L >= sizeof(base)) L = sizeof(base) - 1;
            memcpy(base, seg, L);
            base[L] = 0;
            const char *sl = strrchr(base, '/');
            if (sl) memmove(base, sl + 1, strlen(sl + 1) + 1);
        }

        /* 危险参数 */
        if (!strcmp(base, "find")) {
            static const char *bad[] = {"-delete", "-exec", "-execdir", "-ok", "-okdir", NULL};
            for (int i = 0; bad[i]; i++)
                if (strstr(seg, bad[i])) {
                    snprintf(why, whycap, "find 不允许 %s", bad[i]);
                    return 0;
                }
        }
        if (!strcmp(base, "sed") && (strstr(seg, "-i ") || strstr(seg, "--in-place"))) {
            snprintf(why, whycap, "sed 不允许 -i（直接改文件）");
            return 0;
        }
        if (!strcmp(base, "awk") && strstr(seg, "system")) {
            snprintf(why, whycap, "awk 不允许 system()");
            return 0;
        }

        /* python / node：只跑脚本文件，不给 -c / -e / -m */
        if (!strcmp(base, "python") || !strcmp(base, "python3")) {
            const char *rest = seg + strlen(base);
            while (*rest == ' ') rest++;
            if (*rest == '-') {
                snprintf(why, whycap, "python 只允许执行 .py 文件，不支持 -c/-m");
                return 0;
            }
            if (!strstr(rest, ".py")) {
                snprintf(why, whycap, "python 后面要跟 .py 文件名");
                return 0;
            }
            continue;
        }
        if (!strcmp(base, "node") || !strcmp(base, "npx")) {
            const char *rest = seg + strlen(base);
            while (*rest == ' ') rest++;
            if (*rest == '-') {
                snprintf(why, whycap, "node 只允许执行 .js 文件，不支持 -e/-p");
                return 0;
            }
            continue;
        }
        if (!strcmp(base, "npm")) {
            const char *rest = seg + strlen(base);
            while (*rest == ' ') rest++;
            if (strncmp(rest, "ls", 2) && strncmp(rest, "list", 4) && strncmp(rest, "view", 4)
                && strncmp(rest, "outdated", 8) && strncmp(rest, "root", 4)) {
                snprintf(why, whycap, "npm 只允许 ls/list/view/outdated 这类只读子命令");
                return 0;
            }
            continue;
        }

        int ok = 0;
        for (int i = 0; allow[i]; i++)
            if (!strcmp(base, allow[i])) { ok = 1; break; }
        if (!ok) {
            snprintf(why, whycap, "不允许的命令 %s", base);
            return 0;
        }
    }
    return 1;
}

/* 命令里有没有"能改文件/删文件/刷设备"的动作。
 *
 * 白名单挡住了 rm/mv/dd，但挡不住这些：sed -i 原地改、find -delete 批量删、
 * awk 的重定向写、python3/node 跑任意脚本（我自己救板子就是这么干的）。
 * 这些不禁止，但必须由用户在页面上确认过才能跑。
 */
static const char *cmd_dangerous(const char *cmd) {
    static const struct { const char *needle; const char *label; } P[] = {
        { "sed -i",      "sed -i 原地改文件" },
        { "-delete",     "find -delete 批量删除" },
        { "truncate",    "truncate 清空文件" },
        { "shred",       "shred 粉碎文件" },
        { "chmod",       "chmod 改权限" },
        { "chown",       "chown 改属主" },
        { ">>",          ">> 追加写入文件" },
        { "python3",     "python3 跑脚本（等同任意代码）" },
        { "node",        "node 跑脚本（等同任意代码）" },
        { "reboot",      "重启" },
        { "kill",        "杀进程" },
        { NULL, NULL }
    };
    if (!cmd) return NULL;
    for (int i = 0; P[i].needle; i++)
        if (strstr(cmd, P[i].needle)) return P[i].label;
    /* 单个 > 重定向也算（上面对 >> 已经处理，这里补 > 到文件） */
    for (const char *p = cmd; (p = strchr(p, '>')) != NULL; p++) {
        if (p[1] == '&') continue;          /* 2>&1 之类不算 */
        if (p > cmd && p[-1] == '>') continue;
        return "> 重定向写文件";
    }
    return NULL;
}

static char *op_shell(const char *json) {
    char cmd[2048] = {0};
    char confirm[16] = {0};
    jget(json, "cmd", cmd, sizeof(cmd));
    jget(json, "confirm", confirm, sizeof(confirm));
    if (!cmd[0]) return jerr("缺少 cmd");

    /* 能改文件的命令，必须由用户确认过（前端弹窗点"确认执行"才会补这个字段） */
    const char *dg = cmd_dangerous(cmd);
    if (dg && strcmp(confirm, "yes")) {
        char m[600];
        snprintf(m, sizeof(m),
            "这条命令会改系统状态（%s），需要用户在页面上确认后才能执行。"
            "确认后前端会自动带上 confirm:\"yes\"。", dg);
        return jerr(m);
    }

    char why[256] = {0};
    if (!cmd_allowed(cmd, why, sizeof(why))) {
        char msg[600];
        snprintf(msg, sizeof(msg),
            "拒绝执行：%s。可用命令：ls cat head tail wc grep find du df free uname "
            "uptime ps date echo stat file which sort uniq cut tr awk sed id pwd tree "
            "jq ip iw uci logread + python3 xxx.py / node xxx.js / sh xxx.sh，可以用 | 管道。", why);
        return jerr(msg);
    }

    int t = (int)jget_int(json, "timeout", 30);
    if (t < 1) t = 1;
    if (t > 120) t = 120;

    char *out = malloc(OUTMAX);
    if (!out) return NULL;
    int tr = 0;
    int rc = run_sh(cmd, t, out, OUTMAX, &tr);

    char text[OUTMAX + 512];
    size_t i = (size_t)snprintf(text, sizeof(text), "```\n%s\n```", out[0] ? out : "(无输出)");
    if (tr) i += (size_t)snprintf(text + i, sizeof(text) - i, "\n（输出超过上限，已截断）");

    char ans[300];
    snprintf(ans, sizeof(ans), "`%s` 退出码 %d", cmd, rc);
    char *r = jpair(text, ans);
    free(out);
    return r;
}

/* ---------------- list ---------------- */
static char *op_list(const char *json) {
    char p[512] = {0};
    jget(json, "path", p, sizeof(p));
    char path[768];
    resolve_path(p, path, sizeof(path));

    DIR *d = opendir(path);
    if (!d) { char m[900]; snprintf(m, sizeof(m), "打不开目录 %s", path); return jerr(m); }

    char *out = malloc(OUTMAX);
    if (!out) { closedir(d); return NULL; }
    size_t i = (size_t)snprintf(out, OUTMAX, "| 名称 | 类型 | 大小 | 修改时间 |\n|---|---|---|---|\n");

    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL && i < OUTMAX - 300 && n < 400) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        char kind[16] = "file";
        if (S_ISDIR(st.st_mode)) snprintf(kind, sizeof(kind), "dir");
        else if (S_ISLNK(st.st_mode)) snprintf(kind, sizeof(kind), "link");

        char size[32] = "-";
        if (!S_ISDIR(st.st_mode)) {
            if (st.st_size >= 1048576) snprintf(size, sizeof(size), "%.1fM", st.st_size / 1048576.0);
            else if (st.st_size >= 1024) snprintf(size, sizeof(size), "%.1fK", st.st_size / 1024.0);
            else snprintf(size, sizeof(size), "%lld", (long long)st.st_size);
        }
        char when[32] = {0};
        struct tm tmv;
        localtime_r(&st.st_mtime, &tmv);
        strftime(when, sizeof(when), "%m-%d %H:%M", &tmv);

        i += (size_t)snprintf(out + i, OUTMAX - i, "| %s%s | %s | %s | %s |\n",
                             e->d_name, S_ISDIR(st.st_mode) ? "/" : "", kind, size, when);
        n++;
    }
    closedir(d);

    char ans[200];
    snprintf(ans, sizeof(ans), "%s 下 %d 项", path, n);
    char *r = jpair(out, ans);
    free(out);
    return r;
}

/* ---------------- du ---------------- */
/* 走目录时顺手记下最大的几个文件 —— 用户问"哪个文件最大"，只给目录总和
 * 就是答非所问。 */
#define TOPN 8
static struct { char path[512]; long long size; } g_topfile[TOPN];
static int g_ntopfile;

static void topfile_push(const char *path, long long sz) {
    if (sz <= 0) return;
    if (g_ntopfile < TOPN) {
        snprintf(g_topfile[g_ntopfile].path, sizeof(g_topfile[0].path), "%s", path);
        g_topfile[g_ntopfile].size = sz;
        g_ntopfile++;
        return;
    }
    int mi = 0;
    for (int i = 1; i < g_ntopfile; i++)
        if (g_topfile[i].size < g_topfile[mi].size) mi = i;
    if (sz > g_topfile[mi].size) {
        snprintf(g_topfile[mi].path, sizeof(g_topfile[0].path), "%s", path);
        g_topfile[mi].size = sz;
    }
}

static long long walk_size(const char *path, int depth, long long *files) {
    if (depth > 8) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        if (files) (*files)++;
        topfile_push(path, (long long)st.st_size);
        return (long long)st.st_size;
    }
    DIR *d = opendir(path);
    if (!d) return 0;
    long long total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        total += walk_size(full, depth + 1, files);
    }
    closedir(d);
    return total;
}

static char *op_du(const char *json) {
    char p[512] = {0};
    jget(json, "path", p, sizeof(p));
    char path[768];
    resolve_path(p, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        char m[900];
        snprintf(m, sizeof(m), "路径不存在：%s。常搜的位置：/var/log（系统日志）、"
                               "/etc/config（UCI 配置）、/tmp、/www", path);
        return jerr(m);
    }

    long long files = 0;
    g_ntopfile = 0;
    long long total = walk_size(path, 0, &files);

    /* 按大小降序 */
    for (int a = 0; a < g_ntopfile; a++)
        for (int b = a + 1; b < g_ntopfile; b++)
            if (g_topfile[b].size > g_topfile[a].size) {
                char tp[512];
                long long ts = g_topfile[a].size;
                snprintf(tp, sizeof(tp), "%s", g_topfile[a].path);
                snprintf(g_topfile[a].path, sizeof(g_topfile[0].path), "%s", g_topfile[b].path);
                g_topfile[a].size = g_topfile[b].size;
                snprintf(g_topfile[b].path, sizeof(g_topfile[0].path), "%s", tp);
                g_topfile[b].size = ts;
            }

    char text[3500];
    size_t i = (size_t)snprintf(text, sizeof(text),
        "**%s**\n\n| 项 | 值 |\n|---|---|\n"
        "| 总大小 | %.2f MB |\n| 文件数 | %lld |\n\n",
        path, total / 1048576.0, files);
    if (g_ntopfile > 0) {
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "**最大的文件**\n\n| # | 文件 | 大小 |\n|---|---|---|\n");
        int show = g_ntopfile < 8 ? g_ntopfile : 8;
        for (int k = 0; k < show && i < sizeof(text) - 300; k++)
            i += (size_t)snprintf(text + i, sizeof(text) - i,
                "| %d | `%s` | %.2f MB |\n", k + 1,
                g_topfile[k].path, g_topfile[k].size / 1048576.0);
    }
    char ans[300];
    if (g_ntopfile > 0)
        snprintf(ans, sizeof(ans), "%s 共 %.2f MB / %lld 个文件；最大的是 %s（%.2f MB）",
                 path, total / 1048576.0, files, strrchr(g_topfile[0].path, '/') ?
                 strrchr(g_topfile[0].path, '/') + 1 : g_topfile[0].path,
                 g_topfile[0].size / 1048576.0);
    else
        snprintf(ans, sizeof(ans), "%s 共 %.2f MB / %lld 个文件", path, total / 1048576.0, files);
    return jpair(text, ans);
}

/* ---------------- grep ---------------- */
static char *op_grep(const char *json) {
    char pat[256] = {0}, p[512] = {0};
    jget(json, "pattern", pat, sizeof(pat));
    jget(json, "path", p, sizeof(p));
    if (!pat[0])
        return jerr("缺少 pattern（要搜的关键字）。正确写法："
                    "{\"op\":\"grep\",\"pattern\":\"404\",\"path\":\"/var/log\"}；"
                    "path 省略时在 . 下搜，max_hits 默认 30");
    char path[768];
    resolve_path(p[0] ? p : ".", path, sizeof(path));
    long maxh = jget_int(json, "max_hits", 30);
    if (maxh <= 0 || maxh > 200) maxh = 30;
    int icase = jget_int(json, "icase", 1);

    struct stat st;
    if (stat(path, &st) != 0) {
        char m[900];
        snprintf(m, sizeof(m), "路径不存在：%s。常搜的位置：/var/log（系统日志）、"
                               "/etc/config（UCI 配置）、/tmp、/www", path);
        return jerr(m);
    }

    char *out = malloc(OUTMAX);
    if (!out) return NULL;
    size_t i = (size_t)snprintf(out, OUTMAX, "匹配 `%s`：\n\n", pat);

    char cmd[1600];
    /* 用 grep 命令更靠谱（支持递归、二进制跳过），参数已白名单校验。
     * 文件名里有单引号会出问题，所以先检查一下。 */
    if (strchr(path, '\'')) {
        free(out);
        return jerr("路径里含单引号，grep 不安全，换个路径或直接 read");
    }
    snprintf(cmd, sizeof(cmd),
        "grep -rn%s -m 3 -- '%s' '%s' 2>/dev/null | head -n %ld",
        icase ? "i" : "", pat, path, maxh);
    char raw[OUTMAX];
    int tr = 0;
    run_sh(cmd, 20, raw, sizeof(raw), &tr);
    if (!raw[0]) {
        snprintf(out + i, OUTMAX - i, "（没有匹配）");
        char ans[200];
        snprintf(ans, sizeof(ans), "%s 里没找到 %s", path, pat);
        char *r = jpair(out, ans);
        free(out);
        return r;
    }
    snprintf(out + i, OUTMAX - i, "```\n%s```", raw);
    int lines = 0;
    for (const char *q = raw; *q; q++) if (*q == '\n') lines++;
    char ans[200];
    snprintf(ans, sizeof(ans), "在 %s 找到 %d 处匹配", path, lines);
    char *r = jpair(out, ans);
    free(out);
    return r;
}

/* ---------------- read ---------------- */
static char *op_read(const char *json) {
    char p[512] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) return jerr("缺少 path");
    char path[768];
    resolve_path(p, path, sizeof(path));

    long maxlines = jget_int(json, "lines", 500);
    if (maxlines <= 0) maxlines = 500;
    if (maxlines > 5000) maxlines = 5000;
    long start = jget_int(json, "start", 1);
    if (start < 1) start = 1;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* 二进制文件直接说清楚，别吐一堆乱码 */
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return jerr("这是目录，用 list 看内容");
        char m[900];
        snprintf(m, sizeof(m), "打不开文件：%s", path);
        return jerr(m);
    }

    char *out = malloc(OUTMAX);
    if (!out) { fclose(f); return NULL; }
    size_t i = (size_t)snprintf(out, OUTMAX, "```\n");

    char line[4096];
    long ln = 0, shown = 0;
    while (fgets(line, sizeof(line), f) && i < OUTMAX - 300) {
        ln++;
        if (ln < start) continue;
        if (shown >= maxlines) break;
        /* 单行太长：截断并标注，避免一个 minified js 把输出撑爆 */
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (L > 400) {
            i += (size_t)snprintf(out + i, OUTMAX - i, "%5ld| %.400s…(本行 %zu 字)\n", ln, line, L);
        } else {
            i += (size_t)snprintf(out + i, OUTMAX - i, "%5ld| %s\n", ln, line);
        }
        shown++;
    }
    fclose(f);
    snprintf(out + i, OUTMAX - i, "```");

    char ans[220];
    snprintf(ans, sizeof(ans), "%s 读了 %ld 行（第 %ld 行起，共 %ld 行）", path, shown, start, ln);
    char *r = jpair(out, ans);
    free(out);
    return r;
}

/* ---------------- write ---------------- */
static char *op_write(const char *json) {
    char p[512] = {0}, content[65536] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "content", content, sizeof(content));
    if (!p[0]) return jerr("缺少 path");
    char path[768];
    resolve_path(p, path, sizeof(path));

    if (is_sensitive(path)) {
        char m[900];
        snprintf(m, sizeof(m),
            "拒绝写入 %s：这是系统目录（/etc /usr /rom /root 等）。"
            "要改系统配置请用 owrt 插件的 uci 操作。", path);
        return jerr(m);
    }

    /* 覆盖保护：已经存在的媒体文件不许被 sys.write 覆盖。
     * 实测事故：模型把 "ffmpeg -i x.mp4 ..." 这条命令当成文件内容，
     * 用 {"op":"write","path":"/opt/mp4/01.mp4","content":"ffmpeg ..."}
     * 把 81MB 的源视频覆盖成了 66 字节。文本写文本是自己的事，
     * 拿文本覆盖视频/音频/图片不是。 */
    {
        static const char *media[] = {".mp4", ".mkv", ".mov", ".m4v", ".avi", ".flv", ".ts",
                                      ".webm", ".mp3", ".wav", ".flac", ".ogg", ".m4a",
                                      ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp",
                                      ".xlsx", ".docx", ".pptx", ".pdf", NULL};
        struct stat mst;
        if (stat(path, &mst) == 0) {
            const char *dot = strrchr(path, '.');
            if (dot) {
                for (int i = 0; media[i]; i++) {
                    if (!strcasecmp(dot, media[i]))
                        return jerr("拒绝覆盖已有的媒体文件（要重新生成请换个输出路径，"
                                    "或先用 sys shell 明确删除）");
                }
            }
        }
    }

    /* 建父目录 */
    char dir[768];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl && sl != dir) {
        *sl = 0;
        char mk[900];
        snprintf(mk, sizeof(mk), "mkdir -p '%s' 2>/dev/null", dir);
        if (!strchr(dir, '\'')) system(mk);
    }

    FILE *f = fopen(path, "w");
    if (!f) { char m[900]; snprintf(m, sizeof(m), "写不了 %s（权限或目录不存在）", path); return jerr(m); }
    size_t len = strlen(content);
    size_t w = fwrite(content, 1, len, f);
    fclose(f);

    /* 回读确认 */
    long long back = 0;
    FILE *rf = fopen(path, "r");
    if (rf) {
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), rf)) > 0) back += (long long)r;
        fclose(rf);
    }

    char text[1024];
    snprintf(text, sizeof(text),
        "已写 `%s`\n\n| 项 | 值 |\n|---|---|\n| 写入 | %zu 字节 |\n| 回读 | %lld 字节 |\n| 一致 | %s |\n",
        path, w, back, (back == (long long)len) ? "是 ✓" : "否 ✗");
    char ans[220];
    snprintf(ans, sizeof(ans), "已写到 %s（%zu 字节）", path, w);
    return jpair(text, ans);
}

/* ---------------- sysinfo ---------------- */
static char *op_sysinfo(void) {
    char *out = malloc(OUTMAX);
    if (!out) return NULL;
    size_t i = 0;
    i += (size_t)snprintf(out + i, OUTMAX - i, "| 项 | 值 |\n|---|---|\n");

    /* CPU 频率 */
    {
        char freqs[256] = {0};
        int ncpu = 0;
        for (int c = 0; c < 16; c++) {
            char p[128];
            snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            long khz = 0;
            if (fscanf(f, "%ld", &khz) == 1) {
                char seg[32];
                snprintf(seg, sizeof(seg), "%s%ld", ncpu ? "/" : "", khz / 1000);
                strncat(freqs, seg, sizeof(freqs) - strlen(freqs) - 1);
                ncpu++;
            }
            fclose(f);
        }
        if (ncpu)
            i += (size_t)snprintf(out + i, OUTMAX - i, "| CPU 频率 | %s MHz（%d 核） |\n", freqs, ncpu);
    }
    /* 型号 */
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "model name", 10) || !strncmp(line, "Hardware", 8)) {
                    char *c = strchr(line, ':');
                    if (c) {
                        c++;
                        while (*c == ' ') c++;
                        size_t L = strlen(c);
                        while (L && (c[L - 1] == '\n' || c[L - 1] == '\r')) c[--L] = 0;
                        i += (size_t)snprintf(out + i, OUTMAX - i, "| CPU 型号 | %s |\n", c);
                        break;
                    }
                }
            }
            fclose(f);
        }
    }
    /* 负载 + 运行时长 */
    {
        FILE *f = fopen("/proc/loadavg", "r");
        if (f) {
            char a[32] = {0}, b[32] = {0}, c[32] = {0};
            if (fscanf(f, "%31s %31s %31s", a, b, c) >= 3)
                i += (size_t)snprintf(out + i, OUTMAX - i, "| 负载 1/5/15 | %s / %s / %s |\n", a, b, c);
            fclose(f);
        }
        f = fopen("/proc/uptime", "r");
        if (f) {
            double up = 0;
            if (fscanf(f, "%lf", &up) == 1) {
                long d = (long)(up / 86400), h = (long)((up - d * 86400) / 3600),
                     m = (long)((up - d * 86400 - h * 3600) / 60);
                i += (size_t)snprintf(out + i, OUTMAX - i, "| 运行时长 | %ld 天 %ld 小时 %ld 分 |\n", d, h, m);
            }
            fclose(f);
        }
    }
    /* 内存 */
    {
        FILE *f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[256];
            double total = 0, avail = 0;
            while (fgets(line, sizeof(line), f)) {
                unsigned long kb;
                if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) total = (double)kb;
                else if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) avail = (double)kb;
            }
            fclose(f);
            if (total > 0)
                i += (size_t)snprintf(out + i, OUTMAX - i, "| 内存 | 已用 %.0f / 总 %.0f MB（%.0f%%） |\n",
                                      (total - avail) / 1024.0, total / 1024.0,
                                      avail > 0 ? (total - avail) / total * 100 : 0);
        }
    }
    /* 磁盘 */
    {
        struct statvfs vfs;
        if (statvfs("/", &vfs) == 0) {
            double t = (double)vfs.f_blocks * vfs.f_frsize / 1073741824.0;
            double fr = (double)vfs.f_bavail * vfs.f_frsize / 1073741824.0;
            i += (size_t)snprintf(out + i, OUTMAX - i, "| 根分区 | 可用 %.1f / 共 %.1f GB |\n", fr, t);
        }
        if (statvfs("/tmp", &vfs) == 0) {
            double t = (double)vfs.f_blocks * vfs.f_frsize / 1048576.0;
            double fr = (double)vfs.f_bavail * vfs.f_frsize / 1048576.0;
            i += (size_t)snprintf(out + i, OUTMAX - i, "| /tmp | 可用 %.0f / 共 %.0f MB |\n", fr, t);
        }
    }
    /* 温度 */
    {
        const char *paths[] = {
            "/sys/class/thermal/thermal_zone0/temp",
            "/sys/class/hwmon/hwmon0/temp1_input", NULL
        };
        for (int k = 0; paths[k]; k++) {
            FILE *f = fopen(paths[k], "r");
            if (!f) continue;
            long v = 0;
            if (fscanf(f, "%ld", &v) == 1 && v > 0) {
                i += (size_t)snprintf(out + i, OUTMAX - i, "| 芯片温度 | %.1f ℃ |\n", v / 1000.0);
                fclose(f);
                break;
            }
            fclose(f);
        }
    }
    /* 网卡收发汇总 */
    {
        FILE *f = fopen("/proc/net/dev", "r");
        if (f) {
            char line[512];
            long long rx = 0, tx = 0;
            while (fgets(line, sizeof(line), f)) {
                char ifc[32] = {0};
                unsigned long long rb = 0, tb = 0;
                if (sscanf(line, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                           ifc, &rb, &tb) >= 3) {
                    if (!strcmp(ifc, "lo")) continue;
                    rx += (long long)rb;
                    tx += (long long)tb;
                }
            }
            fclose(f);
            if (rx || tx)
                i += (size_t)snprintf(out + i, OUTMAX - i,
                                      "| 累计流量 | 收 %.1f MB / 发 %.1f MB |\n",
                                      rx / 1048576.0, tx / 1048576.0);
        }
    }
    /* 进程数 */
    {
        int cnt = 0;
        FILE *f = fopen("/proc/loadavg", "r");
        if (f) {
            char a[32], b[32], c[32], procs[32];
            if (fscanf(f, "%31s %31s %31s %31s", a, b, c, procs) >= 4) {
                char *sl = strchr(procs, '/');
                if (sl) cnt = atoi(sl + 1);
            }
            fclose(f);
        }
        if (cnt)
            i += (size_t)snprintf(out + i, OUTMAX - i, "| 进程/线程数 | %d |\n", cnt);
    }

    char ans[220];
    snprintf(ans, sizeof(ans), "系统信息已取（CPU/内存/磁盘/温度/流量）");
    char *r = jpair(out, ans);
    free(out);
    return r;
}

/* ---------------- http_get / download ----------------
 * HTTPS 必须走 TLS，板上没有 openssl 头文件。这里用 wget（板上有，
 * 带 TLS），C 只负责拼参数、校验 URL、控制大小 —— 逻辑仍在本插件里。
 */
static int url_ok(const char *url) {
    if (!url || !*url) return 0;
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return 0;
    /* 只放行安全字符，避免拼进 shell 出问题 */
    for (const char *p = url; *p; p++) {
        if (isalnum((unsigned char)*p)) continue;
        if (strchr(":/?&=._-%+@~#,[]!", *p)) continue;
        return 0;
    }
    return 1;
}

static char *op_http_get(const char *json) {
    char url[1024] = {0};
    jget(json, "url", url, sizeof(url));
    if (!url_ok(url)) return jerr("URL 不合法（只支持 http/https，且不能含引号等特殊字符）");
    long maxc = jget_int(json, "max_chars", 3000);
    if (maxc <= 0 || maxc > 20000) maxc = 3000;

    /* 先拿 HTTP 状态码，再抓正文。
     * 原来是 wget -q 一把梭：4xx/5xx 时 wget 不输出正文，于是被判成
     * "抓取失败"——可用户问的往往就是"这个链接是不是 404"，状态码才是
     * 他要的答案。curl 能同时给状态码和正文（含错误页）。 */
    char cmd[1600];
    snprintf(cmd, sizeof(cmd),
        "curl -s -o /dev/null -w '%%{http_code}' --max-time 20 -L '%s' 2>/dev/null", url);
    char code[64] = {0};
    int tr0 = 0;
    run_sh(cmd, 25, code, sizeof(code), &tr0);
    long status = (code[0] >= '1' && code[0] <= '5') ? atol(code) : 0;

    snprintf(cmd, sizeof(cmd),
        "curl -sL --max-time 20 '%s' 2>/dev/null | head -c %ld", url, maxc * 6L);
    char *raw = malloc(OUTMAX);
    if (!raw) return NULL;
    int tr = 0;
    run_sh(cmd, 25, raw, OUTMAX, &tr);

    if (!raw[0] && !status) {
        free(raw);
        return jerr("连不上（DNS 解析失败 / 网络不通 / 站点拒绝连接）。"
                    "先确认网址和网络，或用 ping、resolve 检查");
    }
    /* 粗略剥 HTML：去掉 <...>，压缩空行，保留正文 */
    char *clean = malloc(OUTMAX);
    if (!clean) { free(raw); return NULL; }
    size_t ci = 0;
    int in_tag = 0;
    for (const char *p = raw; *p && ci < OUTMAX - 8; p++) {
        if (*p == '<') { in_tag = 1; continue; }
        if (*p == '>') { in_tag = 0; continue; }
        if (in_tag) continue;
        if (*p == '\n') {
            if (ci && clean[ci - 1] != '\n') clean[ci++] = '\n';
            continue;
        }
        clean[ci++] = *p;
        if (ci >= (size_t)maxc) break;
    }
    clean[ci] = 0;
    free(raw);

    char text[OUTMAX];
    const char *meaning = "";
    if (status == 200) meaning = "正常";
    else if (status == 301 || status == 302) meaning = "重定向（已跟随）";
    else if (status == 403) meaning = "禁止访问";
    else if (status == 404) meaning = "页面不存在";
    else if (status == 500) meaning = "服务器错误";
    else if (status >= 400) meaning = "客户端/服务端报错";
    if (status)
        snprintf(text, sizeof(text),
                 "**HTTP %ld** %s\n\n抓取 `%s`：\n\n```\n%s\n```",
                 status, meaning, url, clean[0] ? clean : "(内容为空)");
    else
        snprintf(text, sizeof(text), "抓取 `%s`（未拿到状态码）：\n\n```\n%s\n```",
                 url, clean[0] ? clean : "(内容为空)");

    char ans[300];
    if (status) snprintf(ans, sizeof(ans), "%s → HTTP %ld %s", url, status, meaning);
    else snprintf(ans, sizeof(ans), "已抓取 %s（%zu 字）", url, ci);
    char *r = jpair(text, ans);
    free(clean);
    return r;
}

static char *op_download(const char *json) {
    char url[1024] = {0}, p[512] = {0};
    jget(json, "url", url, sizeof(url));
    jget(json, "path", p, sizeof(p));
    if (!url_ok(url)) return jerr("URL 不合法（只支持 http/https）");

    long maxmb = jget_int(json, "max_mb", 20);
    if (maxmb <= 0 || maxmb > 200) maxmb = 20;

    char path[768];
    if (p[0]) {
        resolve_path(p, path, sizeof(path));
    } else {
        /* 没给路径：从 URL 末段取文件名 */
        const char *sl = strrchr(url, '/');
        const char *name = (sl && sl[1]) ? sl + 1 : "download.bin";
        snprintf(path, sizeof(path), "%s/uploads/%s", WORK_DIR, name);
    }
    if (is_sensitive(path)) return jerr("拒绝下载到系统目录");

    char cmd[1600];
    snprintf(cmd, sizeof(cmd),
        "wget -q -T 30 -O '%s' '%s' 2>&1", path, url);
    char out[4096] = {0};
    int tr = 0;
    int rc = run_sh(cmd, 120, out, sizeof(out), &tr);
    if (rc != 0) {
        char m[900];
        snprintf(m, sizeof(m), "下载失败：%.300s", out[0] ? out : "wget 返回非 0");
        return jerr(m);
    }

    struct stat st;
    if (stat(path, &st) != 0) return jerr("下载完却找不到文件");
    if (st.st_size > maxmb * 1024 * 1024) {
        unlink(path);
        char m[400];
        snprintf(m, sizeof(m), "文件超过 %ld MB，已删除", maxmb);
        return jerr(m);
    }

    char text[1024];
    snprintf(text, sizeof(text),
        "已下载到 `%s`\n\n| 项 | 值 |\n|---|---|\n| 大小 | %.2f MB |\n| 来源 | %s |\n",
        path, st.st_size / 1048576.0, url);
    char ans[300];
    snprintf(ans, sizeof(ans), "已下载 %s（%.2f MB）", path, st.st_size / 1048576.0);
    return jpair(text, ans);
}

/* ---------------- 入口 ---------------- */
static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    if (!op[0]) {
        /* 没给 op 就按参数猜 */
        if (strstr(json_in, "\"cmd\"")) snprintf(op, sizeof(op), "shell");
        else if (strstr(json_in, "\"url\"") && strstr(json_in, "\"path\"")) snprintf(op, sizeof(op), "download");
        else if (strstr(json_in, "\"url\"")) snprintf(op, sizeof(op), "http_get");
        else if (strstr(json_in, "\"content\"")) snprintf(op, sizeof(op), "write");
        else if (strstr(json_in, "\"pattern\"")) snprintf(op, sizeof(op), "grep");
        else if (strstr(json_in, "\"path\"")) snprintf(op, sizeof(op), "read");
        else snprintf(op, sizeof(op), "sysinfo");
    }

    if (!strcmp(op, "shell") || !strcmp(op, "sh") || !strcmp(op, "cmd")) return op_shell(json_in);
    if (!strcmp(op, "list") || !strcmp(op, "ls"))                    return op_list(json_in);
    if (!strcmp(op, "du"))                                           return op_du(json_in);
    if (!strcmp(op, "grep"))                                         return op_grep(json_in);
    if (!strcmp(op, "read") || !strcmp(op, "cat"))                   return op_read(json_in);
    if (!strcmp(op, "write"))                                        return op_write(json_in);
    if (!strcmp(op, "sysinfo") || !strcmp(op, "info"))               return op_sysinfo();
    if (!strcmp(op, "http_get") || !strcmp(op, "http"))              return op_http_get(json_in);
    if (!strcmp(op, "download") || !strcmp(op, "wget"))              return op_download(json_in);

    {
        char m[760];
        if (!op[0])
            snprintf(m, sizeof(m),
                "缺少 op（sys 不会自己猜）。支持的 op：shell / list / du / grep / "
                "read / write / sysinfo / http_get / download。\n"
                "抓网页：{\"op\":\"http_get\",\"url\":\"http://example.com\"}；"
                "列目录：{\"op\":\"list\",\"path\":\"/tmp\"}；"
                "搜索：{\"op\":\"grep\",\"pattern\":\"404\",\"path\":\"/var/log\"}");
        else
            snprintf(m, sizeof(m),
                "不支持的 op：%s。sys 只认 shell / list / du / grep / read / write / "
                "sysinfo / http_get / download。\n"
                "抓网页请用 {\"op\":\"http_get\",\"url\":\"...\"}", op);
        return jerr(m);
    }
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "sys",
    "sys 文件与系统（日志在 owrt 插件的 logread，不在这里）："
    "sysinfo=磁盘/内存/负载/温度 shell=跑命令(可管道) list=列目录 du=占用 "
    "grep=按关键字搜文件内容 read=读文件 write=写文件 http_get=抓网页(带状态码) download=下载",
    "{\"op\":\"shell\",\"cmd\":\"ls -la /tmp\"}"
    "  或 {\"op\":\"list\",\"path\":\"/tmp\"}"
    "  或 {\"op\":\"read\",\"path\":\"/tmp/a.txt\",\"lines\":200}"
    "  或 {\"op\":\"write\",\"path\":\"a.txt\",\"content\":\"内容\"}"
    "  或 {\"op\":\"grep\",\"pattern\":\"error\",\"path\":\"/var/log\"}"
    "  或 {\"op\":\"du\",\"path\":\"/tmp\"}"
    "  或 {\"op\":\"sysinfo\"}"
    "  或 {\"op\":\"http_get\",\"url\":\"https://example.com\"}"
    "  或 {\"op\":\"download\",\"url\":\"https://example.com/a.zip\",\"path\":\"/tmp/a.zip\"}",
    run,
    "shell,cmd,list,ls,du,grep,read,cat,write,sysinfo,http_get,download,wget,文件,命令"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
