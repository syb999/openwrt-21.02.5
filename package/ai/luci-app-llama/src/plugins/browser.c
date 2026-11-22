/* ============================================================
 * browser.c — 登录态网站抓取插件（chrome-headless-shell + selenium）
 *
 * 免登录的页面用 curl 插件就够了；这个插件专治需要登录的站点：
 * 登录框没有统一的接口，只能真的把浏览器跑起来，填表、点按钮，
 * 再把 cookie 掏出来。
 *
 * 模型怎么用（两轮）：
 *   1. {"op":"probe","url":"https://x.com/login"}
 *      → 回来一串 inputs / buttons，每个带 i 下标和 label/placeholder
 *   2. {"op":"login","url":"...","steps":[
 *        {"act":"fill","i":0,"value":"账号"},
 *        {"act":"fill","i":1,"value":"密码"},
 *        {"act":"click","i":0}]}
 *      → 回来 cookies 数组，直接拿去喂 curl 插件
 *
 * 为什么 login 必须一次跑完：这块板子的 chromedriver 不能带 --user-data-dir
 * （chroot 里找不到 DevToolsActivePort，60 秒后报 unable to discover open pages），
 * 会话没法跨进程复用，所以登录流程得在同一个浏览器进程里走完。
 *
 * 内存：chrome 起来要 300MB 上下。可用内存低于 350MB 时脚本会直接拒绝，
 * 提示先跑 /root/freemm.sh 重启释放。
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>

#include "plugin.h"
#include "py_browse.h"

#define BZ_OUT_MAX (2 * 1024 * 1024)     /* cookie 多的时候能到几百 KB */
#define BZ_TIMEOUT 200                   /* 秒。chrome 冷启动 + 等跳转，留足余量 */

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
        if (*p == '\\' && p[1]) { p++; }
        out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

/* fork 出 python3，脚本走 -c 传，参数走 stdin */
static char *run_py(const char *op, const char *json) {
    int in[2], ou[2];
    if (pipe(in) != 0) return strdup("{\"error\":\"pipe 失败\"}");
    if (pipe(ou) != 0) { close(in[0]); close(in[1]); return strdup("{\"error\":\"pipe 失败\"}"); }

    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(ou[0]); close(ou[1]);
        return strdup("{\"error\":\"fork 失败\"}");
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(ou[1], 1);
        dup2(ou[1], 2);
        close(in[0]); close(in[1]);
        close(ou[0]); close(ou[1]);
        char *argv[] = {
            (char *)"python3",
            (char *)"-c",
            (char *)PY_BROWSE_SRC,
            (char *)op,
            NULL
        };
        execv("/usr/bin/python3", argv);
        _exit(127);
    }

    close(in[0]);
    close(ou[1]);

    size_t len = strlen(json), off = 0;
    while (off < len) {
        ssize_t w = write(in[1], json + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(in[1]);

    /* 超时兜底：chrome 偶尔会卡在等待某个永不结束的请求上，
     * 不设上限的话 agent 会一直挂在那儿（手停都停不掉） */
    alarm((unsigned)BZ_TIMEOUT);

    char *buf = malloc(BZ_OUT_MAX);
    if (!buf) {
        close(ou[0]);
        waitpid(pid, NULL, 0);
        alarm(0);
        return NULL;
    }
    size_t n = 0;
    ssize_t r;
    int timed_out = 0;
    while ((r = read(ou[0], buf + n, BZ_OUT_MAX - 1 - n)) > 0) {
        n += (size_t)r;
        if (n + 1 >= BZ_OUT_MAX) break;
    }
    buf[n] = 0;
    close(ou[0]);
    alarm(0);

    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == 0) {
        /* 还活着 → 上面是超时中断的，强杀 */
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        timed_out = 1;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) {
        free(buf);
        return strdup("{\"error\":\"找不到 python3，browser 插件用不了\"}");
    }
    if (timed_out && n == 0) {
        free(buf);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "{\"error\":\"浏览器 %d 秒没出结果（页面卡住或内存不足）。"
                 "可以跑 /root/freemm.sh 重启设备再试\"}", BZ_TIMEOUT);
        return strdup(msg);
    }
    if (!n) {
        free(buf);
        return strdup("{\"error\":\"browser 脚本没有输出（chrome 可能起不来，看 /tmp/browse/chromedriver.log）\"}");
    }
    return buf;
}

static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    if (!op[0]) {
        return strdup("{\"error\":\"缺少 op：probe 探测登录框 / login 填表点击取 cookie / "
                      "cookie 直接导出 / shot 截图\"}");
    }
    if (strcmp(op, "probe") && strcmp(op, "login") && strcmp(op, "autologin")
        && strcmp(op, "cookie") && strcmp(op, "shot")
        && strcmp(op, "captcha") && strcmp(op, "hunt") && strcmp(op, "drag")) {
        char m[260];
        snprintf(m, sizeof(m),
                 "{\"error\":\"未知 op '%s'：支持 probe / login / autologin / "
                 "cookie / shot / captcha / hunt / drag\"}", op);
        return strdup(m);
    }
    return run_py(op, json_in);
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "browser",
    "登录态网站（chrome+selenium）：probe 探测登录框/按钮/验证码 → "
    "autologin 自动猜输入框填表点击 / login 按步骤执行 → 导出 cookie。免登录的网站用 curl 更快",
    "探测 {\"op\":\"probe\",\"url\":\"https://x.com/login\"}"
    " ｜登录 {\"op\":\"login\",\"url\":\"...\",\"steps\":["
    "{\"act\":\"fill\",\"i\":0,\"value\":\"账号\"},"
    "{\"act\":\"fill\",\"i\":1,\"value\":\"密码\"},"
    "{\"act\":\"click\",\"i\":0}]}"
    " ｜自动登录 {\"op\":\"autologin\",\"url\":\"...\",\"user\":\"账号\",\"pass\":\"密码\"}"
    " ｜取 cookie {\"op\":\"cookie\",\"url\":\"...\"}"
    " ｜截图 {\"op\":\"shot\",\"url\":\"...\",\"path\":\"/tmp/a.png\"}",
    run,
    "browse,selenium,chrome,login,cookie,登录,验证码,抓取"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
