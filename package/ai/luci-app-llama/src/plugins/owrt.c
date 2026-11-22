/* owrt.c — OpenWrt 配置插件
 *
 * 覆盖 OpenWrt 日常配置与无线查询：uci / iwinfo / iw / init.d / logread。
 *
 * 安全三条原则：
 *   1. 全部用 fork+execv，参数作为 argv 直接传给命令，不经过 shell —— 没有注入面。
 *   2. uci 的"改"和"生效"分开：uci_set/add_list/delete 只写 /etc/config 里的文件，
 *      不 commit 就不生效；要生效必须再调 op=uci_commit。中间随时能 op=uci_revert。
 *   3. 碰到会断网/锁死自己的键（network.lan.*、dropbear、firewall、network.wan）
 *      照样执行，但返回里带警告，让调用方知道风险。
 *
 * 支持的 op：
 *   uci_show     [package]           整个包（表格）
 *   uci_get      key                 单个值
 *   uci_changes                      还没提交的改动
 *   uci_set      key value           改一个值（不生效）
 *   uci_add_list key value           往列表加一项
 *   uci_del_list key value           从列表删一项
 *   uci_delete   key                 删节点
 *   uci_commit   package [reload]    提交（reload=network/firewall/dropbear… 可选）
 *   uci_revert   [package]           撤销未提交改动
 *   iwinfo       [iface] [sub]       无线信息（sub=info/assoclist/scan）
 *   iw           sub [iface]         iw dev / iw list / station dump
 *   service      name action         status / reload / restart
 *   logread      [lines] [key]       系统日志；key=只看含该关键字的行
 *   passwd       user pass confirm:yes 改密码（必须用户确认；自动避开 bcrypt）
 *   reboot       confirm:yes         重启（必须显式确认）
 *   opkg_list    [pattern]           已装软件包清单
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include "plugin.h"

#define OUTMAX 24576

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
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atoi(p + strlen(pat)) : dflt;
}

/* ---------------- 参数白名单 ---------------- */
/* uci 的键/节/包名、以及值：只允许可打印且不含 shell 元字符。
 * 反正走 execv 不过 shell，这里再拦一道是防"参数被当成选项"（比如值以 - 开头）。 */
static int safe_token(const char *s, size_t maxlen, int allow_space) {
    if (!s || !*s || strlen(s) > maxlen) return 0;
    if (s[0] == '-') return 0;                       /* 别让它变成选项 */
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0;         /* 控制字符 */
        if (c >= 0x80) continue;                     /* 中文等 UTF-8 放行 */
        if (isalnum(c) || strchr("._-:/@,+=%()[]", c)) continue;
        if (allow_space && c == ' ') continue;
        return 0;
    }
    return 1;
}

/* 接口名：wlan0 / phy0-ap0 / br-lan */
static int safe_iface(const char *s) {
    if (!s || !*s || strlen(s) > 24) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("._-@", *p)) return 0;
    return 1;
}

