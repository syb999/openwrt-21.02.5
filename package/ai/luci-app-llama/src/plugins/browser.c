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
#include <ctype.h>

#include "plugin.h"
#include "py_browse.h"

#define BZ_OUT_MAX (2 * 1024 * 1024)     /* cookie 多的时候能到几百 KB */
#define BZ_TIMEOUT 200                   /* 秒。chrome 冷启动 + 等跳转，留足余量 */

static char *jerr(const char *msg) {
    char *o = malloc(strlen(msg) + 32);
    if (!o) return NULL;
    /* 简单转义：引号换行 */
    char *d = o; *d++ = '{'; 
    const char *hdr = "\"error\":\"";
    memcpy(d, hdr, strlen(hdr)); d += strlen(hdr);
    for (const char *p = msg; *p; p++) {
        if (*p == '"' || *p == '\\') { *d++ = '\\'; *d++ = *p; }
        else if (*p == '\n') { *d++ = '\\'; *d++ = 'n'; }
        else *d++ = *p;
    }
    *d++ = '"'; *d++ = '}'; *d = 0;
    return o;
}

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


/* ---------------- 由一段 HTML 生成 Selenium 定位/操作代码 ----------------
 * 场景：用户在浏览器里"检查元素"复制了一段标签，想让脚本能操作它。
 * 这里把标签里的 id/name/class/type/placeholder/aria-label/text 都解析出来，
 * 生成 CSS / XPath（多种）/ JS 三套定位，再按元素类型给出对应操作代码。
 * 纯字符串处理，不依赖 selenium、不启动浏览器 —— 板上没装 python 也能用。
 */
typedef struct { char k[40]; char v[320]; } hattr_t;

static int h_attr_find(hattr_t *a, int n, const char *k, char *out, size_t cap) {
    for (int i = 0; i < n; i++)
        if (!strcasecmp(a[i].k, k)) { snprintf(out, cap, "%s", a[i].v); return 1; }
    return 0;
}

/* 解析 <tag k="v" k2='v2' ...> —— 只要第一个标签，够用 */
static void h_parse(const char *html, char *tag, size_t tcap, hattr_t *a, int *an, int amax) {
    *an = 0; tag[0] = 0;
    const char *p = strchr(html, '<');
    if (!p) return;
    p++;
    while (*p == '/' || *p == '!' || *p == '?') p++;      /* 跳过注释/闭合/声明 */
    size_t i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_') && i + 1 < tcap)
        tag[i++] = *p++;
    tag[i] = 0;
    for (int k = 0; k < (int)strlen(tag); k++) tag[k] = (char)tolower((unsigned char)tag[k]);

    while (*p && *p != '>' && *an < amax) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '>' || *p == '/' || !*p) break;
        char kn[40]; size_t ki = 0;
        while (*p && *p != '=' && *p != ' ' && *p != '>' && ki + 1 < sizeof(kn))
            kn[ki++] = *p++;
        kn[ki] = 0;
        while (*p == ' ') p++;
        char val[320] = {0};
        if (*p == '=') {
            p++;
            while (*p == ' ') p++;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                size_t vi = 0;
                while (*p && *p != q && vi + 1 < sizeof(val)) val[vi++] = *p++;
                val[vi] = 0;
                if (*p == q) p++;
            } else {
                size_t vi = 0;
                while (*p && *p != ' ' && *p != '>' && vi + 1 < sizeof(val)) val[vi++] = *p++;
                val[vi] = 0;
            }
        }
        if (kn[0]) {
            snprintf(a[*an].k, sizeof(a[*an].k), "%s", kn);
            snprintf(a[*an].v, sizeof(a[*an].v), "%s", val);
            (*an)++;
        } else {
            while (*p && *p != ' ' && *p != '>') p++;
        }
    }
}

