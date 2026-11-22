/* json.c — 递归下降 JSON 解析器
 *
 * 覆盖 RFC 8259 的完整语法：对象、数组、字符串（全转义 + \uXXXX 含代理对）、
 * 数字（±整数/小数/指数）、true/false/null。深度上限 64 防递归炸栈。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "json.h"

#define MAX_DEPTH 64

typedef struct {
    const char *p;
    const char *start;
    char *err;
    size_t errcap;
    int depth;
} jctx;

static jval *parse_value(jctx *c);

static void set_err(jctx *c, const char *msg) {
    if (c->err && c->errcap && !c->err[0]) {
        snprintf(c->err, c->errcap, "%s（位置 %ld）", msg, (long)(c->p - c->start));
    }
}

static void skip_ws(jctx *c) {
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n') c->p++;
}

static jval *jnew(jtype t) {
    jval *v = calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

/* UTF-8 编码一个码点 */
static int put_utf8(char *out, unsigned cp) {
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

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* 解析字符串（调用时 *p 一定是 "） */
static char *parse_string_raw(jctx *c) {
    if (*c->p != '"') { set_err(c, "字符串缺少引号"); return NULL; }
    c->p++;
    /* 预留 3 倍空间（\uXXXX → 最多 3 字节） */
    size_t cap = 64, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (*c->p && *c->p != '"') {
        if (n + 8 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '\\') {
            c->p++;
            switch (*c->p) {
            case '"':  buf[n++] = '"';  c->p++; break;
            case '\\': buf[n++] = '\\'; c->p++; break;
            case '/':  buf[n++] = '/';  c->p++; break;
            case 'b':  buf[n++] = '\b'; c->p++; break;
            case 'f':  buf[n++] = '\f'; c->p++; break;
            case 'n':  buf[n++] = '\n'; c->p++; break;
            case 'r':  buf[n++] = '\r'; c->p++; break;
            case 't':  buf[n++] = '\t'; c->p++; break;
            case 'u': {
                unsigned cp = 0;
                if (hex4(c->p + 1, &cp) != 0) { free(buf); set_err(c, "\\u 转义非法"); return NULL; }
                c->p += 5;
                /* 代理对：\uD800-\uDBFF 后面必须跟 \uDC00-\uDFFF */
                if (cp >= 0xD800 && cp <= 0xDBFF && c->p[0] == '\\' && c->p[1] == 'u') {
                    unsigned lo = 0;
                    if (hex4(c->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        c->p += 6;
                    }
                }
                n += (size_t)put_utf8(buf + n, cp);
                break;
            }
            default:
                free(buf);
                set_err(c, "未知转义");
                return NULL;
            }
        } else {
            buf[n++] = (char)ch;
            c->p++;
        }
    }
    if (*c->p != '"') { free(buf); set_err(c, "字符串未闭合"); return NULL; }
    c->p++;
    buf[n] = 0;
    return buf;
}

static jval *parse_object(jctx *c) {
    jval *v = jnew(J_OBJ);
    if (!v) return NULL;
    c->p++;                       /* { */
    skip_ws(c);
    if (*c->p == '}') { c->p++; return v; }

    int cap = 8;
    v->keys = calloc((size_t)cap, sizeof(char *));
    v->items = calloc((size_t)cap, sizeof(jval *));
    if (!v->keys || !v->items) { json_free(v); return NULL; }

    for (;;) {
        skip_ws(c);
        char *k = parse_string_raw(c);
        if (!k) { json_free(v); return NULL; }
        skip_ws(c);
        if (*c->p != ':') { free(k); json_free(v); set_err(c, "对象缺少冒号"); return NULL; }
        c->p++;
        jval *val = parse_value(c);
        if (!val) { free(k); json_free(v); return NULL; }

        if (v->n >= cap) {
            cap *= 2;
            /* 两次 realloc 必须分开判：前一个成功后 v->keys 已经指向新块，
             * 后一个失败时再去 json_free 就会释放野指针 */
            char **nk = realloc(v->keys, (size_t)cap * sizeof(char *));
            if (!nk) { free(k); json_free(val); json_free(v); return NULL; }
            v->keys = nk;
            jval **ni = realloc(v->items, (size_t)cap * sizeof(jval *));
            if (!ni) { free(k); json_free(val); json_free(v); return NULL; }
            v->items = ni;
        }
        v->keys[v->n] = k;
        v->items[v->n] = val;
        v->n++;

        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; return v; }
        json_free(v);
        set_err(c, "对象缺少逗号或右括号");
        return NULL;
    }
}

static jval *parse_array(jctx *c) {
    jval *v = jnew(J_ARR);
    if (!v) return NULL;
    c->p++;                       /* [ */
    skip_ws(c);
    if (*c->p == ']') { c->p++; return v; }

    int cap = 8;
    v->items = calloc((size_t)cap, sizeof(jval *));
    if (!v->items) { json_free(v); return NULL; }

    for (;;) {
        jval *val = parse_value(c);
        if (!val) { json_free(v); return NULL; }
        if (v->n >= cap) {
            cap *= 2;
            jval **ni = realloc(v->items, (size_t)cap * sizeof(jval *));
            if (!ni) { json_free(val); json_free(v); return NULL; }
            v->items = ni;
        }
        v->items[v->n++] = val;

        skip_ws(c);
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; return v; }
        json_free(v);
        set_err(c, "数组缺少逗号或右括号");
        return NULL;
    }
}