/* ---------------- fork+execv 执行，输出 + 退出码 ---------------- */
static int run_argv(char *const argv[], char *out, size_t cap) {
    int fd[2];
    if (pipe(fd) != 0) { snprintf(out, cap, "pipe 失败"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); snprintf(out, cap, "fork 失败"); return -1; }

    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[1]);
        int nul = open("/dev/null", 0);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); if (nul > 2) close(nul); }
        execv(argv[0], argv);
        _exit(127);
    }

    close(fd[1]);
    size_t n = 0;
    ssize_t r;
    while ((r = read(fd[0], out + n, cap - 1 - n)) > 0) {
        n += (size_t)r;
        if (n + 1 >= cap) break;
    }
    out[n] = 0;
    close(fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* ---------------- 输出封装 ---------------- */
static char *json_pair(const char *text, const char *answer, const char *warn) {
    size_t cap = strlen(text) * 2 + strlen(answer) * 2 + (warn ? strlen(warn) * 2 : 0) + 512;
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
    i += (size_t)snprintf(o + i, cap - i, "\"");
    if (warn && warn[0]) {
        i += (size_t)snprintf(o + i, cap - i, ",\"warning\":\"");
        for (const char *p = warn; *p && i + 12 < cap; p++) {
            if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
            else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
            else if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
            else o[i++] = *p;
        }
        i += (size_t)snprintf(o + i, cap - i, "\"");
    }
    snprintf(o + i, cap - i, "}");
    return o;
}

static char *err_json(const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char *o = malloc(strlen(msg) * 2 + 64);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, strlen(msg) * 2 + 64, "{\"error\":\"");
    for (const char *p = msg; *p; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    snprintf(o + i, strlen(msg) * 2 + 64 - i, "\"}");
    return o;
}

/* 危险键：不阻止，但返回警告 */
static const char *risky_key(const char *key) {
    static const char *risky[] = {
        "network.lan.", "network.wan.", "network.globals", "dropbear.",
        "firewall.", "dhcp.lan.dhcpv4", "system.", NULL
    };
    for (int i = 0; risky[i]; i++)
        if (!strncmp(key, risky[i], strlen(risky[i]))) return risky[i];
    return NULL;
}

/* ---------------- op: uci show ---------------- */
static char *op_uci_show(const char *pkg) {
    /* 不带 package 时【不要】整库 dump：这台板上 uci show 有 600+ 项，
     * 一次全吐出去会把上下文撑爆、回复直接被截断（实测就发生了）。
     * 改成先返回包名清单，让调用方挑一个再查。 */
    if (!pkg || !pkg[0]) {
        /* 不要跑 `uci show` 全量：这台板 1181 行 ≈ 60KB，超过 OUTMAX(24KB)
         * 会被截断并判成失败。直接列 /etc/config 下的文件名就是包名，
         * 又快又准。 */
        char names[96][40];
        int n = 0;
        DIR *d = opendir("/etc/config");
        if (!d) return err_json("打不开 /etc/config");
        struct dirent *de;
        while ((de = readdir(d)) != NULL && n < 96) {
            const char *nm = de->d_name;
            if (nm[0] == '.') continue;
            if (strchr(nm, '.')) continue;          /* 跳过 .bak / .old / .rpcd */
            if (strchr(nm, '~')) continue;
            size_t L = strlen(nm);
            if (L == 0 || L >= sizeof(names[0])) continue;
            /* 确认真的是普通文件（有的目录也在这儿） */
            char full[128];
            snprintf(full, sizeof(full), "/etc/config/%s", nm);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            snprintf(names[n], sizeof(names[n]), "%s", nm);
            n++;
        }
        closedir(d);
        /* 简单排序，输出稳定 */
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++)
                if (strcmp(names[b], names[a]) < 0) {
                    char t[40];
                    snprintf(t, sizeof(t), "%s", names[a]);
                    snprintf(names[a], sizeof(names[a]), "%s", names[b]);
                    snprintf(names[b], sizeof(names[b]), "%s", t);
                }
        char text[OUTMAX];
        size_t i = (size_t)snprintf(text, sizeof(text),
            "共 **%d 个配置包**。要查哪个就带 package，例如 "
            "`{\"op\":\"uci_show\",\"package\":\"%s\"}`：\n\n",
            n, n > 0 ? names[0] : "network");
        for (int k = 0; k < n && i + 60 < sizeof(text); k++) {
            i += (size_t)snprintf(text + i, sizeof(text) - i, "`%s`%s",
                                  names[k], (k + 1) % 6 ? "　" : "\n");
        }
        char ans[128];
        snprintf(ans, sizeof(ans), "共 %d 个配置包（未指定 package）", n);
        return json_pair(text, ans, NULL);
    }

    char out[OUTMAX];
    char *argv[4];
    argv[0] = (char *)"/sbin/uci";
    argv[1] = (char *)"show";
    argv[2] = (pkg && pkg[0]) ? (char *)pkg : NULL;
    argv[3] = NULL;
    if (pkg && pkg[0] && !safe_token(pkg, 64, 0))
        return err_json("包名不合法：%s", pkg);

    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0 || !out[0])
        return err_json("uci show %s 没有输出（包不存在？）", pkg ? pkg : "");

    /* 输出是 key='value' 一行一条，做成表格更好读 */
    char *buf = malloc(OUTMAX);
    if (!buf) return NULL;
    size_t i = (size_t)snprintf(buf, OUTMAX, "| 键 | 值 |\n|---|---|\n");
    int cnt = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line && i < OUTMAX - 200;
         line = strtok_r(NULL, "\n", &save)) {
        if (cnt >= 150) break;          /* 单包也可能很大，封个顶 */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        size_t vl = strlen(val);
        if (vl >= 2 && (*val == '\'' || *val == '"') && val[vl - 1] == *val) {
            val[vl - 1] = 0;
            val++;
        }
        i += (size_t)snprintf(buf + i, OUTMAX - i, "| `%s` | %s |\n", line, val);
        cnt++;
    }
    char ans[128];
    snprintf(ans, sizeof(ans), "%s 共 %d 项配置",
             (pkg && pkg[0]) ? pkg : "全部", cnt);
    char *r = json_pair(buf, ans, NULL);
    free(buf);
    return r;
}

/* ---------------- op: uci get ---------------- */
static char *op_uci_get(const char *key) {
    if (!safe_token(key, 128, 0)) return err_json("键名不合法：%s", key);
    char out[8192];
    char *argv[] = { (char *)"/sbin/uci", (char *)"get", (char *)key, NULL };
    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0) return err_json("%s 不存在", key);

    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    char text[9000];
    snprintf(text, sizeof(text), "`%s` = **%s**", key, out);
    char ans[256];
    snprintf(ans, sizeof(ans), "%s = %s", key, out);
    return json_pair(text, ans, NULL);
}

/* ---------------- op: uci changes ---------------- */
static char *op_uci_changes(void) {
    char out[OUTMAX];
    char *argv[] = { (char *)"/sbin/uci", (char *)"changes", NULL };
    int rc = run_argv(argv, out, sizeof(out));
    (void)rc;
    if (!out[0]) {
        return json_pair("没有未提交的改动。", "没有未提交的改动", NULL);
    }
    char text[OUTMAX];
    snprintf(text, sizeof(text), "未提交的改动：\n\n```\n%s\n```\n\n"
             "要生效：`op=uci_commit`；要撤销：`op=uci_revert`", out);
    char ans[128];
    snprintf(ans, sizeof(ans), "有未提交的 uci 改动");
    return json_pair(text, ans, NULL);
}