/* 找标签内的可见文本（<button>登录</button> 这种） */
static void h_text(const char *html, char *out, size_t cap) {
    out[0] = 0;
    const char *gt = strchr(html, '>');
    if (!gt) return;
    gt++;
    const char *end = strchr(gt, '<');
    if (!end) end = gt + strlen(gt);
    size_t n = (size_t)(end - gt);
    while (n && (gt[0] == ' ' || gt[0] == '\n' || gt[0] == '\t' || gt[0] == '\r')) { gt++; n--; }
    while (n && (gt[n-1] == ' ' || gt[n-1] == '\n' || gt[n-1] == '\t' || gt[n-1] == '\r')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, gt, n); out[n] = 0;
}


/* JSON 反转义：前端 JSON.stringify 之后，正文里的 " 会变成 \" ，
 * 不解开的话解析器拿到的是 <input type=\"text\" …> → 属性全读成带反斜杠的垃圾，
 * 生成的代码看起来就"跟输入无关"。 */
static void jun_esc(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
                case 'n': *w++ = '\n'; break;
                case 't': *w++ = '\t'; break;
                case 'r': *w++ = '\r'; break;
                case '"': *w++ = '"'; break;
                case '\\': *w++ = '\\'; break;
                case '/': *w++ = '/'; break;
                default: *w++ = *r; break;
            }
            r++;
        } else *w++ = *r++;
    }
    *w = 0;
}

typedef struct {
    char tag[24], id[256], nm[256], cls[512], ph[256], typ[48], txt[160];
    char aria[256], title[256], href[512], role[64], vmodel[128], onclick[256], value[256];
    char data[3][128]; int ndata;
    int depth;
} hnode_t;

static int h_scan(const char *html, hnode_t *out, int max) {
    int n = 0, depth = 0;
    const char *p = html;
    while (*p && n < max) {
        if (*p != '<') { p++; continue; }
        if (p[1] == '/') { if (depth > 0) depth--; p += 2; while (*p && *p != '>') p++; if (*p) p++; continue; }
        if (p[1] == '!' || p[1] == '?' || p[1] == 0) { while (*p && *p != '>') p++; if (*p) p++; continue; }
        hattr_t a[32]; int an = 0; char tag[24] = {0};
        h_parse(p, tag, sizeof(tag), a, &an, 32);
        if (!tag[0]) { p++; continue; }
        hnode_t *h = &out[n];
        memset(h, 0, sizeof(*h));
        snprintf(h->tag, sizeof(h->tag), "%s", tag);
        h->depth = depth;
        h_attr_find(a, an, "id", h->id, sizeof(h->id));
        h_attr_find(a, an, "name", h->nm, sizeof(h->nm));
        h_attr_find(a, an, "class", h->cls, sizeof(h->cls));
        h_attr_find(a, an, "placeholder", h->ph, sizeof(h->ph));
        h_attr_find(a, an, "type", h->typ, sizeof(h->typ));
        h_attr_find(a, an, "aria-label", h->aria, sizeof(h->aria));
        h_attr_find(a, an, "title", h->title, sizeof(h->title));
        h_attr_find(a, an, "href", h->href, sizeof(h->href));
        h_attr_find(a, an, "role", h->role, sizeof(h->role));
        h_attr_find(a, an, "onclick", h->onclick, sizeof(h->onclick));
        h_attr_find(a, an, "value", h->value, sizeof(h->value));
        if (!h_attr_find(a, an, "v-model", h->vmodel, sizeof(h->vmodel)))
            h_attr_find(a, an, "ng-model", h->vmodel, sizeof(h->vmodel));
        for (int i = 0; i < an && h->ndata < 3; i++)
            if (!strncmp(a[i].k, "data-", 5) && strlen(a[i].v) <= 100) {
                snprintf(h->data[h->ndata], sizeof(h->data[0]), "%s=%s", a[i].k + 5, a[i].v);
                h->ndata++;
            }
        const char *gt = strchr(p, '>');
        if (gt) {
            const char *b = gt + 1, *e = strchr(b, '<');
            if (!e) e = b + strlen(b);
            while (b < e && (*b == ' ' || *b == '\n' || *b == '\t' || *b == '\r')) b++;
            while (e > b && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\t' || e[-1] == '\r')) e--;
            size_t L = (size_t)(e - b);
            if (L >= sizeof(h->txt)) L = sizeof(h->txt) - 1;
            memcpy(h->txt, b, L); h->txt[L] = 0;
        }
        n++; depth++;
        while (*p && *p != '>') p++;
        if (*p) p++;
    }
    return n;
}

/* class 里挑最稳的：跳过通用名；含随机哈希（css-x1y2z3 / 32位hex / 数字结尾很长）判为不稳定 */
static int looks_random(const char *s) {
    int digit = 0, alpha = 0, len = (int)strlen(s);
    for (const char *p = s; *p; p++) { if (isdigit((unsigned char)*p)) digit++; else if (isalpha((unsigned char)*p)) alpha++; }
    if (len >= 12 && digit >= 4 && alpha >= 3) return 1;                    /* 混合长串 */
    if (len >= 6 && strchr(s, '-')) {
        const char *dash = strrchr(s, '-');
        int tail = (int)strlen(dash + 1);
        if (tail >= 5 && digit >= 2) return 1;                              /* css-1a2b3c */
    }
    return 0;
}

static void h_best_class(const char *cls, char *out, size_t cap, int *unstable) {
    static const char *GENERIC[] = {"iconfont", "icon", "fa", "fas", "far", "fab", "glyphicon",
        "btn", "button", "input", "form-control", "active", "clearfix", "container", "row",
        "col", "text", "item", "list", "wrap", "inner", "primary", "default", "small", "large",
        "left", "right", "center", "flex", "block", "inline", NULL};
    out[0] = 0; if (unstable) *unstable = 0;
    const char *p = cls;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *b = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t L = (size_t)(p - b);
        if (!L || L >= 100) continue;
        char tok[128]; memcpy(tok, b, L); tok[L] = 0;
        int generic = 0;
        for (int i = 0; GENERIC[i]; i++) if (!strcasecmp(tok, GENERIC[i])) { generic = 1; break; }
        if (generic) continue;
        if (strlen(tok) > strlen(out)) {
            snprintf(out, cap, "%s", tok);
            if (unstable) *unstable = looks_random(tok);
        }
    }
}