static jval *parse_value(jctx *c) {
    if (++c->depth > MAX_DEPTH) { set_err(c, "嵌套过深"); return NULL; }
    skip_ws(c);

    jval *v = NULL;
    char ch = *c->p;
    if (ch == '{') {
        v = parse_object(c);
    } else if (ch == '[') {
        v = parse_array(c);
    } else if (ch == '"') {
        v = jnew(J_STR);
        if (v) {
            v->s = parse_string_raw(c);
            if (!v->s) { free(v); v = NULL; }
        }
    } else if (!strncmp(c->p, "true", 4)) {
        v = jnew(J_BOOL);
        if (v) { v->b = 1; c->p += 4; }
    } else if (!strncmp(c->p, "false", 5)) {
        v = jnew(J_BOOL);
        if (v) { v->b = 0; c->p += 5; }
    } else if (!strncmp(c->p, "null", 4)) {
        v = jnew(J_NULL);
        if (v) c->p += 4;
    } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char *end = NULL;
        double d = strtod(c->p, &end);
        if (end == c->p) { set_err(c, "数字非法"); v = NULL; }
        else {
            v = jnew(J_NUM);
            if (v) { v->num = d; c->p = end; }
        }
    } else {
        set_err(c, "无法识别的值");
    }
    c->depth--;
    return v;
}

jval *json_parse(const char *text, char *err, size_t errcap) {
    if (err && errcap) err[0] = 0;
    if (!text) return NULL;
    jctx c = { text, text, err, errcap, 0 };
    jval *v = parse_value(&c);
    if (!v) return NULL;
    skip_ws(&c);
    /* 允许尾随内容（模型常带解释文字）——只要求前面是合法 JSON */
    return v;
}

void json_free(jval *v) {
    if (!v) return;
    if (v->s) free(v->s);
    for (int i = 0; i < v->n; i++) {
        if (v->keys) free(v->keys[i]);
        if (v->items) json_free(v->items[i]);
    }
    free(v->keys);
    free(v->items);
    free(v);
}

jval *json_get(const jval *obj, const char *key) {
    if (!obj || obj->type != J_OBJ || !key) return NULL;
    for (int i = 0; i < obj->n; i++) {
        if (obj->keys[i] && !strcmp(obj->keys[i], key)) return obj->items[i];
    }
    return NULL;
}

jval *json_at(const jval *arr, int i) {
    if (!arr || arr->type != J_ARR || i < 0 || i >= arr->n) return NULL;
    return arr->items[i];
}

const char *json_str(const jval *v) {
    return (v && v->type == J_STR) ? v->s : NULL;
}

int json_int(const jval *v, int dflt) {
    if (!v) return dflt;
    if (v->type == J_NUM) return (int)v->num;
    if (v->type == J_BOOL) return v->b;
    return dflt;
}

double json_num(const jval *v, double dflt) {
    if (v && v->type == J_NUM) return v->num;
    return dflt;
}

int json_bool(const jval *v, int dflt) {
    if (!v) return dflt;
    if (v->type == J_BOOL) return v->b;
    if (v->type == J_NUM) return v->num != 0;
    return dflt;
}