/* ---------------- op: uci set / add_list / del_list / delete ---------------- */
static char *op_uci_set(const char *key, const char *value, const char *mode) {
    if (!safe_token(key, 128, 0)) return err_json("键名不合法：%s", key);
    if (value && !safe_token(value, 512, 1)) return err_json("值含不允许的字符：%s", value);

    char pair[700];
    char out[4096];
    char *argv[6];
    int rc;
    const char *verb_cn;

    if (!strcmp(mode, "set")) {
        if (!value || !*value) return err_json("set 需要 value");
        snprintf(pair, sizeof(pair), "%s=%s", key, value);
        argv[0] = (char *)"/sbin/uci"; argv[1] = (char *)"set"; argv[2] = pair; argv[3] = NULL;
        verb_cn = "设置";
    } else if (!strcmp(mode, "add_list")) {
        if (!value || !*value) return err_json("add_list 需要 value");
        snprintf(pair, sizeof(pair), "%s=%s", key, value);
        argv[0] = (char *)"/sbin/uci"; argv[1] = (char *)"add_list"; argv[2] = pair; argv[3] = NULL;
        verb_cn = "追加到列表";
    } else if (!strcmp(mode, "del_list")) {
        if (!value || !*value) return err_json("del_list 需要 value");
        snprintf(pair, sizeof(pair), "%s=%s", key, value);
        argv[0] = (char *)"/sbin/uci"; argv[1] = (char *)"del_list"; argv[2] = pair; argv[3] = NULL;
        verb_cn = "从列表移除";
    } else {
        argv[0] = (char *)"/sbin/uci"; argv[1] = (char *)"delete"; argv[2] = (char *)key; argv[3] = NULL;
        verb_cn = "删除";
    }

    rc = run_argv(argv, out, sizeof(out));
    if (rc != 0)
        return err_json("%s 失败：%s", verb_cn, out[0] ? out : "uci 返回非 0");

    /* 读回当前值，确认真的写进去了 */
    char back[1024] = {0};
    {
        char *gargv[] = { (char *)"/sbin/uci", (char *)"get", (char *)key, NULL };
        run_argv(gargv, back, sizeof(back));
        size_t bl = strlen(back);
        while (bl > 0 && (back[bl - 1] == '\n' || back[bl - 1] == '\r')) back[--bl] = 0;
    }

    /* 包名 = 键的第一个点之前那段：network.lan.ipaddr → network */
    char pkgname[80] = {0};
    {
        const char *dot = strchr(key, '.');
        size_t L = dot ? (size_t)(dot - key) : strlen(key);
        if (L >= sizeof(pkgname)) L = sizeof(pkgname) - 1;
        memcpy(pkgname, key, L);
        pkgname[L] = 0;
    }

    char text[2048];
    snprintf(text, sizeof(text),
        "%s 完成：`%s`\n\n当前值：`%s`\n\n"
        "**还没生效** —— 改动只写进了 /etc/config。\n"
        "要生效：`{\"op\":\"uci_commit\",\"package\":\"%s\"}`\n"
        "要撤销：`{\"op\":\"uci_revert\",\"package\":\"%s\"}`",
        verb_cn, key, back[0] ? back : "(空)", pkgname, pkgname);

    char ans[256];
    snprintf(ans, sizeof(ans), "%s %s 成功（未提交）", verb_cn, key);

    const char *rk = risky_key(key);
    if (rk)
        return json_pair(text, ans,
            "这个键会影响网络/远程登录，commit 后可能立刻断开现有连接。"
            "建议先确认有串口或本地终端可用。");
    return json_pair(text, ans, NULL);
}

/* ---------------- op: uci commit ---------------- */
static char *op_uci_commit(const char *pkg, const char *reload) {
    if (pkg && pkg[0] && !safe_token(pkg, 64, 0)) return err_json("包名不合法：%s", pkg);

    char out[4096];
    char *argv[4];
    argv[0] = (char *)"/sbin/uci";
    argv[1] = (char *)"commit";
    argv[2] = (pkg && pkg[0]) ? (char *)pkg : NULL;
    argv[3] = NULL;
    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0) return err_json("uci commit 失败：%s", out[0] ? out : "非 0");

    char text[4096];
    size_t i = (size_t)snprintf(text, sizeof(text),
        "已提交到 `/etc/config/%s`。", (pkg && pkg[0]) ? pkg : "(全部)");

    char reloaded[128] = {0};
    if (reload && reload[0]) {
        if (!safe_token(reload, 48, 0)) return err_json("reload 名不合法：%s", reload);
        char script[128];
        snprintf(script, sizeof(script), "/etc/init.d/%s", reload);
        char *rargv[] = { script, (char *)"reload", NULL };
        int rrc = run_argv(rargv, out, sizeof(out));
        if (rrc != 0) {
            char *rargv2[] = { script, (char *)"restart", NULL };
            rrc = run_argv(rargv2, out, sizeof(out));
        }
        snprintf(reloaded, sizeof(reloaded), "%s(%s)", reload, rrc == 0 ? "ok" : "失败");
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "\n已重载服务：%s%s", reload, rrc == 0 ? "" : "（reload/restart 返回非 0，看看下面的输出）");
        if (rrc != 0 && out[0] && i < sizeof(text) - 400)
            i += (size_t)snprintf(text + i, sizeof(text) - i, "\n\n```\n%.300s\n```", out);
    } else {
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "\n**注意**：commit 只是写盘，很多改动要 reload 服务才生效。"
            "\n可以用 `{\"op\":\"uci_commit\",\"package\":\"%s\",\"reload\":\"%s\"}`",
            (pkg && pkg[0]) ? pkg : "network",
            (pkg && !strcmp(pkg, "wireless")) ? "network" : ((pkg && pkg[0]) ? pkg : "network"));
    }

    char ans[256];
    snprintf(ans, sizeof(ans), "已提交%s%s", (pkg && pkg[0]) ? pkg : "全部配置",
             reloaded[0] ? "，服务已重载" : "");
    const char *rk = pkg ? risky_key(pkg) : NULL;
    return json_pair(text, ans,
        (pkg && (!strcmp(pkg, "network") || !strcmp(pkg, "firewall") || !strcmp(pkg, "dhcp")))
            ? "网络类配置提交后可能断开当前连接，如果通过 SSH 操作请准备好重连或串口。"
            : (rk ? "这个包会影响网络/登录，注意连接可能中断。" : NULL));
}