static const char *h_role(const hnode_t *h) {
    if (!strcasecmp(h->tag, "select")) return "下拉选择";
    if (!strcasecmp(h->tag, "textarea")) return "多行输入";
    if (!strcasecmp(h->tag, "input")) {
        if (!strcasecmp(h->typ, "checkbox")) return "复选框";
        if (!strcasecmp(h->typ, "radio")) return "单选框";
        if (!strcasecmp(h->typ, "submit") || !strcasecmp(h->typ, "button")) return "按钮";
        if (!strcasecmp(h->typ, "file")) return "文件上传";
        if (!strcasecmp(h->typ, "password")) return "密码框";
        if (!strcasecmp(h->typ, "search")) return "搜索框";
        return "输入框";
    }
    if (!strcasecmp(h->tag, "button")) return "按钮";
    if (!strcasecmp(h->tag, "a")) return "链接";
    if (!strcasecmp(h->tag, "img")) return "图片";
    if (!strcasecmp(h->tag, "form")) return "表单";
    if (!strcasecmp(h->tag, "iframe")) return "内嵌页";
    if (!strcasecmp(h->tag, "i") || !strcasecmp(h->tag, "svg") || strstr(h->cls, "icon"))
        return "图标";
    if (!strcasecmp(h->tag, "label")) return "标签";
    if (!strcasecmp(h->tag, "li")) return "列表项";
    if (!strcmp(h->role, "button")) return "按钮(role)";
    return "容器/其它";
}

