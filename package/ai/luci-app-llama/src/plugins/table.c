/* table.c — 表格插件
 *
 * 按列排序取 TOP/BOTTOM，外加多条件筛选（同时满足 a 列…、b 列…）。
 *
 * 入参（都是可选的，能给的都给最好）：
 *   {"path":"/tmp/x.csv",         数据文件；不给就按列名自动找
 *    "sort_by":"播放量",           按这一列排
 *    "order":"desc|asc",           desc=最高的在前（默认），asc=最低的在前
 *    "limit":10,                   取几条
 *    "where":[{"col":"作者","op":"contains","val":"小明"},
 *             {"col":"评分","op":">=","val":"8"}],   多个条件是 AND
 *    "_task":"展示表格内 播放量 top10"}   —— 框架会把原任务塞进来
 *
 * 数据格式：自动识别分隔符（doc 解析 xlsx 出的是 \t，jq 导出的是 ,，老格式是 |），
 * 单元格两侧的引号会去掉。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "plugin.h"

/* 当前表格的分隔符：\t（doc 解析 xlsx）/ ,（jq 导出 csv）/ |（老格式） */
static char g_delim = '|';

#define MAX_ROWS 30000
#define MAX_COLS 64
#define LINE 2048
#define MAX_CONDS 6

static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    /* 只认「后面紧跟冒号」的匹配：字段值恰好等于某个 key 名时会误命中 */
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return -1;
    p = found + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
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

static int jget_num(const char *js, const char *key, int dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return dflt;
    p = strchr(p + strlen(pat), ':');
    if (!p) return dflt;
    return atoi(p + 1);
}

/* "1:28" → 88，"387.3万" → 3873000，"1,234" → 1234 */
static double num_of(const char *s) {
    char buf[64];
    size_t n = 0;
    while (*s == ' ') s++;
    for (; *s && n < sizeof(buf) - 1; s++) {
        if (*s == ',' && g_delim != ',') continue;
        if (*s == g_delim) break;
        buf[n++] = *s;
    }
    buf[n] = 0;
    double mul = 1;
    if (n >= 3 && strcmp(buf + n - 3, "万") == 0) { mul = 10000; buf[n - 3] = 0; }
    else if (n >= 3 && strcmp(buf + n - 3, "亿") == 0) { mul = 1e8; buf[n - 3] = 0; }
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) {
        int mm = 0, ss = 0;
        if (sscanf(buf, "%d:%d", &mm, &ss) == 2) return mm * 60.0 + ss;
        return 0;
    }
    return v * mul;
}

typedef struct {
    double key;
    char c[5][128];      /* 只存要显示的列（第0列 + 排序列 + 筛选列 + 分组列），
                          * 整行存 3 万行会把内存吃光 */
} row_t;