/* ---------------- op: uci revert ---------------- */
static char *op_uci_revert(const char *pkg) {
    if (pkg && pkg[0] && !safe_token(pkg, 64, 0)) return err_json("包名不合法：%s", pkg);
    char out[4096];
    char *argv[4];
    argv[0] = (char *)"/sbin/uci";
    argv[1] = (char *)"revert";
    argv[2] = (pkg && pkg[0]) ? (char *)pkg : NULL;
    argv[3] = NULL;
    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0) return err_json("uci revert 失败：%s", out[0] ? out : "非 0");
    char text[512];
    snprintf(text, sizeof(text), "已撤销 %s 的未提交改动，回到 /etc/config 里的原值。",
             (pkg && pkg[0]) ? pkg : "全部");
    char ans[128];
    snprintf(ans, sizeof(ans), "已撤销未提交改动");
    return json_pair(text, ans, NULL);
}

/* ---------------- op: iwinfo ---------------- */
static char *op_iwinfo(const char *iface, const char *sub) {
    char out[OUTMAX];
    char *argv[5];
    argv[0] = (char *)"/usr/bin/iwinfo";

    if (!iface || !iface[0]) {
        /* 不带参数：列出所有无线设备 */
        argv[1] = NULL;
        run_argv(argv, out, sizeof(out));
        if (!out[0]) return json_pair("这台设备没有无线接口（iwinfo 无输出）。", "没有无线接口", NULL);
        char text[OUTMAX];
        snprintf(text, sizeof(text), "```\n%s```", out);
        return json_pair(text, "无线设备列表", NULL);
    }

    if (!safe_iface(iface)) return err_json("接口名不合法：%s", iface);
    const char *s = (sub && sub[0]) ? sub : "info";
    if (strcmp(s, "info") && strcmp(s, "assoclist") && strcmp(s, "scan")
        && strcmp(s, "txpower") && strcmp(s, "country") && strcmp(s, "freqlist"))
        return err_json("不支持的 iwinfo 子命令：%s（可用 info/assoclist/scan/txpower/country）", s);

    argv[1] = (char *)iface;
    argv[2] = (char *)s;
    argv[3] = NULL;
    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0 || !out[0]) return err_json("iwinfo %s %s 失败", iface, s);

    char text[OUTMAX];
    if (!strcmp(s, "scan")) {
        snprintf(text, sizeof(text), "扫描结果（`%s`）：\n\n```\n%.12000s```", iface, out);
    } else {
        snprintf(text, sizeof(text), "```\n%.12000s```", out);
    }
    char ans[128];
    snprintf(ans, sizeof(ans), "iwinfo %s %s", iface, s);
    return json_pair(text, ans, NULL);
}

/* ---------------- op: iw ---------------- */
static char *op_iw(const char *sub, const char *iface) {
    char out[OUTMAX];
    char *argv[6];
    argv[0] = (char *)"/usr/sbin/iw";

    if (!sub || !*sub || !strcmp(sub, "dev")) {
        argv[1] = (char *)"dev";
        argv[2] = NULL;
    } else if (!strcmp(sub, "list")) {
        argv[1] = (char *)"list";
        argv[2] = NULL;
    } else if (!strcmp(sub, "station")) {
        if (!safe_iface(iface)) return err_json("station 需要合法 iface");
        argv[1] = (char *)"dev"; argv[2] = (char *)iface;
        argv[3] = (char *)"station"; argv[4] = (char *)"dump"; argv[5] = NULL;
    } else if (!strcmp(sub, "link")) {
        if (!safe_iface(iface)) return err_json("link 需要合法 iface");
        argv[1] = (char *)"dev"; argv[2] = (char *)iface;
        argv[3] = (char *)"link"; argv[4] = NULL;
    } else {
        return err_json("不支持的 iw 子命令：%s（可用 dev/list/station/link）", sub);
    }

    int rc = run_argv(argv, out, sizeof(out));
    /* station dump 在"没有客户端关联"时输出为空、退出码 0 —— 那是正常状态，
     * 不是失败。这里分开判断，别把"没人连着"报成错误。 */
    if (!out[0] && rc == 0) {
        if (!strcmp(sub, "station"))
            return json_pair("`station dump` 为空：当前没有客户端关联到该接口。",
                             "没有客户端连接", NULL);
        return json_pair("命令没有输出（接口或无线不存在？）", "无输出", NULL);
    }
    if (rc != 0) return err_json("iw %s 失败（设备可能没有无线）", sub ? sub : "dev");
    char text[OUTMAX];
    snprintf(text, sizeof(text), "```\n%.12000s```", out);
    char ans[128];
    snprintf(ans, sizeof(ans), "iw %s", sub && *sub ? sub : "dev");
    return json_pair(text, ans, NULL);
}