/* 从 class/placeholder/文本里猜用途，给"该怎么操作"的建议 */
static void guess_purpose(const hnode_t *h, char *out, size_t cap) {
    out[0] = 0;
    struct { const char *kw; const char *say; } TB[] = {
        {"phone", "手机号相关"}, {"mobile", "手机号相关"}, {"shouji", "手机号相关"},
        {"手机", "手机号相关"}, {"手机号", "手机号相关"},
        {"搜索", "搜索框/搜索按钮"}, {"密码", "密码框"}, {"登录", "登录"}, {"登陆", "登录"},
        {"验证码", "验证码"}, {"发送", "发送（验证码/消息）"}, {"提交", "提交"},
        {"关闭", "关闭"}, {"删除", "删除"}, {"编辑", "编辑"}, {"上传", "上传"},
        {"购物车", "购物车"}, {"菜单", "菜单"}, {"确定", "确认按钮"}, {"取消", "取消按钮"},
        {"search", "搜索框/搜索按钮"}, {"sousuo", "搜索"}, {"login", "登录"}, {"denglu", "登录"},
        {"pass", "密码"}, {"captcha", "验证码"}, {"code", "验证码/短码"}, {"send", "发送（验证码/消息）"},
        {"submit", "提交"}, {"close", "关闭"}, {"next", "下一步"}, {"prev", "上一步"},
        {"cart", "购物车"}, {"menu", "菜单"}, {"nav", "导航"}, {"del", "删除"}, {"edit", "编辑"},
        {"upload", "上传"}, {"file", "文件"}, {"avatar", "头像"}, {"tab", "标签页"},
        {NULL, NULL}
    };
    char low[700];
    snprintf(low, sizeof(low), "%s %s %s %s %s", h->ph, h->aria, h->txt, h->cls, h->nm);
    for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (int i = 0; TB[i].kw; i++)
        if (strstr(low, TB[i].kw)) { snprintf(out, cap, "%s", TB[i].say); return; }
    if (h->typ[0]) snprintf(out, cap, "type=%s", h->typ);
}

/* 框架识别 */
static const char *guess_framework(const hnode_t *h, const char *html) {
    if (h->vmodel[0] || strstr(html, "v-model") || strstr(html, "@click")) return "Vue";
    if (strstr(html, "data-reactroot") || strstr(html, "react")) return "React";
    if (strstr(html, "ng-") || h->vmodel[0]) return "Angular";
    if (strstr(html, "svelte")) return "Svelte";
    return "";
}