/* 按字节截断但保证不劈开 UTF-8 字符 */
static void cut_utf8(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (src[n] && n + 1 < cap) n++;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* 单元格里的 '|' 会破坏 markdown 表格，换成全角 ｜（U+FF5C，3 字节）。
 * 以前写成单字节 0xA6 —— 那不是合法的 UTF-8，页面和终端都会显示成 �，
 * 看着像"源数据非法"，其实是这里换坏的。 */
static void md_cell(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 8 < cap; p++) {
        if (*p == '|') {
            dst[o++] = (char)0xEF; dst[o++] = (char)0xBD; dst[o++] = (char)0x9C;
        } else if (*p == '"') {
            /* value 这段是手拼的 JSON 字符串，标题里的双引号必须转义，
             * 否则 {"value":"…| 带"引号"的标题 |…"} 直接非法 —— 前端
             * JSON.parse 失败，整张表退化成一行文本（用户看到的就是这个）。 */
            dst[o++] = '\\'; dst[o++] = '"';
        } else if (*p == '\\') {
            dst[o++] = '\\'; dst[o++] = '\\';
        } else if ((unsigned char)*p < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
}

/* 只做 JSON 转义（条件描述/答案里也可能出现引号） */
static void jesc_t(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 4 < cap; p++) {
        if (*p == '"' || *p == '\\') { dst[o++] = '\\'; dst[o++] = *p; }
        else if ((unsigned char)*p < 0x20) dst[o++] = ' ';
        else dst[o++] = *p;
    }
    dst[o] = 0;
}

static int cmp_desc(const void *a, const void *b) {
    const row_t *x = a, *y = b;
    if (x->key < y->key) return 1;
    if (x->key > y->key) return -1;
    return 0;
}
static int cmp_asc(const void *a, const void *b) { return -cmp_desc(a, b); }

/* ---------- 分隔符 / 单元格 ---------- */
static void detect_delim(const char *line) {
    int nt = 0, nc = 0, np = 0, q = 0;
    for (const char *p = line; *p; p++) {
        if (*p == '"') { q = !q; continue; }
        if (q) continue;
        if (*p == '\t') nt++;
        else if (*p == ',') nc++;
        else if (*p == '|') np++;
    }
    if (nt > nc && nt > np) g_delim = '\t';
    else if (nc > np && nc > nt) g_delim = ',';
    else g_delim = '|';
}

static char *next_cell(char **pp) {
    char *p = *pp;
    if (!p || !*p) return NULL;
    char *start = p;
    int q = 0;
    while (*p) {
        if (*p == '"') q = !q;
        else if (*p == g_delim && !q) break;
        p++;
    }
    if (*p == g_delim) { *p = 0; *pp = p + 1; }
    else *pp = p;

    char *c = start;
    while (*c == ' ' || *c == '\t') c++;
    size_t L = strlen(c);
    while (L && (c[L - 1] == '\r' || c[L - 1] == '\n' || c[L - 1] == ' ')) c[--L] = 0;
    if (L >= 2 && c[0] == '"' && c[L - 1] == '"') { c[L - 1] = 0; c++; }
    return c;
}

static int split_cells(char *line, char **cells, int max) {
    int nc = 0;
    char *p = line, *c;
    while ((c = next_cell(&p)) != NULL && nc < max) cells[nc++] = c;
    return nc;
}

/* ---------- 筛选条件 ---------- */
enum { OP_CONTAINS, OP_EQ, OP_NE, OP_GT, OP_GE, OP_LT, OP_LE, OP_NOTCONTAINS };

typedef struct { int ci; int op; char val[96]; int used; } cond_t;

static int op_of(const char *s) {
    if (!strcmp(s, "contains") || !strcmp(s, "包含") || !strcmp(s, "含有")) return OP_CONTAINS;
    if (!strcmp(s, "not_contains") || !strcmp(s, "不包含")) return OP_NOTCONTAINS;
    if (!strcmp(s, "eq") || !strcmp(s, "=") || !strcmp(s, "==") || !strcmp(s, "等于")) return OP_EQ;
    if (!strcmp(s, "ne") || !strcmp(s, "!=") || !strcmp(s, "<>") || !strcmp(s, "不等于")) return OP_NE;
    if (!strcmp(s, "gt") || !strcmp(s, ">") || !strcmp(s, "大于")) return OP_GT;
    if (!strcmp(s, "ge") || !strcmp(s, ">=") || !strcmp(s, "大于等于")) return OP_GE;
    if (!strcmp(s, "lt") || !strcmp(s, "<") || !strcmp(s, "小于")) return OP_LT;
    if (!strcmp(s, "le") || !strcmp(s, "<=") || !strcmp(s, "小于等于")) return OP_LE;
    return -1;
}

static int cond_hit(const cond_t *c, const char *cell) {
    if (c->op == OP_CONTAINS)    return strstr(cell, c->val) != NULL;
    if (c->op == OP_NOTCONTAINS) return strstr(cell, c->val) == NULL;
    if (c->op == OP_EQ || c->op == OP_NE) {
        int eq = !strcmp(cell, c->val);
        /* 数字也按数值比一次，避免 "8" 和 "8.0"/" 8" 对不上 */
        if (!eq) {
            char *e1 = NULL, *e2 = NULL;
            double a = strtod(cell, &e1), b = strtod(c->val, &e2);
            if (e1 != cell && e2 != c->val && a == b) eq = 1;
        }
        return c->op == OP_EQ ? eq : !eq;
    }
    double a = num_of(cell), b = strtod(c->val, NULL);
    if (c->op == OP_GT) return a > b;
    if (c->op == OP_GE) return a >= b;
    if (c->op == OP_LT) return a < b;
    if (c->op == OP_LE) return a <= b;
    return 1;
}

static const char *op_text(int op) {
    switch (op) {
    case OP_CONTAINS: return "包含";
    case OP_NOTCONTAINS: return "不包含";
    case OP_EQ: return "=";
    case OP_NE: return "≠";
    case OP_GT: return ">";
    case OP_GE: return "≥";
    case OP_LT: return "<";
    default: return "≤";
    }
}

/* ---------- 数据源：按列名找 ---------- */
static int header_has(const char *path, const char *col);

typedef struct { char path[900]; time_t mt; } cand_t;
static int cand_cmp(const void *a, const void *b) {
    const cand_t *x = a, *y = b;
    if (x->mt < y->mt) return 1;
    if (x->mt > y->mt) return -1;
    return 0;
}

static int list_cands(cand_t *cands, int max) {
    static const char *dirs[] = {"/tmp/llama_agent/downloads",
                                 "/tmp/llama_agent/uploads",
                                 "/tmp/llama_agent/gen", NULL};
    int nc = 0;
    for (int d = 0; dirs[d] && nc < max; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && nc < max) {
            if (e->d_name[0] == '.') continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            if (strcasecmp(dot, ".csv") && strcasecmp(dot, ".txt")
                && strcasecmp(dot, ".tsv")) continue;
            snprintf(cands[nc].path, sizeof(cands[nc].path), "%s/%s", dirs[d], e->d_name);
            struct stat st;
            cands[nc].mt = (stat(cands[nc].path, &st) == 0) ? st.st_mtime : 0;
            nc++;
        }
        closedir(dp);
    }
    qsort(cands, (size_t)nc, sizeof(cand_t), cand_cmp);
    return nc;
}