/* ---------------- op: service ---------------- */
/* ---------------- op: passwd ----------------
 * 改用户密码。走 busybox 的 passwd，密码通过 stdin 喂进去。
 *
 * 为什么不用 chpasswd：OpenWrt 上它写出来的哈希 dropbear 和 rpcd 都不认
 * （实测 shadow 里确实有 60 字节 SHA512，但 SSH 和 LuCI 全都登不进去），
 * 是个能把人锁在门外的坑。
 *
 * 密码不进 argv（ps 里能看到），也不写进任何日志/返回值 —— 只报长度。
 */
/* 读 /etc/shadow 里某个用户的哈希前缀（$1$ / $2a$ / $6$ …），失败返回空 */
static void shadow_prefix(const char *user, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen("/etc/shadow", "r");
    if (!f) return;
    char line[512];
    size_t ul = strlen(user);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, user, ul) != 0 || line[ul] != ':') continue;
        char *p = line + ul + 1;
        size_t i = 0;
        while (*p && *p != ':' && i + 1 < cap) out[i++] = *p++;
        out[i] = 0;
        break;
    }
    fclose(f);
}

/* 纯 python 的 md5-crypt 兜底：这块板 musl 的 crypt() 返回 ENOSYS，
 * 而 busybox passwd 会生成 bcrypt($2a$) —— dropbear 完全不认。
 * 把脚本写到 /tmp 再跑，密码走管道，不进 argv。 */