static char *run_gen(const char *json_in) {
    char html[2000] = {0}, action[24] = "auto", var[40] = "el";
    if (jget(json_in, "html", html, sizeof(html)) != 0 || !html[0])
        return jerr("要给 html=在浏览器里检查元素复制的那段标签");
    { char *r = html, *w = html;
      while (*r) { if (*r == '\\' && r[1]) { r++; if (*r == 'n') *w++ = '\n'; else if (*r == 't') *w++ = '\t'; else *w++ = *r; r++; } else *w++ = *r++; }
      *w = 0; }
    jget(json_in, "action", action, sizeof(action));
    if (!action[0]) snprintf(action, sizeof(action), "auto");
    jget(json_in, "var", var, sizeof(var));
    (void)var;

    hnode_t nd[16]; int nn = h_scan(html, nd, 16);
    if (!nn) return jerr("解析不出标签，确认给的是元素本身的 HTML");

    int best = -1, bs = -1;
    for (int i = 0; i < nn; i++) {
        int sc = nd[i].depth;
        if (nd[i].id[0]) sc += 100;
        if (nd[i].nm[0]) sc += 60;
        if (nd[i].aria[0]) sc += 40;
        char bc[200]; int uf = 0; h_best_class(nd[i].cls, bc, sizeof(bc), &uf);
        if (bc[0]) sc += uf ? 15 : 30;
        if (nd[i].ph[0]) sc += 20;
        if (nd[i].txt[0]) sc += 15;
        if (sc > bs) { bs = sc; best = i; }
    }
    if (best < 0) best = 0;
    hnode_t *t = &nd[best];
    char tc[200]; int tuns = 0; h_best_class(t->cls, tc, sizeof(tc), &tuns);
    const char *role = h_role(t);
    char purpose[64]; guess_purpose(t, purpose, sizeof(purpose));
    const char *fw = guess_framework(t, html);

    char css[512] = {0}, xp[700] = {0}, xptxt[400] = {0}, xpin[700] = {0}, xparia[600] = {0};
    if (t->id[0]) snprintf(css, sizeof(css), "#%s", t->id);
    else if (t->nm[0]) snprintf(css, sizeof(css), "%s[name='%s']", t->tag, t->nm);
    else if (tc[0] && !tuns) snprintf(css, sizeof(css), "%s.%s", t->tag, tc);
    else if (t->aria[0]) snprintf(css, sizeof(css), "%s[aria-label='%s']", t->tag, t->aria);
    else if (t->ph[0]) snprintf(css, sizeof(css), "%s[placeholder='%s']", t->tag, t->ph);
    else if (tc[0]) snprintf(css, sizeof(css), "%s.%s", t->tag, tc);
    else snprintf(css, sizeof(css), "%s", t->tag);

    if (t->id[0]) snprintf(xp, sizeof(xp), "//%s[@id='%s']", t->tag, t->id);
    else if (t->nm[0]) snprintf(xp, sizeof(xp), "//%s[@name='%s']", t->tag, t->nm);
    else if (t->aria[0]) snprintf(xp, sizeof(xp), "//%s[@aria-label='%s']", t->tag, t->aria);
    else if (tc[0] && best > 0 && strcasecmp(nd[0].tag, t->tag))
        snprintf(xp, sizeof(xp), "//%s//%s[contains(@class,'%s')]", nd[0].tag, t->tag, tc);
    else if (tc[0]) snprintf(xp, sizeof(xp), "//%s[contains(@class,'%s')]", t->tag, tc);
    else snprintf(xp, sizeof(xp), "//%s", t->tag);

    if (t->txt[0]) snprintf(xptxt, sizeof(xptxt), "//%s[normalize-space()='%s']", t->tag, t->txt);
    if (t->aria[0]) snprintf(xparia, sizeof(xparia), "//%s[@aria-label='%s']", t->tag, t->aria);
    if (best > 0) snprintf(xpin, sizeof(xpin), "//%s//input | //%s//textarea", nd[0].tag, nd[0].tag);

    int is_input = strstr(role, "输入") || strstr(role, "密码") || strstr(role, "搜索");
    int is_click = !strcmp(role, "按钮") || !strcmp(role, "按钮(role)") || !strcmp(role, "链接")
                   || !strcmp(role, "复选框") || !strcmp(role, "单选框") || !strcmp(role, "图标");
    int is_select = !strcmp(role, "下拉选择");
    const char *want = action;
    if (!strcmp(action, "auto")) want = is_select ? "select" : (is_click ? "click" : (is_input ? "type" : "get_text"));

    static char t1[24000];
    size_t o = 0;
#define P(...) do { if (o < sizeof(t1) - 1) { int _w = snprintf(t1 + o, sizeof(t1) - o, __VA_ARGS__); if (_w > 0) o += (size_t)_w; if (o >= sizeof(t1)) o = sizeof(t1) - 1; } } while (0)
    P("【元素分析】\n");
    P("  目标   : <%s>  (depth=%d)\n", t->tag, t->depth);
    if (best > 0) P("  父级   : <%s>\n", nd[0].tag);
    if (purpose[0]) P("  用途猜测: %s\n", purpose);
    if (t->cls[0])  P("  class  : %s\n", t->cls);
    if (tc[0])      P("  关键class: %s%s\n", tc, tuns ? "  ⚠ 看起来是构建生成的随机名（换版本会变），不建议作为主要定位" : "（语义稳定）");
    if (t->aria[0])  P("  aria-label: %s\n", t->aria);
    if (t->ph[0])    P("  placeholder: %s\n", t->ph);
    if (t->txt[0])   P("  可见文本: %s\n", t->txt);
    if (t->href[0])  P("  href   : %s\n", strlen(t->href) > 60 ? "(很长，可能是跳转/签名链接)" : t->href);
    if (t->ndata)    P("  data-* : %s%s%s\n", t->data[0], t->ndata > 1 ? " " : "", t->ndata > 1 ? t->data[1] : "");
    if (t->onclick[0]) P("  onclick: %s\n", t->onclick);
    if (fw[0])       P("  框架   : %s%s\n", fw, !strcmp(fw, "React") ? "（受控组件：直接改 value 后要派发 input 事件，见下方 JS 段）" : "");
    if (nn > 1)      P("  候选   : 共扫到 %d 个标签，选的是最稳的那个（id/name/aria > class > 文本）\n", nn);
    P("  角色   : %s    动作: %s\n", role, want);

    P("\n【稳定性评估】\n");
    if (t->id[0])  P("  ✔ id 定位最稳（前提：不是自动生成的随机 id）\n");
    if (t->nm[0])  P("  ✔ name 稳定，表单提交也常用它\n");
    if (tuns)      P("  ✘ class 含随机后缀 → 换版本就失效，优先用 name/aria/文本定位\n");
    if (xptxt[0])  P("  △ 文本定位：多语言站/文案改动会失效，但可读性最好\n");
    if (t->aria[0]) P("  ✔ aria-label 无障碍属性，通常稳定\n");
    if (!t->id[0] && !t->nm[0] && !t->aria[0] && !xptxt[0])
        P("  △ 只有 class/tag 可依：建议在外面再包一层父级限定（例如 //%s//%s），否则可能命中多个同类元素\n",
               best > 0 ? nd[0].tag : t->tag, t->tag);

    if (!strcmp(want, "type") && !is_input) {
        P("\n⚠ 这不是输入框（%s），不能 send_keys\n", role);
        if (xpin[0]) P("    同一容器里的输入框: By.XPATH, \"%s\"\n", xpin);
        if (is_click) P("    若目的是点击它 → 用下面的 click 段；图标类建议点父级\n");
    }
    if (!strcmp(want, "click") && is_input)
        P("\n⚠ 这是输入框：click 只等于聚焦，输入请用 send_keys（下方也给了）\n");

    P("\n【定位（按推荐顺序）】\n");
    if (t->id[0])    P("  1. By.ID            : %s\n", t->id);
    if (t->nm[0])    P("  2. By.NAME          : %s\n", t->nm);
    if (xparia[0])   P("  3. By.XPATH(aria)   : %s\n", xparia);
    P("  4. By.CSS_SELECTOR  : %s\n", css);
    P("  5. By.XPATH         : %s\n", xp);
    if (xptxt[0])    P("  6. By.XPATH(文本)   : %s\n", xptxt);

    P("\n【Python + Selenium】\n");
    P("from selenium.webdriver.common.by import By\n");
    P("from selenium.webdriver.support.ui import WebDriverWait\n");
    P("from selenium.webdriver.support import expected_conditions as EC\n");
    P("from selenium.webdriver.common.keys import Keys\n\n");
    P("# 容错版：先试 CSS，失败退回 XPath（页面改版时更抗打）\n");
    P("def find(driver):\n");
    P("    try:\n");
    P("        return WebDriverWait(driver, 10).until(\n");
    P("            EC.presence_of_element_located((By.CSS_SELECTOR, \"%s\")))\n", css);
    P("    except Exception:\n");
    P("        return WebDriverWait(driver, 5).until(\n");
    P("            EC.presence_of_element_located((By.XPATH, \"%s\")))\n\n", xp);
    P("el = find(driver)\n");
    if (!strcmp(want, "type") && is_input) {
        P("el.clear()\n");
        P("el.send_keys(\"要输入的内容\")\n");
        P("# 回车提交：el.send_keys(Keys.RETURN)\n");
        P("# 有些前端框架不认 send_keys 的输入事件（比如 Vue/React 受控组件），见下面 JS 段\n");
    } else if (!strcmp(want, "select") && is_select) {
        P("from selenium.webdriver.support.ui import Select\n");
        P("Select(el).select_by_visible_text(\"选项文字\")   # 或 select_by_value / select_by_index\n");
    } else if (is_click) {
        P("try:\n");
        P("    WebDriverWait(driver, 10).until(EC.element_to_be_clickable(el)).click()\n");
        P("except Exception:\n");
        P("    # 被遮挡/动画中 → 滚动到可见再点，仍不行就 JS 点\n");
        P("    driver.execute_script(\"arguments[0].scrollIntoView({block:'center'})\", el)\n");
        P("    driver.execute_script(\"arguments[0].click()\", el)\n");
        if (!strcmp(role, "图标") && best > 0)
            P("# 图标本身常不可点，点父级更稳：\n#   driver.find_element(By.TAG_NAME, \"%s\").click()\n", nd[0].tag);
    } else {
        P("print(el.text)\n# 取属性：el.get_attribute(\"href\") / \"value\" / \"src\"\n");
    }
    P("\n【JS 兜底（绕过遮挡、适配 RxVue 受控组件）】\n");
    P("driver.execute_script(\"\"\"\n  const el = document.querySelector('%s');\n  el.scrollIntoView({block:'center'});\n", css);
    if (is_input)
        P("  const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;\n"
               "  setter.call(el, '要输入的内容');\n"
               "  el.dispatchEvent(new Event('input', {bubbles:true}));\n"
               "  el.dispatchEvent(new Event('change', {bubbles:true}));\n");
    else
        P("  el.click();\n");
    P("\"\"\")\n");
    P("\n【常见坑】\n");
    P("  · 元素在 iframe 里 → 先 driver.switch_to.frame(...)\n");
    P("  · 点击无反应 → 可能被遮罩层挡住：先关弹窗或 JS click\n");
    P("  · StaleElementReference → 页面刷新后要重新 find\n");
    if (fw[0]) P("  · %s 受控组件 → 用上面 JS 段的 native setter + input 事件\n", fw);

#undef P

    static char jout[52000];
    size_t j = 0;
    j += snprintf(jout + j, sizeof(jout) - j,
        "{\"ok\":1,\"tag\":\"%s\",\"role\":\"%s\",\"purpose\":\"%s\","
        "\"answer\":\"%s%s%s → %s\",\"text\":\"",
        t->tag, role, purpose, role, purpose[0] ? "（" : "", purpose, want);
    for (size_t k = 0; t1[k] && j + 8 < sizeof(jout); k++) {
        unsigned char c = (unsigned char)t1[k];
        if (c == '"' || c == '\\') { jout[j++] = '\\'; jout[j++] = (char)c; }
        else if (c == '\n') { jout[j++] = '\\'; jout[j++] = 'n'; }
        else if (c == '\t') { jout[j++] = '\\'; jout[j++] = 't'; }
        else jout[j++] = (char)c;
    }
    jout[j] = 0;
    snprintf(jout + j, sizeof(jout) - j, "\"}");
    return strdup(jout);
}


static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    /* 生成 Selenium 代码：纯字符串活，不启动浏览器、不依赖 python */
    if (!strcmp(op, "gen") || !strcmp(op, "locator") || !strcmp(op, "xpath")
        || !strcmp(op, "css") || !strcmp(op, "selenium") || !strcmp(op, "code"))
        return run_gen(json_in);
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
    " ｜截图 {\"op\":\"shot\",\"url\":\"...\",\"path\":\"/tmp/a.png\"}"
    " ｜生成定位/操作代码 {\"op\":\"gen\",\"html\":\"<input name=\\\"wd\\\" class=\\\"s\\\">\",\"action\":\"type\"}"
    "（action 可 type/click/select/hover/scroll/get_text/auto；不启浏览器，纯生成）",
    run,
    "browse,selenium,chrome,login,cookie,登录,验证码,抓取"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
