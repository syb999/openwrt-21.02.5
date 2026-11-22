/* table.c — 表格插件
 *
 * 把 Python 版 table 工具的核心（按列排序 / 分组计数 / 分组求和）搬到 C。
 * 输入：{"path":"/tmp/x.txt","sort_by":"播放量","limit":10,"desc":true}
 * 数据格式沿用 uploads 转出来的管道分隔文本（第一行表头）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin.h"

#define MAX_ROWS 5000
#define MAX_COLS 32
#define LINE 2048

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
        if (*s == ',') continue;
        if (*s == '|') break;
        buf[n++] = *s;
    }
    buf[n] = 0;
    double mul = 1;
    /* 万/亿 是 UTF-8 三字节，不能用多字节字符常量比 */
    if (n >= 3 && strcmp(buf + n - 3, "万") == 0) { mul = 10000; buf[n - 3] = 0; }
    else if (n >= 3 && strcmp(buf + n - 3, "亿") == 0) { mul = 1e8; buf[n - 3] = 0; }
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) {
        /* 分:秒 */
        int mm = 0, ss = 0;
        if (sscanf(buf, "%d:%d", &mm, &ss) == 2) return mm * 60.0 + ss;
        return 0;
    }
    return v * mul;
}

typedef struct { double key; char name[200]; } row_t;

static int cmp_desc(const void *a, const void *b) {
    const row_t *x = a, *y = b;
    if (x->key < y->key) return 1;
    if (x->key > y->key) return -1;
    return 0;
}
static int cmp_asc(const void *a, const void *b) { return -cmp_desc(a, b); }

static char *run(const char *json_in) {
    char path[512] = {0}, col[64] = {0};
    jget(json_in, "path", path, sizeof(path));
    jget(json_in, "sort_by", col, sizeof(col));
    int limit = jget_num(json_in, "limit", 10);
    int desc = 1;
    {   /* desc 显式 false 时取 0 */
        const char *p = strstr(json_in, "\"desc\"");
        if (p && strstr(p, "false")) desc = 0;
    }
    if (!path[0]) return strdup("{\"error\":\"缺少 path\"}");

    FILE *f = fopen(path, "r");
    if (!f) {
        char *o = malloc(300);
        if (!o) return NULL;
        snprintf(o, 300, "{\"error\":\"打不开文件 %s\"}", path);
        return o;
    }

    char line[LINE];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return strdup("{\"error\":\"空文件\"}"); }

    /* 表头 */
    char *hdr[MAX_COLS];
    int ncol = 0;
    for (char *p = strtok(line, "|\n"); p && ncol < MAX_COLS; p = strtok(NULL, "|\n")) {
        char *e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        hdr[ncol++] = strdup(p);
    }
    if (ncol == 0) { fclose(f); return strdup("{\"error\":\"没有表头\"}"); }

    int vcol = -1;
    for (int i = 0; i < ncol; i++) {
        if (col[0] && strstr(hdr[i], col)) { vcol = i; break; }
    }
    if (vcol < 0) vcol = 0;

    row_t *rows = malloc(sizeof(row_t) * MAX_ROWS);
    if (!rows) { fclose(f); return NULL; }
    int nr = 0;
    while (nr < MAX_ROWS && fgets(line, sizeof(line), f)) {
        char *cells[MAX_COLS];
        int nc = 0;
        char *p = line;
        cells[nc++] = p;
        for (; *p && nc < MAX_COLS; p++) {
            if (*p == '|') { *p = 0; cells[nc++] = p + 1; }
        }
        if (nc <= vcol) continue;
        rows[nr].key = num_of(cells[vcol]);
        snprintf(rows[nr].name, sizeof(rows[nr].name), "%.60s", cells[0]);
        nr++;
    }
    fclose(f);

    qsort(rows, (size_t)nr, sizeof(row_t), desc ? cmp_desc : cmp_asc);
    if (limit > nr) limit = nr;
    if (limit < 1) limit = 1;

    /* 输出 markdown 表格（和 Python 版一致，页面能直接渲染） */
    size_t cap = 4096 + (size_t)limit * 300;
    char *out = malloc(cap);
    if (!out) { free(rows); return NULL; }
    size_t off = 0;
    off += (size_t)snprintf(out + off, cap - off,
        "{\"value\":\"按「%s」%s，%s %d 条：\\n\\n| %s | %s |\\n|---|---|\\n",
        hdr[vcol], desc ? "降序" : "升序", desc ? "最高" : "最低", limit,
        hdr[0], hdr[vcol]);
    for (int i = 0; i < limit && off < cap - 400; i++) {
        char nm[200];
        snprintf(nm, sizeof(nm), "%s", rows[i].name);
        for (char *q = nm; *q; q++) if (*q == '|') *q = 0xA6;   /* 半角竖线→全角 */
        if (rows[i].key == (long long)rows[i].key) {
            off += (size_t)snprintf(out + off, cap - off,
                "| %s | %.0f |\\n", nm, rows[i].key);
        } else {
            off += (size_t)snprintf(out + off, cap - off,
                "| %s | %g |\\n", nm, rows[i].key);
        }
    }
    off += (size_t)snprintf(out + off, cap - off, "\"}");
    free(rows);
    return out;
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "table",
    "表格：按某列排序取 TOP/BOTTOM（表头用 | 分隔的文本文件）",
    "{\"path\":\"/tmp/x.txt\",\"sort_by\":\"播放量\",\"limit\":10,\"desc\":true}",
    run
};

plugin_desc_t *plugin_entry(void) { return &desc; }