static int passwd_md5crypt(const char *user, const char *pass) {
    static const char *PY =
        "import hashlib,sys,os\n"
        "S='./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'\n"
        "def f(p,s):\n"
        "    pw=p.encode();sl=s.encode()\n"
        "    m=hashlib.md5();m.update(pw+b'$1$'+sl)\n"
        "    x=hashlib.md5(pw+sl+pw).digest()\n"
        "    for i in range(len(pw)):m.update(x[i%16:i%16+1])\n"
        "    i=len(pw)\n"
        "    while i:\n"
        "        m.update(b'\\\\0' if i&1 else pw[:1]);i>>=1\n"
        "    d=m.digest()\n"
        "    for i in range(1000):\n"
        "        m2=hashlib.md5()\n"
        "        m2.update(pw if i&1 else d)\n"
        "        if i%3:m2.update(sl)\n"
        "        if i%7:m2.update(pw)\n"
        "        m2.update(d if i&1 else pw)\n"
        "        d=m2.digest()\n"
        "    def t(v,n):\n"
        "        o=''\n"
        "        for _ in range(n):o+=S[v&0x3f];v>>=6\n"
        "        return o\n"
        "    o=''\n"
        "    for a,b,c in ((0,6,12),(1,7,13),(2,8,14),(3,9,15),(4,10,5)):\n"
        "        o+=t((d[a]<<16)|(d[b]<<8)|d[c],4)\n"
        "    return '$1$'+s+'$'+o+t(d[11],2)\n"
        "u=sys.argv[1];p=sys.stdin.readline().rstrip('\\\\n')\n"
        "h=f(p,'ab12cd34')\n"
        "L=open('/etc/shadow').read().splitlines()\n"
        "O=[];done=False\n"
        "for x in L:\n"
        "    if x.startswith(u+':'):\n"
        "        q=x.split(':');q[1]=h;O.append(':'.join(q));done=True\n"
        "    else:O.append(x)\n"
        "if not done:sys.stderr.write('no such user');sys.exit(3)\n"
        "t=open('/etc/shadow.tmp','w');t.write('\\\\n'.join(O)+'\\\\n');t.close()\n"
        "os.chmod('/etc/shadow.tmp',0o600)\n"
        "os.rename('/etc/shadow.tmp','/etc/shadow')\n"
        "print(h)\n";

    const char *sp = "/tmp/.owrt_pw.py";
    FILE *sf = fopen(sp, "w");
    if (!sf) return -1;
    fputs(PY, sf);
    fclose(sf);

    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);
        alarm(20);
        execl("/usr/bin/python3", "python3", sp, user, (char *)NULL);
        execl("/bin/python3", "python3", sp, user, (char *)NULL);
        _exit(127);
    }
    close(fd[0]);
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "%s\n", pass);
    if (n > 0) { ssize_t w = write(fd[1], buf, (size_t)n); (void)w; }
    close(fd[1]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---------------- op: passwd ----------------
 * 改某个用户的密码。
 *
 * 两道闸：
 *   1. 必须 confirm:"yes" —— 但这个值由【页面弹窗用户确认后】才补上，
 *      模型自己填不算数（前端检测不到确认就不会补）。
 *   2. 生成哈希后要检查格式：这块板的 dropbear 只认 $1$/$5$/$6$，
 *      busybox passwd 却会生成 bcrypt($2a$)，设完照样登不进去。
 *      发现不是支持的格式就自动用纯 python md5-crypt 重写。
 *
 * 密码不写 argv、不进日志（只报长度）。
 */
static char *op_passwd(const char *user, const char *pass, const char *confirm) {
    const char *u = (user && user[0]) ? user : "root";
    if (!confirm || strcmp(confirm, "yes"))
        return err_json("改密码是高危操作：必须由用户在页面上确认后传 confirm:\"yes\"。"
                        "没人确认过就拒绝执行");
    if (!safe_token(u, 32, 0)) return err_json("用户名不合法：%s", u);
    if (!pass || !pass[0]) return err_json("需要 pass（新密码）");
    if (strlen(pass) > 128) return err_json("密码太长（上限 128 字节）");

    /* 第一步：走系统自带的 passwd */
    int code = -1;
    int fd[2];
    if (pipe(fd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fd[0], STDIN_FILENO);
            close(fd[0]);
            close(fd[1]);
            alarm(15);
            execl("/bin/passwd", "passwd", u, (char *)NULL);
            execl("/usr/bin/passwd", "passwd", u, (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            close(fd[0]);
            char buf[320];
            int n = snprintf(buf, sizeof(buf), "%s\n%s\n", pass, pass);
            if (n > 0 && n < (int)sizeof(buf)) {
                size_t off = 0;
                while (off < (size_t)n) {
                    ssize_t w = write(fd[1], buf + off, (size_t)n - off);
                    if (w <= 0) break;
                    off += (size_t)w;
                }
            }
            close(fd[1]);
            int st = 0;
            waitpid(pid, &st, 0);
            code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        } else {
            close(fd[0]);
            close(fd[1]);
        }
    }

    /* 第二步：查生成的哈希格式，dropbear 不认的自动换成 md5-crypt */
    char pref[16] = {0};
    shadow_prefix(u, pref, sizeof(pref));
    int fallback = 0;
    if (!pref[0] || (!strncmp(pref, "$2a$", 4) || !strncmp(pref, "$2b$", 4)
                     || !strncmp(pref, "$2y$", 4) || pref[0] != '$')) {
        /* bcrypt 或不认识 → 换 md5-crypt */
        int rc2 = passwd_md5crypt(u, pass);
        if (rc2 != 0)
            return err_json("passwd 返回 %d，md5-crypt 兜底也失败（退出码 %d）", code, rc2);
        fallback = 1;
        shadow_prefix(u, pref, sizeof(pref));
    }

    if (code != 0 && !fallback)
        return err_json("passwd 失败（退出码 %d）", code);
    if (!pref[0] || !strncmp(pref, "$2", 2))
        return err_json("密码写进去了但哈希格式不受支持（%s），dropbear 会登不进去", pref);

    char text[600], ans[200];
    snprintf(text, sizeof(text),
             "已设置用户 **%s** 的密码（%d 字节）。\n\n"
             "哈希格式：`%s`（dropbear 认这个）%s\n\n"
             "SSH 和 LuCI 立刻生效。旧会话不受影响，新连接用新密码。",
             u, (int)strlen(pass), pref,
             fallback ? "\n\n> 系统 passwd 生成的是 bcrypt，dropbear 不认，已自动改用 md5-crypt。" : "");
    snprintf(ans, sizeof(ans), "%s 的密码已更新（%s）", u, pref);
    return json_pair(text, ans, NULL);
}

/* ---------------- op: reboot ----------------
 * 重启。必须显式 confirm:"yes" —— 这个动作会断开所有连接，
 * 不设这道门槛的话模型很容易顺手调一下。
 */
static char *op_reboot(const char *confirm) {
    if (!confirm || strcmp(confirm, "yes"))
        return err_json("重启必须显式传 confirm:\"yes\"（会断开所有连接，约 30-60 秒恢复）");
    sync();
    /* 延迟 2 秒，让这次的 HTTP 响应先发出去 */
    pid_t pid = fork();
    if (pid == 0) {
        sleep(2);
        execl("/sbin/reboot", "reboot", (char *)NULL);
        execl("/bin/reboot", "reboot", (char *)NULL);
        _exit(0);
    }
    char text[300];
    snprintf(text, sizeof(text),
             "设备将在 2 秒后重启。\n\n"
             "所有连接会断开，大约 30-60 秒后恢复。\n\n"
             "重启后 llama-server 和 agent 由 init.d 自动拉起。");
    return json_pair(text, "已安排重启（2 秒后）", NULL);
}

/* ---------------- op: opkg_list ----------------
 * 已安装软件包清单，可按名字过滤（只读）。
 */
static char *op_opkg_list(const char *pattern) {
    char out[OUTMAX];
    char *argv[] = { (char *)"/bin/opkg", (char *)"list-installed", NULL };
    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0) return err_json("opkg list-installed 失败（退出码 %d）", rc);

    /* 按 pattern 过滤 + 转成表格 */
    char text[OUTMAX];
    size_t ti = 0;
    int cnt = 0;
    ti += snprintf(text + ti, sizeof(text) - ti, "| 软件包 | 版本 |\n|---|---|\n");
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line && ti + 200 < sizeof(text);
         line = strtok_r(NULL, "\n", &save)) {
        if (pattern && pattern[0] && !strstr(line, pattern)) continue;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (++cnt > 80) continue;
        ti += snprintf(text + ti, sizeof(text) - ti, "| `%s` | %s |\n", line, sp + 1);
    }
    if (cnt == 0) return err_json("没有匹配的软件包（共扫描完 opkg 列表）");
    char ans[200];
    snprintf(ans, sizeof(ans), "已装 %d 个包%s", cnt,
             (pattern && pattern[0]) ? "（已过滤）" : "");
    return json_pair(text, ans, NULL);
}