static int header_has(const char *path, const char *col) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[LINE];
    int hit = 0;
    if (fgets(line, sizeof(line), f)) {
        detect_delim(line);
        char *p = line, *c;
        while ((c = next_cell(&p)) != NULL)
            if (strstr(c, col)) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

static int pick_source(const char *want_path, const char *col, char *out, size_t cap,
                       char *tried, size_t tcap) {
    if (want_path && want_path[0] && col[0] && header_has(want_path, col)) {
        snprintf(out, cap, "%s", want_path);
        return 1;
    }
    cand_t cands[256];
    int nc = list_cands(cands, 250);
    if (col[0]) {
        for (int i = 0; i < nc; i++)
            if (header_has(cands[i].path, col)) { snprintf(out, cap, "%s", cands[i].path); return 1; }
    } else if (nc > 0) {
        /* 没指定列（纯筛选场景）：用最新那份 */
        snprintf(out, cap, "%s", cands[0].path);
        return 1;
    }
    if (tried && tcap) {
        tried[0] = 0;
        size_t u = 0;
        for (int i = 0; i < nc && i < 3; i++) {
            FILE *f = fopen(cands[i].path, "r");
            if (!f) continue;
            char line[LINE];
            if (fgets(line, sizeof(line), f)) {
                char *nl = strpbrk(line, "\r\n");
                if (nl) *nl = 0;
                const char *base = strrchr(cands[i].path, '/');
                u += (size_t)snprintf(tried + u, tcap - u, "%s%s 的列：%s",
                                      u ? "；" : "", base ? base + 1 : cands[i].path, line);
            }
            fclose(f);
            if (u > tcap - 400) break;
        }
    }
    return 0;
}

/* 从任务文字里认出列名：只认表头里真实存在的名字，不猜同义词。
 * 优先级：① "按X" 里的 X；② topN 前面紧挨着的列（"播放量TOP10"）；
 *         ③ 任务里出现的第一个列名。 */
static void col_from_task(const char *task, char *col, size_t cap) {
    cand_t cands[256];
    int nc = list_cands(cands, 250);

    const char *tp = NULL;
    for (const char *q = task; *q; q++)
        if (!strncasecmp(q, "top", 3) || !strncmp(q, "前", 3) ||
            !strncmp(q, "倒数", 6) || !strncmp(q, "最低", 6) ||
            !strncmp(q, "最高", 6) || !strncmp(q, "最少", 6)) { tp = q; break; }

    const char *sort_word = strstr(task, "按");
    if (sort_word) sort_word += strlen("按");

    for (int i = 0; i < nc; i++) {
        FILE *f = fopen(cands[i].path, "r");
        if (!f) continue;
        char line[LINE], raw[LINE], tmp[LINE];
        if (!fgets(line, sizeof(line), f)) { fclose(f); continue; }
        detect_delim(line);
        snprintf(raw, sizeof(raw), "%s", line);

        char *pp, *c;

        /* ① "按X" */
        snprintf(tmp, sizeof(tmp), "%s", raw);
        pp = tmp;
        while ((c = next_cell(&pp)) != NULL) {
            if (strlen(c) < 2 || strlen(c) >= 32) continue;
            if (sort_word && strstr(sort_word, c)) {
                snprintf(col, cap, "%s", c); fclose(f); return;
            }
        }

        /* ② topN 前面紧挨着的列 */
        if (tp) {
            const char *best = NULL;
            int bestgap = 99;
            snprintf(tmp, sizeof(tmp), "%s", raw);
            pp = tmp;
            while ((c = next_cell(&pp)) != NULL) {
                size_t L = strlen(c);
                if (L < 2 || L >= 32) continue;
                const char *last = NULL;
                for (const char *r = task; r + L <= tp; r++)
                    if (!strncmp(r, c, L)) last = r;
                if (!last) continue;
                int gap = (int)(tp - (last + L));
                if (gap >= 0 && gap < bestgap) { bestgap = gap; best = c; }
            }
            if (best) { snprintf(col, cap, "%s", best); fclose(f); return; }
        }

        /* ③ 任务里出现的第一个列名 */
        snprintf(tmp, sizeof(tmp), "%s", raw);
        pp = tmp;
        while ((c = next_cell(&pp)) != NULL) {
            size_t L = strlen(c);
            if (L < 2 || L >= 32) continue;
            if (strstr(task, c)) { snprintf(col, cap, "%s", c); fclose(f); return; }
        }
        fclose(f);
    }
}

/* 任务里的筛选条件："评分大于8、类型包含科技、作者是这样" → cond 数组
 * 只认明确的比较词；没有比较词的短语不当条件（避免把 "播放量 top10" 的
 * "top10" 当成筛选值）。 */
static int conds_from_task(const char *task, char hdr[][128], int ncol, cond_t *cs) {
    int n = 0;
    static const struct { const char *w; int op; } OPS[] = {
        {"大于等于", OP_GE}, {">=", OP_GE},
        {"小于等于", OP_LE}, {"<=", OP_LE},
        {"不等于", OP_NE},   {"!=", OP_NE},
        {"包含", OP_CONTAINS}, {"含有", OP_CONTAINS}, {"不包含", OP_NOTCONTAINS},
        {"大于", OP_GT},     {">", OP_GT},
        {"小于", OP_LT},     {"<", OP_LT},
        {"等于", OP_EQ},     {"=", OP_EQ},
        {NULL, 0}
    };
    for (int i = 0; i < ncol && n < MAX_CONDS; i++) {
        const char *p = strstr(task, hdr[i]);
        if (!p) continue;
        p += strlen(hdr[i]);
        if (!strncmp(p, "一列", 6)) p += 6;
        else if (!strncmp(p, "列", 3)) p += 3;
        for (;;) {
            if (*p == ' ') { p++; continue; }
            if (!strncmp(p, "的", 3)) { p += 3; continue; }
            break;
        }
        int op = -1, wl = 0;
        for (int k = 0; OPS[k].w; k++) {
            size_t L = strlen(OPS[k].w);
            if (!strncmp(p, OPS[k].w, L)) { op = OPS[k].op; wl = (int)L; break; }
        }
        if (op < 0) continue;
        p += wl;
        for (;;) {
            if (*p == ' ' || *p == ':') { p++; continue; }
            if (!strncmp(p, "：", 3)) { p += 3; continue; }
            break;
        }
        /* 值的结尾：空白、顿号、逗号、分号、"且/并/同时/要求/的" */
        const char *e = p;
        while (*e) {
            if (*e == ' ' || *e == '\t' || *e == ',' || *e == ';')
                break;
            /* 全角分隔符是多字节，不能用字符常量比（那样恒不相等，
             * 值会把整句话都吞进去） */
            if (!strncmp(e, "，", 3) || !strncmp(e, "、", 3) || !strncmp(e, "；", 3) ||
                !strncmp(e, "。", 3) || !strncmp(e, "的", 3))
                break;
            if (!strncmp(e, "并且", 6) || !strncmp(e, "同时", 6) || !strncmp(e, "而且", 6) ||
                !strncmp(e, "且", 3) || !strncmp(e, "并", 3) || !strncmp(e, "要求", 6) ||
                !strncmp(e, "的", 3) || !strncmp(e, "行", 3) || !strncmp(e, "记录", 6))
                break;
            e++;
        }
        size_t L = (size_t)(e - p);
        if (L == 0 || L >= sizeof(cs[n].val)) continue;
        cs[n].ci = i;
        cs[n].op = op;
        cs[n].used = 1;
        memcpy(cs[n].val, p, L);
        cs[n].val[L] = 0;
        n++;
    }
    return n;
}

/* "题干重复的 top10" / "出现次数最多的作者"：这是**按值统计次数**，
 * 不是排序 —— 得 GROUP BY 后按 COUNT 降序，输出的列是「值 + 次数」。
 * 以前按普通排序处理，结果按字符串列排了一遍（key 全是 0），
 * 表头还变成 序号|题干，完全不是用户要的东西。 */
static int wants_group_count(const char *task) {
    if (!task || !task[0]) return 0;
    int dup = strstr(task, "重复") || strstr(task, "出现次数") || strstr(task, "次数最多")
           || strstr(task, "统计") || strstr(task, "每个") || strstr(task, "各");
    int top = strstr(task, "top") || strstr(task, "TOP") || strstr(task, "Top")
           || strstr(task, "bottom") || strstr(task, "BOTTOM")
           || strstr(task, "前") || strstr(task, "最多") || strstr(task, "最少")
           || strstr(task, "最低") || strstr(task, "倒数")
           || strstr(task, "排名") || strstr(task, "排行") || strstr(task, "榜");
    return (dup && top) ? 1 : 0;
}

typedef struct { char v[320]; int n; int seq; char e[3][560]; } g2_t;   /* e[] = 顺带展示的列 */
static int g2_cmp(const void *a, const void *b) {      /* 次数多的在前，同次数按出现顺序 */
    const g2_t *x = a, *y = b;
    if (x->n != y->n) return (x->n < y->n) ? 1 : -1;
    return (x->seq < y->seq) ? -1 : (x->seq > y->seq);
}
static int g2_cmp_asc(const void *a, const void *b) {  /* 次数少的在前，同次数按出现顺序 */
    const g2_t *x = a, *y = b;
    if (x->n != y->n) return (x->n > y->n) ? 1 : -1;
    return (x->seq < y->seq) ? -1 : (x->seq > y->seq);
}

/* 用户嘴里的列名和表头常常差一个字（"题目" vs "题干"、"up主" vs "作者"）。
 * 只认下面这张**明确写死的别名表**，不做"猜同义词" —— 表头里没有对应真实列
 * 就老实报错，说清有哪些列可选。 */
static const struct { const char *alias, *real; } COL_ALIAS[] = {
    {"题目", "题干"}, {"题面", "题干"}, {"问题", "题干"}, {"试题", "题干"},
    {"题干内容", "题干"}, {"标题", "视频名称"}, {"视频标题", "视频名称"},
    {"up主", "作者"}, {"UP主", "作者"}, {"作者名", "作者"}, {"上传者", "作者"},
    {"播放", "播放量"}, {"观看", "播放量"}, {"浏览量", "播放量"}, {"播放次数", "播放量"},
    {"点赞", "点赞数"}, {"喜欢", "点赞数"}, {"时长", "播放时长"},
    {"次数", "重复次数"}, {"序号", "序号"}, {NULL, NULL}
};

static int find_col(char hdr[][128], int ncol, const char *name);

static int find_col_fuzzy(char hdr[][128], int ncol, const char *name) {
    int i = find_col(hdr, ncol, name);
    if (i >= 0) return i;
    for (int k = 0; COL_ALIAS[k].alias; k++) {
        if (strcmp(COL_ALIAS[k].alias, name) != 0) continue;
        i = find_col(hdr, ncol, COL_ALIAS[k].real);
        if (i >= 0) return i;
    }
    return -1;
}

static int find_col(char hdr[][128], int ncol, const char *name) {
    for (int i = 0; i < ncol; i++)
        if (!strcmp(hdr[i], name) || strstr(hdr[i], name)) return i;
    return -1;
}

/* ---------- 主流程 ---------- */
static char *run(const char *json_in) {
    char path[512] = {0}, col[128] = {0}, task[1024] = {0}, order[16] = {0};
    jget(json_in, "path", path, sizeof(path));
    jget(json_in, "sort_by", col, sizeof(col));
    if (!col[0]) jget(json_in, "column", col, sizeof(col));
    jget(json_in, "_task", task, sizeof(task));
    jget(json_in, "order", order, sizeof(order));
    if (!col[0] && task[0]) col_from_task(task, col, sizeof(col));
    /* 任务里说的是别名（"题目列"）而表头是"题干"：查别名表直接换成真实列名，
     * 不用等打开文件 —— 否则会被判成"没认出列"退回模型（一等就是几分钟）。 */
    if (!col[0] && task[0]) {
        for (int k = 0; COL_ALIAS[k].alias; k++) {
            if (strlen(COL_ALIAS[k].alias) < 2) continue;
            if (!strstr(task, COL_ALIAS[k].alias)) continue;
            snprintf(col, sizeof(col), "%s", COL_ALIAS[k].real);
            break;
        }
    }

    int need_col = jget_num(json_in, "need_col", 0);
    if (!col[0] && need_col)
        return strdup("{\"error\":\"没认出要按哪一列排（任务里要提到表里的列名，比如「播放量」）\"}");

    int limit = jget_num(json_in, "limit", 10);
    int desc = 1;
    if (order[0]) desc = !strcasecmp(order, "desc");
    else {
        const char *pd = strstr(json_in, "\"desc\"");
        if (pd && strstr(pd, "false")) desc = 0;
    }
    if (!desc) desc = 0;

    char src[900] = {0}, tried[900] = {0};
    if (!pick_source(path, col, src, sizeof(src), tried, sizeof(tried))) {
        size_t n = strlen(tried) + 320;
        char *o = malloc(n);
        if (!o) return NULL;
        if (tried[0])
            snprintf(o, n, "{\"error\":\"没有哪份表的表头里有「%s」。%s\"}", col, tried);
        else
            snprintf(o, n, "{\"error\":\"没有可用的表格数据（downloads/uploads 里没有 csv/txt）\"}");
        return o;
    }

    FILE *f = fopen(src, "r");
    if (!f) {
        char *o = malloc(400);
        if (!o) return NULL;
        snprintf(o, 400, "{\"error\":\"打不开文件 %s\"}", src);
        return o;
    }

    /* 行用 getline 读：50 列的宽表一行就可能上万字节，固定 buf 会截断 */
    char *line = NULL;
    size_t cap_line = 0;
    ssize_t got = getline(&line, &cap_line, f);
    if (got <= 0) { free(line); fclose(f); return strdup("{\"error\":\"空文件\"}"); }
    detect_delim(line);

    char hdr[MAX_COLS][128];
    int ncol = 0;
    {
        char *save = strdup(line), *p = save, *c;
        while ((c = next_cell(&p)) != NULL && ncol < MAX_COLS) {
            snprintf(hdr[ncol], sizeof(hdr[ncol]), "%s", c);
            ncol++;
        }
        free(save);
    }
    if (ncol == 0) { free(line); fclose(f); return strdup("{\"error\":\"没有表头\"}"); }

    int vcol = col[0] ? find_col_fuzzy(hdr, ncol, col) : -1;
    if (vcol < 0 && col[0]) {
        char cols[900] = {0};
        size_t u = 0;
        for (int i = 0; i < ncol && u + 70 < sizeof(cols); i++)
            u += (size_t)snprintf(cols + u, sizeof(cols) - u, "%s%s", i ? "、" : "", hdr[i]);
        free(line); fclose(f);
        size_t n = strlen(cols) + strlen(col) + 320;
        char *o = malloc(n);
        if (!o) return NULL;
        snprintf(o, n, "{\"error\":\"%s 里没有「%s」列，可选列：%s\"}", src, col, cols);
        return o;
    }

    /* ---------- 筛选条件（where 数组 / 紧凑串 / 任务文字） ---------- */
    cond_t cs[MAX_CONDS];
    memset(cs, 0, sizeof(cs));
    int ncond = 0;
    {
        const char *w = strstr(json_in, "\"where\"");
        if (w) {
            const char *arr = strchr(w, '[');
            if (arr) {
                const char *p2 = arr;
                for (;;) {
                    const char *c0 = strstr(p2, "\"col\"");
                    if (!c0) break;
                    const char *rb = strchr(p2, ']');
                    if (rb && c0 > rb) break;
                    char cn[128] = {0}, op[32] = {0}, val[96] = {0};
                    const char *q = c0;
                    int fi;
                    for (fi = 0; fi < 3; fi++) {
                        const char *k = (fi == 0) ? "\"col\"" : (fi == 1 ? "\"op\"" : "\"val\"");
                        q = strstr(q, k);
                        if (!q) break;
                        q = strchr(q, ':');
                        if (!q) break;
                        q = strchr(q, '"');
                        if (!q) break;
                        q++;
                        size_t i = 0;
                        char *dst = (fi == 0) ? cn : (fi == 1 ? op : val);
                        size_t dcap = (fi == 0) ? sizeof(cn) : (fi == 1 ? sizeof(op) : sizeof(val));
                        while (*q && *q != '"' && i + 1 < dcap) {
                            if (*q == '\\' && q[1]) q++;
                            dst[i++] = *q++;
                        }
                        dst[i] = 0;
                    }
                    if (fi < 3) break;
                    int ci = find_col(hdr, ncol, cn);
                    int o = op_of(op);
                    if (ci >= 0 && o >= 0 && ncond < MAX_CONDS) {
                        cs[ncond].ci = ci; cs[ncond].op = o; cs[ncond].used = 1;
                        snprintf(cs[ncond].val, sizeof(cs[ncond].val), "%s", val);
                        ncond++;
                    }
                    p2 = q ? q : c0 + 5;
                }
            } else {
                const char *q = strchr(w, ':');
                q = q ? strchr(q, '"') : NULL;
                if (q) {
                    q++;
                    while (*q && *q != '"' && ncond < MAX_CONDS) {
                        char item[300];
                        size_t i = 0;
                        while (*q && *q != '"' && *q != ';' && i + 1 < sizeof(item)) item[i++] = *q++;
                        item[i] = 0;
                        if (*q == ';') q++;
                        char *c1 = strchr(item, '~');
                        if (c1) {
                            *c1 = 0;
                            char *c2 = strchr(c1 + 1, '~');
                            if (c2) {
                                *c2 = 0;
                                int ci = find_col(hdr, ncol, item);
                                int o = op_of(c1 + 1);
                                if (ci >= 0 && o >= 0) {
                                    cs[ncond].ci = ci; cs[ncond].op = o; cs[ncond].used = 1;
                                    snprintf(cs[ncond].val, sizeof(cs[ncond].val), "%s", c2 + 1);
                                    ncond++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (ncond == 0 && task[0]) ncond = conds_from_task(task, hdr, ncol, cs);

    /* ---------- 分组计数："同一个作者超过1次" / "作者出现超过1次" ---------- */
    int gci = -1, having = 0;
    if (task[0]) {
        const char *g = strstr(task, "同一个");
        size_t gl = strlen("同一个");
        if (!g) { g = strstr(task, "同一"); gl = strlen("同一"); }
        if (g) {
            const char *gname = g + gl;
            while (!strncmp(gname, "的", 3)) gname += 3;
            {   /* 先精确匹配（任务里直接点了表头名），再走别名表 */
                size_t gl2 = 0;
                for (const char *q = gname; *q && strncmp(q, "超过", 6) && strncmp(q, "出现", 6)
                     && strncmp(q, "重复", 6) && strncmp(q, "的", 3) && *q != ' '; q += (*q < 0x80 ? 1 : 3))
                    gl2 += (*q < 0x80 ? 1 : 3);
                char gcol2[128] = {0};
                if (gl2 && gl2 < sizeof(gcol2)) {
                    memcpy(gcol2, gname, gl2);
                    gcol2[gl2] = 0;
                    gci = find_col_fuzzy(hdr, ncol, gcol2);
                }
            }
        }
        if (gci < 0) {
            const char *rep = strstr(task, "出现");
            if (!rep) rep = strstr(task, "重复");
            if (rep) {
                const char *best = NULL;
                for (int i = 0; i < ncol; i++) {
                    size_t L = strlen(hdr[i]);
                    if (L < 2) continue;
                    for (const char *q = task; q + L <= rep; q++)
                        if (!strncmp(q, hdr[i], L)) { if (!best || q > best) best = q; }
                }
                if (best)
                    for (int i = 0; i < ncol; i++) {
                        size_t L = strlen(hdr[i]);
                        if (L >= 2 && !strncmp(best, hdr[i], L)) { gci = i; break; }
                    }
            }
        }
        /* 别名兜底只在"确实要做分组统计/计数筛选"时才用。
         * 否则 "观看 top2"（观看→播放量）会被误当成分组列，变成"按播放量
         * 计数"这种莫名其妙的结果。 */
        int groupish = wants_group_count(task) || strstr(task, "同一个")
                    || strstr(task, "出现") || strstr(task, "重复")
                    || strstr(task, "超过") || strstr(task, "多于");
        if (gci < 0 && task[0] && groupish) {
            for (int k = 0; COL_ALIAS[k].alias; k++) {
                if (strlen(COL_ALIAS[k].alias) < 2) continue;
                if (!strstr(task, COL_ALIAS[k].alias)) continue;
                int i = find_col(hdr, ncol, COL_ALIAS[k].real);
                if (i >= 0) { gci = i; break; }
            }
        }
        const char *h = strstr(task, "超过");
        if (!h) h = strstr(task, "多于");
        if (h) {
            int v = atoi(h + strlen("超过"));
            if (v >= 1 && v <= 100) having = (v >= 2) ? v : 2;
        }
        if (gci >= 0 && !having) having = 2;
    }

    /* ---------- 聚合模式：按某列统计出现次数 ---------- */
    if (wants_group_count(task) && gci >= 0) {
        /* 任务里还点名了别的列（"同时显示对应的 选项列"）就跟一起显示 */
        int extra[3], nextra = 0;
        for (int i = 0; i < ncol && nextra < 3; i++) {
            if (i == gci) continue;
            size_t L = strlen(hdr[i]);
            if (L < 2) continue;
            if (strstr(task, hdr[i])) extra[nextra++] = i;
        }

        int cap2 = 12000;   /* 每项约 2KB，2 万项就是 40MB，按实际规模收一收 */
        g2_t *g2 = calloc((size_t)cap2, sizeof(g2_t));
        int ng = 0, tot = 0;
        if (g2) {
            while (getline(&line, &cap_line, f) > 0) {
                char *cs2[MAX_COLS];
                int nc2 = split_cells(line, cs2, MAX_COLS);
                if (nc2 < 1) continue;
                tot++;
                if (gci >= nc2) continue;
                const char *v = cs2[gci];
                if (!v[0]) continue;
                int k;
                for (k = 0; k < ng; k++) if (!strcmp(g2[k].v, v)) break;
                if (k == ng) {
                    if (ng >= cap2) continue;
                    cut_utf8(g2[ng].v, 300, v);
                    g2[ng].n = 0;
                    g2[ng].seq = ng;
                    for (int j = 0; j < nextra; j++) {
                        char tmp[600] = {0};
                        if (extra[j] < nc2) cut_utf8(tmp, 520, cs2[extra[j]]);
                        snprintf(g2[ng].e[j], sizeof(g2[ng].e[j]), "%s", tmp);
                    }
                    ng++;
                }
                g2[k].n++;
            }
            free(line);
            fclose(f);

            qsort(g2, (size_t)ng, sizeof(g2_t), desc ? g2_cmp : g2_cmp_asc);
            /* top：只看重复的（n>=2）；bottom：重复本来就少，按文件行序把
             * 出现最少的（多半是只出现 1 次的）列出来 —— 用户要的就是这个，
             * 而不是"筛完 n>=2 之后空空如也"。 */
            int show_n = 0;
            for (int i = 0; i < ng && show_n < limit; i++)
                if (!desc || g2[i].n >= 2) show_n++;

            size_t cap3 = 4096 + (size_t)(limit + 4) * 900;
            char *out = malloc(cap3);
            if (!out) { free(g2); return NULL; }
            size_t off = 0;
            char gj[300] = {0};
            jesc_t(gj, sizeof(gj), hdr[gci]);
            off += (size_t)snprintf(out + off, cap3 - off,
                "{\"value\":\"**「%s」重复次数%s的 %d 个**（共 %d 行、%d 个不同值）\\n\\n"
                "| %s | 重复次数 |", gj, desc ? "最多" : "最少", show_n, tot, ng, gj);
            for (int j = 0; j < nextra; j++) {
                char hc[280];
                md_cell(hc, sizeof(hc), hdr[extra[j]]);
                off += (size_t)snprintf(out + off, cap3 - off, " %s |", hc);
            }
            off += (size_t)snprintf(out + off, cap3 - off, "\\n|---|");
            for (int j = 0; j < nextra; j++)
                off += (size_t)snprintf(out + off, cap3 - off, "---|");
            off += (size_t)snprintf(out + off, cap3 - off, "\\n");
            int shown = 0;
            for (int i = 0; i < ng && shown < show_n; i++) {
                if (desc && g2[i].n < 2) continue;
                char cel[300];
                md_cell(cel, sizeof(cel), g2[i].v);
                off += (size_t)snprintf(out + off, cap3 - off, "| %s | %d |", cel, g2[i].n);
                for (int j = 0; j < nextra; j++) {
                    char ec[300];
                    md_cell(ec, sizeof(ec), g2[i].e[j]);
                    off += (size_t)snprintf(out + off, cap3 - off, " %s |", ec);
                }
                off += (size_t)snprintf(out + off, cap3 - off, "\\n");
                shown++;
            }
            char ans[300];
            snprintf(ans, sizeof(ans), "%s 重复%s的是「%s」，出现 %d 次（共 %d 行 / %d 个不同值）",
                     hdr[gci], desc ? "最多" : "最少", show_n > 0 ? g2[0].v : "-",
                     show_n > 0 ? g2[0].n : 0, tot, ng);
            char ansj[400] = {0};
            jesc_t(ansj, sizeof(ansj), ans);
            off += (size_t)snprintf(out + off, cap3 - off,
                "\",\"answer\":\"%s\",\"scanned\":%d,\"distinct\":%d,\"conds\":0}", ansj, tot, ng);
            free(g2);
            return out;
        }
    }

    /* ---------- 输出列：第0列 + 排序列 + 筛选列 + 分组列（最多 5 列） ---------- */
    int show[MAX_COLS], nshow = 0;
    show[nshow++] = 0;
    if (vcol > 0 && nshow < 5) show[nshow++] = vcol;
    if (vcol < 0 && ncond == 0)
        for (int i = 1; i < ncol && nshow < 4; i++) show[nshow++] = i;
    for (int k = 0; k < ncond && nshow < 5; k++) {
        int dup = 0;
        for (int j = 0; j < nshow; j++) if (show[j] == cs[k].ci) dup = 1;
        if (!dup && cs[k].ci < ncol) show[nshow++] = cs[k].ci;
    }
    int gslot = -1;
    if (gci > 0) {
        for (int j = 0; j < nshow; j++) if (show[j] == gci) gslot = j;
        if (gslot < 0 && nshow < 5) { show[nshow] = gci; gslot = nshow; nshow++; }
    }

    /* ---------- 读行 ---------- */
    row_t *rows = calloc(MAX_ROWS, sizeof(row_t));
    if (!rows) { free(line); fclose(f); return NULL; }
    int nr = 0, scanned = 0;
    while (nr < MAX_ROWS && getline(&line, &cap_line, f) > 0) {
        char *cells[MAX_COLS];
        int nc = split_cells(line, cells, MAX_COLS);
        if (nc < 1) continue;
        scanned++;
        int ok = 1;
        for (int k = 0; k < ncond; k++) {
            if (cs[k].ci >= nc) { ok = 0; break; }
            if (!cond_hit(&cs[k], cells[cs[k].ci])) { ok = 0; break; }
        }
        if (!ok) continue;
        rows[nr].key = (vcol >= 0 && vcol < nc) ? num_of(cells[vcol]) : 0;
        for (int j = 0; j < nshow; j++) {
            char tmp[200] = {0};
            if (show[j] < nc) cut_utf8(tmp, 110, cells[show[j]]);
            snprintf(rows[nr].c[j], sizeof(rows[nr].c[j]), "%s", tmp);
        }
        nr++;
    }
    free(line);
    fclose(f);

    if (nr == 0 && scanned <= 1) {
        free(rows);
        return strdup("{\"error\":\"这个文件只有表头、没有数据行（可能是旧的解析残留，重新上传一次文件）\"}");
    }

    /* 分组计数过滤 */
    if (gci >= 0 && having >= 2 && gslot >= 0 && nr > 0) {
        typedef struct { char v[128]; int n; } gc_t;
        gc_t *gc = calloc(4096, sizeof(gc_t));
        if (gc) {
            int ng = 0;
            for (int i = 0; i < nr; i++) {
                const char *v = rows[i].c[gslot];
                int k;
                for (k = 0; k < ng; k++) if (!strcmp(gc[k].v, v)) break;
                if (k == ng && ng < 4096) {
                    snprintf(gc[ng].v, sizeof(gc[ng].v), "%s", v);
                    gc[ng].n = 0;
                    ng++;
                }
                if (k < ng) gc[k].n++;
            }
            int keep = 0;
            for (int i = 0; i < nr; i++) {
                const char *v = rows[i].c[gslot];
                int cnt = 0;
                for (int k = 0; k < ng; k++) if (!strcmp(gc[k].v, v)) { cnt = gc[k].n; break; }
                if (cnt >= having) rows[keep++] = rows[i];
            }
            nr = keep;
            free(gc);
        }
    }

    /* 排序列要是整列都解析不出数字（纯文本列），排出来的"榜"是文件原顺序，
     * 那是假榜单 —— 直接说清楚，让用户换数字列或用聚合。 */
    if (vcol >= 0 && nr > 1) {
        /* 用比例判断：题干这类文本列里常有"2019年…"开头的，num_of 能抠出
         * 一个数字，但大多数行抠不出来 —— 少数能抠出来就当成数字列排序，
         * 出来的"榜"完全是乱的。要求 ≥60% 的行能解析出数字才算数字列。 */
        int numeric = 0;
        for (int i = 0; i < nr; i++) if (rows[i].key != 0) numeric++;
        if (numeric * 100 < nr * 60) {
            char msg[400];
            snprintf(msg, sizeof(msg),
                     "{\"error\":\"「%s」不是数字列，没法按大小排 top/bottom；"
                     "要统计它重复多少次，请说「%s 重复 top10」\"}", hdr[vcol], hdr[vcol]);
            free(rows);
            return strdup(msg);
        }
        qsort(rows, (size_t)nr, sizeof(row_t), desc ? cmp_desc : cmp_asc);
    }
    if (limit <= 0) limit = 10;
    if (limit > nr) limit = nr;

    /* ---------- 输出 ---------- */
    size_t cap = 4096 + (size_t)(limit + 8) * (200 + nshow * 240);
    char *out = malloc(cap);
    if (!out) { free(rows); return NULL; }
    size_t off = 0;

    char condtxt[600] = {0};
    {
        size_t u = 0;
        for (int k = 0; k < ncond && u + 120 < sizeof(condtxt); k++)
            u += (size_t)snprintf(condtxt + u, sizeof(condtxt) - u, "%s%s %s %s",
                                  k ? "、" : "", hdr[cs[k].ci], op_text(cs[k].op), cs[k].val);
    }
    char condj[760] = {0};
    jesc_t(condj, sizeof(condj), condtxt);

    if (ncond == 0 && vcol < 0)
        off += (size_t)snprintf(out + off, cap - off,
            "{\"value\":\"表格内容（共 %d 行 × %d 列，显示前 %d 行）", scanned, ncol, limit);
    else if (ncond == 0)
        off += (size_t)snprintf(out + off, cap - off,
            "{\"value\":\"按列排序，取 %d 条", limit);
    else
        off += (size_t)snprintf(out + off, cap - off,
            "{\"value\":\"满足条件（%s）的 %d 条", condj, limit);

    if (vcol >= 0) {
        char vj[300] = {0};
        jesc_t(vj, sizeof(vj), hdr[vcol]);
        off += (size_t)snprintf(out + off, cap - off, "，按「%s」%s：\\n\\n",
                                vj, desc ? "降序" : "升序");
    } else {
        off += (size_t)snprintf(out + off, cap - off, "：\\n\\n");
    }

    off += (size_t)snprintf(out + off, cap - off, "|");
    for (int j = 0; j < nshow; j++) {
        char hc[260];
        md_cell(hc, sizeof(hc), hdr[show[j]]);
        off += (size_t)snprintf(out + off, cap - off, " %s |", hc);
    }
    off += (size_t)snprintf(out + off, cap - off, "\\n|");
    for (int j = 0; j < nshow; j++)
        off += (size_t)snprintf(out + off, cap - off, "---|");
    off += (size_t)snprintf(out + off, cap - off, "\\n");

    for (int i = 0; i < limit && off < cap - 600; i++) {
        off += (size_t)snprintf(out + off, cap - off, "|");
        for (int j = 0; j < nshow; j++) {
            char cel2[280];
            md_cell(cel2, sizeof(cel2), rows[i].c[j]);
            off += (size_t)snprintf(out + off, cap - off, " %s |", cel2);
        }
        off += (size_t)snprintf(out + off, cap - off, "\\n");
    }

    {
        char ans[400];
        if (vcol >= 0 && ncond)
            snprintf(ans, sizeof(ans), "筛出 %d 条，按「%s」%s %d 条（共 %d 行）",
                     nr, hdr[vcol], desc ? "最高" : "最低", limit, scanned);
        else if (vcol >= 0 && gci >= 0)
            snprintf(ans, sizeof(ans), "「%s」%s %d 条（只保留 %s 出现≥%d 次的，共 %d 行）",
                     hdr[vcol], desc ? "最高" : "最低", limit, hdr[gci], having, scanned);
        else if (vcol >= 0)
            snprintf(ans, sizeof(ans), "「%s」%s %d 条（共 %d 行）",
                     hdr[vcol], desc ? "最高" : "最低", limit, scanned);
        else if (vcol < 0 && ncond == 0)
            snprintf(ans, sizeof(ans), "表格内容 %d 行 × %d 列（显示前 %d 行）",
                     scanned, ncol, limit);
        else
            snprintf(ans, sizeof(ans), "%s %d 条（共 %d 行）",
                     ncond ? condtxt : "筛出", limit, scanned);
        char ansj[460] = {0};
        jesc_t(ansj, sizeof(ansj), ans);
        off += (size_t)snprintf(out + off, cap - off,
            "\",\"answer\":\"%s\",\"rows\":%d,\"scanned\":%d,\"conds\":%d}", ansj, limit, scanned, ncond);
    }
    free(rows);
    return out;
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "table",
    "表格：按某列取 TOP/BOTTOM，支持多条件筛选（同时满足 A 列包含 xx、B 列大于 100…）。"
    "数据源按列名自动找（上传的 xlsx/csv/tsv 都行）",
    "{\"sort_by\":\"播放量\",\"order\":\"desc\",\"limit\":10}  |  "
    "{\"sort_by\":\"播放量\",\"order\":\"asc\",\"limit\":10}  |  "
    "{\"where\":[{\"col\":\"作者\",\"op\":\"contains\",\"val\":\"小明\"},"
    "{\"col\":\"评分\",\"op\":\">=\",\"val\":\"8\"}],\"limit\":20}",
    run
};

plugin_desc_t *plugin_entry(void) { return &desc; }