static char *op_service(const char *name, const char *action) {
    if (!safe_token(name, 48, 0)) return err_json("服务名不合法：%s", name);
    const char *a = (action && action[0]) ? action : "status";
    if (strcmp(a, "status") && strcmp(a, "reload") && strcmp(a, "restart")
        && strcmp(a, "start") && strcmp(a, "stop") && strcmp(a, "enable") && strcmp(a, "disable"))
        return err_json("不支持的动作：%s", a);

    char script[160];
    snprintf(script, sizeof(script), "/etc/init.d/%s", name);
    if (access(script, X_OK) != 0) return err_json("没有这个服务：%s", name);

    char out[8192];
    char *argv[] = { script, (char *)a, NULL };
    int rc = run_argv(argv, out, sizeof(out));

    char text[4096];
    snprintf(text, sizeof(text), "`%s %s` → 退出码 %d\n\n```\n%.2000s```", name, a, rc, out);
    char ans[160];
    snprintf(ans, sizeof(ans), "%s %s %s", name, a, rc == 0 ? "成功" : "返回非 0");
    return json_pair(text, ans,
        (!strcmp(a, "stop") || !strcmp(a, "restart"))
            ? "停止/重启网络类服务可能导致当前连接中断。" : NULL);
}

/* ---------------- op: logread ---------------- */
/* 不区分大小写的子串查找（musl 的 strcasestr 不可靠，自己写） */
static int has_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k]
               && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) return 1;
    }
    return 0;
}

/* 读系统日志。
 * pattern 非空时只保留包含它的行 —— "日志里的 404" 这类需求必须先过滤，
 * 否则只能盲吐最近 N 行，用户要的东西淹没在里面。 */
static char *op_logread(int lines, const char *pattern) {
    if (lines <= 0) lines = 30;
    if (lines > 300) lines = 300;
    char out[OUTMAX];
    char *argv[] = { (char *)"/sbin/logread", NULL };
    int rc = run_argv(argv, out, sizeof(out));
    if (rc != 0 || !out[0]) return err_json("logread 失败");

    int total = 0;
    int n = 0;
    char *keep[300];
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        total++;
        if (pattern && pattern[0] && !has_ci(line, pattern)) continue;
        keep[n % 300] = line;
        n++;
    }

    int show = n < lines ? n : lines;
    char text[OUTMAX];
    size_t i;
    if (pattern && pattern[0]) {
        if (n == 0) {
            snprintf(text, sizeof(text),
                     "日志里没有含 **%s** 的记录（共扫了 %d 行）。\n\n"
                     "可以换个关键字，或者不带关键字看最近几行。", pattern, total);
            char a[160];
            snprintf(a, sizeof(a), "没找到含 %s 的日志（共 %d 行）", pattern, total);
            return json_pair(text, a, NULL);
        }
        i = (size_t)snprintf(text, sizeof(text),
                             "日志里含 **%s** 的记录：共 %d 行（总 %d 行），显示最近 %d 行\n\n```\n",
                             pattern, n, total, show);
    } else {
        i = (size_t)snprintf(text, sizeof(text), "最近 %d 行日志（总共 %d 行）：\n\n```\n",
                             show, total);
    }
    for (int k = n - show; k < n && i < OUTMAX - 400; k++)
        i += (size_t)snprintf(text + i, OUTMAX - i, "%s\n", keep[k % 300]);
    snprintf(text + i, OUTMAX - i, "```");

    char ans[200];
    if (pattern && pattern[0])
        snprintf(ans, sizeof(ans), "含 %s 的日志 %d 行（总 %d 行）", pattern, n, total);
    else
        snprintf(ans, sizeof(ans), "最近 %d 行日志（总 %d 行）", show, total);
    return json_pair(text, ans, NULL);
}

static char *run(const char *json_in) {
    char op[32] = {0}, key[160] = {0}, value[600] = {0};
    char pkg[80] = {0}, iface[40] = {0}, sub[32] = {0};
    char reload[64] = {0}, name[64] = {0}, action[32] = {0};
    char user[40] = {0}, pass[160] = {0}, confirm[16] = {0};

    jget(json_in, "op", op, sizeof(op));
    jget(json_in, "key", key, sizeof(key));
    jget(json_in, "value", value, sizeof(value));
    jget(json_in, "package", pkg, sizeof(pkg));
    jget(json_in, "iface", iface, sizeof(iface));
    jget(json_in, "sub", sub, sizeof(sub));
    jget(json_in, "reload", reload, sizeof(reload));
    jget(json_in, "name", name, sizeof(name));
    jget(json_in, "action", action, sizeof(action));
    jget(json_in, "user", user, sizeof(user));
    jget(json_in, "pass", pass, sizeof(pass));
    jget(json_in, "confirm", confirm, sizeof(confirm));

    if (!op[0]) {
        /* 模型常把子命令当工具名写（{"tool":"uci_show"}），
         * 框架按别名前缀路由到这里，这里把 tool 名拿回来当 op。 */
        char toolname[40] = {0};
        jget(json_in, "tool", toolname, sizeof(toolname));
        if (toolname[0] && strcmp(toolname, "owrt") != 0
            && strcmp(toolname, "openwrt") != 0 && strcmp(toolname, "config") != 0)
            snprintf(op, sizeof(op), "%s", toolname);
    }
    if (!op[0]) {
        /* 猜：有 key+value 是改，有 key 是查，有 iface 是无线 */
        if (key[0] && value[0])      snprintf(op, sizeof(op), "uci_set");
        else if (key[0])             snprintf(op, sizeof(op), "uci_get");
        else if (iface[0])           snprintf(op, sizeof(op), "iwinfo");
        else                         snprintf(op, sizeof(op), "uci_show");
    }

    if (!strcmp(op, "uci_show") || !strcmp(op, "uci") || !strcmp(op, "show"))
        return op_uci_show(pkg);
    if (!strcmp(op, "uci_get") || !strcmp(op, "get"))
        return op_uci_get(key);
    if (!strcmp(op, "uci_changes") || !strcmp(op, "changes"))
        return op_uci_changes();
    if (!strcmp(op, "uci_set") || !strcmp(op, "set"))
        return op_uci_set(key, value, "set");
    if (!strcmp(op, "uci_add_list") || !strcmp(op, "add_list"))
        return op_uci_set(key, value, "add_list");
    if (!strcmp(op, "uci_del_list") || !strcmp(op, "del_list"))
        return op_uci_set(key, value, "del_list");
    if (!strcmp(op, "uci_delete") || !strcmp(op, "delete"))
        return op_uci_set(key, NULL, "delete");
    if (!strcmp(op, "uci_commit") || !strcmp(op, "commit"))
        return op_uci_commit(pkg, reload);
    if (!strcmp(op, "uci_revert") || !strcmp(op, "revert"))
        return op_uci_revert(pkg);
    if (!strcmp(op, "iwinfo"))
        return op_iwinfo(iface, sub);
    if (!strcmp(op, "iw"))
        return op_iw(sub[0] ? sub : "dev", iface);
    if (!strcmp(op, "service") || !strcmp(op, "initd"))
        return op_service(name[0] ? name : (pkg[0] ? pkg : ""), action);
    if (!strcmp(op, "logread") || !strcmp(op, "log"))
        return op_logread(jget_int(json_in, "lines", 30), key);
    if (!strcmp(op, "passwd"))
        return op_passwd(user, pass, confirm);
    if (!strcmp(op, "reboot"))
        return op_reboot(confirm);
    if (!strcmp(op, "opkg_list") || !strcmp(op, "packages"))
        return op_opkg_list(key[0] ? key : NULL);

    /* 常见误用：把别的插件的 op 丢给 owrt。直接说清该去哪，别让模型瞎试。 */
    if (!strcmp(op, "grep") || !strcmp(op, "find") || !strcmp(op, "ls")
        || !strcmp(op, "cat") || !strcmp(op, "read") || !strcmp(op, "du"))
        return err_json("op `%s` 不属于 owrt 插件，它是 sys 插件的。"
                        "改用 {\"op\":\"%s\", ...}（不要指定 plugin=owrt，框架会自动路由到 sys）",
                        op, op);
    if (!strcmp(op, "calc") || !strcmp(op, "solve"))
        return err_json("op `%s` 属于 math 插件，不是 owrt。", op);
    if (!strcmp(op, "probe") || !strcmp(op, "login"))
        return err_json("op `%s` 属于 browser 插件，不是 owrt。", op);

    return err_json("不支持的 op：%s（uci_show/get/set/add_list/del_list/delete/commit/revert, "
                    "iwinfo, iw, service, logread, passwd, reboot, opkg_list）", op);
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "owrt",
    "OpenWrt 配置：uci 查询/修改/提交(uci_*) 无线信息(iwinfo) 无线诊断(iw) "
    "服务控制(service) 系统日志(logread) 改密码(passwd) 重启(reboot) "
    "已装包列表(opkg_list)。改配置分两步，commit 才生效；reboot 必须 confirm:yes",
    "{\"op\":\"uci_show\",\"package\":\"network\"}"
    "  或 {\"op\":\"uci_get\",\"key\":\"network.lan.ipaddr\"}"
    "  或 {\"op\":\"uci_set\",\"key\":\"network.lan.ipaddr\",\"value\":\"192.168.1.1\"}"
    "  或 {\"op\":\"uci_commit\",\"package\":\"network\",\"reload\":\"network\"}"
    "  或 {\"op\":\"uci_changes\"}  查未提交改动"
    "  或 {\"op\":\"iwinfo\",\"iface\":\"wlan0\",\"sub\":\"assoclist\"}"
    "  或 {\"op\":\"iw\",\"sub\":\"station\",\"iface\":\"wlan0\"}"
    "  或 {\"op\":\"service\",\"name\":\"dnsmasq\",\"action\":\"restart\"}"
    "  或 {\"op\":\"logread\",\"lines\":40}",
    run,
    "uci,iwinfo,iw,openwrt,owrt,config,service,logread"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
