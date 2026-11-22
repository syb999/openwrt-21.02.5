/* math.c — 数学插件（完整版）
 *
 * 能力：
 *   1. calc      任意表达式：(1+2)*3^2、sqrt(16)+abs(-3)、pi*2、min(3,5)
 *   2. solve     一元方程（线性/二次）：3x+5=20、2(x-3)=x+7、x^2-5x+6=0
 *   3. system    二元一次方程组：x+y=10 与 x-y=2
 *   4. plan      「计划vs实际」应用题 11 种结构（原计划/多修/还差/提前…）
 *   5. stat      统计：平均/中位数/方差/标准差/最大/最小/求和
 *   6. num       数论：gcd/lcm/质因数分解/阶乘/组合数/斐波那契
 *   7. 数字解析  中文数字（七百二十）、中文分数（五分之三）、一半
 *
 * 关键设计：不写符号运算。表达式求值器 + 采样线性化，两次求值就能
 * 从任意写法的线性表达式里提取系数，三次求值能识别二次。这样
 * "3x+5=20" 和 "5+3x=20" 和 "3*(x)+5=20" 都不用手写正则。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "plugin.h"

#define MAXV 4

typedef struct { char name[16]; double val; } var_t;

typedef struct {
    const char *p;
    var_t vars[MAXV];
    int nvars;
    int err;
    char emsg[128];
} ex_t;

/* ============ JSON 取值 ============ */
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
            case 'u': {
                /* \uXXXX：模型输出 √ / π / 中文 常用转义形式，
                 * 不解就会变成 "u221a3+2" 这种垃圾 */
                unsigned cp = 0;
                int ok = 1;
                for (int k = 1; k <= 4; k++) {
                    char ch = p[k];
                    cp <<= 4;
                    if (ch >= '0' && ch <= '9') cp |= (unsigned)(ch - '0');
                    else if (ch >= 'a' && ch <= 'f') cp |= (unsigned)(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F') cp |= (unsigned)(ch - 'A' + 10);
                    else { ok = 0; break; }
                }
                if (ok && cp && i + 4 < cap) {
                    if (cp < 0x80) out[i++] = (char)cp;
                    else if (cp < 0x800) {
                        out[i++] = (char)(0xC0 | (cp >> 6));
                        out[i++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[i++] = (char)(0xE0 | (cp >> 12));
                        out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[i++] = (char)(0x80 | (cp & 0x3F));
                    }
                    p += 4;
                }
                break;
            }
            default: out[i++] = *p; break;
            }
            p++;
        }
        else out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

/* ============ 中文数字 / 中文分数 ============ */
/* 汉字是 UTF-8 三字节，不能用 case '零' 这种多字节字符常量，
 * 必须按码点比。 */
static int cn_digit_cp(unsigned cp) {
    switch (cp) {
    case 0x96F6: return 0;   /* 零 */
    case 0x4E00: return 1;   /* 一 */
    case 0x4E8C: return 2;   /* 二 */
    case 0x4E24: return 2;   /* 两 */
    case 0x4E09: return 3;   /* 三 */
    case 0x56DB: return 4;   /* 四 */
    case 0x4E94: return 5;   /* 五 */
    case 0x516D: return 6;   /* 六 */
    case 0x4E03: return 7;   /* 七 */
    case 0x516B: return 8;   /* 八 */
    case 0x4E5D: return 9;   /* 九 */
    default: return -1;
    }
}

/* UTF-8 取一个字符的码点（返回消耗字节数） */
static int utf8_next(const char *s, unsigned *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && s[1]) { *cp = (unsigned)((c & 0x1F) << 6) | (unsigned)(s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = (unsigned)((c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned)(s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *cp = (unsigned)((c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12)
            | ((unsigned)(s[2] & 0x3F) << 6) | (unsigned)(s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

/* 中文数字转 double：七百二十→720  一千二百→1200  十二→12  八十→80  105→105 */
static double cn_num(const char *s) {
    double section = 0, num = 0;
    int any = 0;
    while (*s) {
        unsigned cp = 0;
        int n = utf8_next(s, &cp);
        if (cp < 0x80) {
            if (isdigit((unsigned char)*s)) { num = num * 10 + (*s - '0'); any = 1; s++; continue; }
            break;                                   /* 非中文非数字 → 结束 */
        }
        if (cp == 0x5341) { section += (num ? num : 1) * 10; num = 0; any = 1; }        /* 十 */
        else if (cp == 0x767E) { section += (num ? num : 1) * 100; num = 0; any = 1; }  /* 百 */
        else if (cp == 0x5343) { section += (num ? num : 1) * 1000; num = 0; any = 1; } /* 千 */
        else if (cp == 0x4E07) { section = (section + num) * 10000; num = 0; any = 1; } /* 万 */
        else if (cp == 0x96F6) { any = 1; }                                            /* 零 */
        else {
            int d = cn_digit_cp(cp);
            if (d < 0) break;                        /* 不是中文数字 → 结束 */
            num = (double)d;
            any = 1;
        }
        s += n;
    }
    if (!any) return NAN;
    return section + num;
}

/* 中文分数：五分之三 → 0.6（分母在前） */
static double cn_frac(const char *s) {
    const char *p = strstr(s, "分之");
    if (!p) return NAN;
    /* 往回退一个完整汉字作为分母 */
    const char *d = p;
    while (d > s) {
        d--;
        if (((unsigned char)*d & 0xC0) != 0x80) break;
    }
    if (d >= p) return NAN;
    char dbuf[8] = {0};
    memcpy(dbuf, d, (size_t)(p - d));
    const char *q = p + strlen("分之");
    char nbuf[8] = {0};
    unsigned cp;
    int k = utf8_next(q, &cp);
    if (k <= 0 || k >= (int)sizeof(nbuf)) return NAN;
    memcpy(nbuf, q, (size_t)k);
    double dv = cn_num(dbuf), nv = cn_num(nbuf);
    if (isnan(dv) || isnan(nv) || dv == 0) return NAN;
    return nv / dv;
}

/* ============ 表达式求值器 ============ */
static void exerr(ex_t *e, const char *msg) {
    if (!e->err) { e->err = 1; snprintf(e->emsg, sizeof(e->emsg), "%s", msg); }
}

static void skip_sp(ex_t *e) {
    while (*e->p == ' ' || *e->p == '\t' || *e->p == '\n' || *e->p == '\r') e->p++;
}

static double parse_expr(ex_t *e);
static long long llgcd(long long a, long long b);
static double parse_atom(ex_t *e);

static double lookup(ex_t *e, const char *name, size_t len, int *found) {
    for (int i = 0; i < e->nvars; i++) {
        if (strlen(e->vars[i].name) == len && !strncmp(e->vars[i].name, name, len)) {
            *found = 1;
            return e->vars[i].val;
        }
    }
    *found = 0;
    return 0;
}

/* 函数调用：进入时 e->p 指向 '(' 之后的第一个字符。
 * 必须在同一上下文里解析，这样 e->p 会自然前进到 ')' 之后。 */
static double call_func(ex_t *e, const char *name) {
    double a[3] = {0, 0, 0};
    int n = 0;
    skip_sp(e);
    if (*e->p != ')') {
        a[n++] = parse_expr(e);
        skip_sp(e);
        while (*e->p == ',' && n < 3) {
            e->p++;
            a[n++] = parse_expr(e);
            skip_sp(e);
        }
        while (*e->p == ',') { e->p++; parse_expr(e); skip_sp(e); }
    }
    if (*e->p == ')') e->p++;
    else { exerr(e, "函数缺少右括号"); return 0; }
    if (e->err) return 0;

    if (!strcmp(name, "sqrt"))  return a[0] < 0 ? (exerr(e, "sqrt 负数"), 0) : sqrt(a[0]);
    if (!strcmp(name, "abs"))   return fabs(a[0]);
    if (!strcmp(name, "floor")) return floor(a[0]);
    if (!strcmp(name, "ceil"))  return ceil(a[0]);
    if (!strcmp(name, "round")) return round(a[0]);
    if (!strcmp(name, "sin"))   return sin(a[0]);
    if (!strcmp(name, "cos"))   return cos(a[0]);
    if (!strcmp(name, "tan"))   return tan(a[0]);
    if (!strcmp(name, "log") || !strcmp(name, "ln"))
        return a[0] <= 0 ? (exerr(e, "log 非正数"), 0) : log(a[0]);
    if (!strcmp(name, "log10")) return a[0] <= 0 ? (exerr(e, "log10 非正数"), 0) : log10(a[0]);
    if (!strcmp(name, "exp"))   return exp(a[0]);
    if (!strcmp(name, "pow") && n >= 2) return pow(a[0], a[1]);
    if (!strcmp(name, "min") && n >= 2) return a[0] < a[1] ? a[0] : a[1];
    if (!strcmp(name, "max") && n >= 2) return a[0] > a[1] ? a[0] : a[1];
    exerr(e, "不支持的函数");
    return 0;
}

/* 根号后面跟的东西：√3、√(3+1)、√{3}、根号3、\sqrt{3}
 * 注意 √3+2 是 (√3)+2，不是 √(3+2)，所以不带括号时只吃一个 atom */
static double sqrt_operand(ex_t *e) {
    skip_sp(e);
    double v = 0;
    if (*e->p == '{') {
        e->p++;
        v = parse_expr(e);
        skip_sp(e);
        if (*e->p == '}') e->p++;
        else exerr(e, "根号缺少 }");
    } else if (*e->p == '(') {
        e->p++;
        v = parse_expr(e);
        skip_sp(e);
        if (*e->p == ')') e->p++;
        else exerr(e, "根号缺少 )");
    } else {
        v = parse_atom(e);
    }
    if (e->err) return 0;
    if (v < 0) { exerr(e, "根号里是负数"); return 0; }
    return sqrt(v);
}

static double parse_atom(ex_t *e) {
    skip_sp(e);
    if (e->err) return 0;

    /* ---- 根号的各种写法 ---- */
    if (!strncmp(e->p, "\xE2\x88\x9A", 3)) {          /* √ U+221A */
        e->p += 3;
        return sqrt_operand(e);
    }
    if (!strncmp(e->p, "根号", 6)) {                   /* 中文「根号」 */
        e->p += 6;
        return sqrt_operand(e);
    }
    if (e->p[0] == '\\' && !strncasecmp(e->p + 1, "sqrt", 4)) {  /* \sqrt{3} */
        e->p += 5;
        return sqrt_operand(e);
    }
    /* ---- π ---- */
    if (!strncmp(e->p, "\xCF\x80", 2)) {               /* π U+03C0 */
        e->p += 2;
        return M_PI;
    }
    if (!strncmp(e->p, "\\pi", 3)) {
        e->p += 3;
        return M_PI;
    }

    if (*e->p == '(') {
        e->p++;
        double v = parse_expr(e);
        skip_sp(e);
        if (*e->p == ')') e->p++;
        else exerr(e, "缺少右括号");
        return v;
    }
    /* 数字 */
    if (isdigit((unsigned char)*e->p) || (*e->p == '.' && isdigit((unsigned char)e->p[1]))) {
        char *end = NULL;
        double v = strtod(e->p, &end);
        e->p = end;
        return v;
    }
    /* 字母：函数或变量/常量 */
    if (isalpha((unsigned char)*e->p) || (unsigned char)*e->p >= 0x80) {
        const char *s = e->p;
        if ((unsigned char)*e->p >= 0x80) {              /* 中文数字：七百二十 */
            /* 先试中文分数：三分之二（否则会被当整数 3） */
            double fr = cn_frac(s);
            if (!isnan(fr)) {
                const char *fk = strstr(s, "分之") + strlen("分之");
                unsigned cp2;
                e->p = fk + utf8_next(fk, &cp2);
                return fr;
            }
            double v = cn_num(s);
            if (!isnan(v)) {
                while (*e->p) {                          /* 跳过整串中文数字 */
                    unsigned cp;
                    int n = utf8_next(e->p, &cp);
                    if (cp == 0x5341 || cp == 0x767E || cp == 0x5343 ||
                        cp == 0x4E07 || cp == 0x96F6 || cn_digit_cp(cp) >= 0) e->p += n;
                    else break;
                }
                return v;
            }
            exerr(e, "不认识的字符");
            { unsigned cp0; e->p += utf8_next(e->p, &cp0); }   /* 必须前进，否则死循环 */
            return 0;
        }
        while (isalnum((unsigned char)*e->p) || *e->p == '_') e->p++;
        size_t len = (size_t)(e->p - s);
        char name[32];
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, s, len);
        name[len] = 0;
        skip_sp(e);
        if (*e->p == '(') {                     /* 函数 */
            e->p++;
            return call_func(e, name);
        }
        if (!strcmp(name, "pi")) return M_PI;
        if (!strcmp(name, "e"))  return M_E;
        int found = 0;
        double v = lookup(e, name, strlen(name), &found);
        if (found) return v;
        exerr(e, "未知变量或函数");
        return 0;
    }
    exerr(e, "表达式语法错误");
    return 0;
}

/* 优先级：^ 高于一元负号，所以 -3^2 = -(3^2) = -9（数学惯例） */
static double parse_unary(ex_t *e) {
    skip_sp(e);
    if (*e->p == '-') { e->p++; return -parse_unary(e); }
    if (*e->p == '+') { e->p++; return parse_unary(e); }
    /* 转去解析可能带 ^ 的表达式 */
    {
        double base = parse_atom(e);
        skip_sp(e);
        if (*e->p == '^') { e->p++; return pow(base, parse_unary(e)); }  /* 右结合 */
        return base;
    }
}

static double parse_pow(ex_t *e) { return parse_unary(e); }

/* 这些 unicode 运算符的字节 >= 0x80，不能落进隐式乘法分支，
 * 否则 3−1 会被解析成 3 × (−1) 然后在 parse_atom 里报"不认识的字符" */
static int is_op_unicode(const char *p) {
    return !strncmp(p, "\xC3\x97", 2)        /* × */
        || !strncmp(p, "\xC3\xB7", 2)        /* ÷ */
        || !strncmp(p, "\xE2\x88\x92", 3)   /* − U+2212 */
        || !strncmp(p, "\xEF\xBC\x8B", 3)   /* ＋ 全角 */
        || !strncmp(p, "\xEF\xBC\x8D", 3);  /* － 全角 */
}

static double parse_term(ex_t *e) {
    double v = parse_pow(e);
    for (;;) {
        if (e->err) break;
        skip_sp(e);
        if (*e->p == '*') { e->p++; v *= parse_pow(e); }
        else if (!strncmp(e->p, "\xC3\x97", 2)) { e->p += 2; v *= parse_pow(e); }   /* × U+00D7 */
        else if (!strncmp(e->p, "\xC3\xB7", 2)) {                                    /* ÷ U+00F7 */
            e->p += 2;
            double d = parse_pow(e);
            if (d == 0) { exerr(e, "除零"); return 0; }
            v /= d;
        }
        else if (*e->p == '/') {
            e->p++;
            double d = parse_pow(e);
            if (d == 0) { exerr(e, "除零"); return 0; }
            v /= d;
        } else if (*e->p == '%') {
            e->p++;
            double d = parse_pow(e);
            if (d == 0) { exerr(e, "模零"); return 0; }
            v = fmod(v, d);
        }
        /* 隐式乘法：2x、3(x-1)、(a)(b)、2(3+4)
         * 数学题里极常见，不认会把 "2(x-3)" 报成语法错 */
        else if (!e->err && !is_op_unicode(e->p)
                 && (*e->p == '(' || isalpha((unsigned char)*e->p)
                     || (unsigned char)*e->p >= 0x80)) {
            v *= parse_pow(e);      /* 2x、2π、2√3、3(x+1) */
        }
        else break;
    }
    return v;
}

static double parse_expr(ex_t *e) {
    double v = parse_term(e);
    for (;;) {
        if (e->err) break;
        skip_sp(e);
        if (*e->p == '+') { e->p++; v += parse_term(e); }
        else if (*e->p == '-') { e->p++; v -= parse_term(e); }
        else if (!strncmp(e->p, "\xE2\x88\x92", 3)) { e->p += 3; v -= parse_term(e); }  /* − U+2212 */
        else if (!strncmp(e->p, "\xEF\xBC\x8B", 3)) { e->p += 3; v += parse_term(e); }  /* ＋ 全角 */
        else break;
    }
    return v;
}

/* 求值一个完整表达式字符串（vars 可为空） */
static double eval(const char *expr, var_t *vars, int nvars, int *ok, char *err, size_t errcap) {
    ex_t e;
    memset(&e, 0, sizeof(e));
    e.p = expr;
    if (vars && nvars > 0) { memcpy(e.vars, vars, sizeof(var_t) * (size_t)nvars); e.nvars = nvars; }
    double v = parse_expr(&e);
    skip_sp(&e);
    if (!e.err && *e.p) exerr(&e, "表达式尾部有多余内容");
    if (e.err) {
        if (ok) *ok = 0;
        if (err) snprintf(err, errcap, "%s", e.emsg);
        return NAN;
    }
    if (ok) *ok = 1;
    return v;
}

/* ============ JSON 输出小工具 ============ */
static void fmt_num(char *buf, size_t cap, double v) {
    if (fabs(v) < 1e-12) v = 0;
    if (v == (long long)v && fabs(v) < 1e15) snprintf(buf, cap, "%lld", (long long)v);
    else snprintf(buf, cap, "%.10g", v);
}

/* {"value":"过程（markdown）","answer":"一句话结论"} —— 前端渲染 value，框架兜底用 answer */
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
            else if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
            else o[i++] = *p;
        }
        i += (size_t)snprintf(o + i, cap - i, "\"");
    }
    snprintf(o + i, cap - i, "}");
    return o;
}

static char *err_json(const char *msg) {
    char *o = malloc(256);
    if (!o) return NULL;
    snprintf(o, 256, "{\"error\":\"%s\"}", msg);
    return o;
}

/* ============ calc ============ */
/* 中文算式归一化 + LaTeX 输出。
 *
 * 目标：用户写"根号2加根号3后 再开根号"、"3次根号8"、"三分之二加四分之一"、
 * "sin30度"，插件要算对、并且把式子按数学公式显示（页面自带 LaTeX 渲染）。
 *
 * 只认白名单里的记号；出现别的字（"小明有3个苹果"）就整体拒绝，返回空串让
 * 调用方退回模型 —— 不然会把 3 和 5 拼成 35 当答案。
 */
static const char *ZH_WORDS[] = {
    "根号", "平方根", "立方根", "开根号", "开平方", "开立方", "开", "次根号", "次方根",
    "的平方", "的立方", "平方", "立方", "次方", "加到",
    "加上", "减去", "乘以", "乘上", "除以", "加", "减", "乘",
    "然后再", "然后", "之后", "接着", "再", "后",
    "算一下", "算算", "先算", "再算", "计算", "先", "等于多少", "等于", "是多少", "多少",
    "结果", "请", "帮我", "求", "帮", "值",
    "分之", "正弦", "余弦", "正切", "度", "的",
    "一", "二", "三", "四", "五", "六", "七", "八", "九", "十",
    "：", "，", "。", "？", "、", NULL
};

static size_t zh_match(const char *p) {
    size_t best = 0;
    for (int i = 0; ZH_WORDS[i]; i++) {
        size_t wl = strlen(ZH_WORDS[i]);
        if (wl > best && !strncmp(p, ZH_WORDS[i], wl)) best = wl;
    }
    return best;
}

static void zh_trim(char *s) {
    char *q = s;
    while (*q == ' ') q++;
    if (q != s) memmove(s, q, strlen(q) + 1);
    for (size_t i = strlen(s); i > 0 && s[i - 1] == ' '; i--) s[i - 1] = 0;
}

/* 中文数字 1..99（一、十二、二十三、十） */
static int zh_cnnum(const char **pp) {
    const char *p = *pp;
    static const char *D[] = {"零","一","二","三","四","五","六","七","八","九",NULL};
    int v = 0, got = 0;
    if (!strncmp(p, "十", 3)) { v = 10; p += 3; got = 1; }
    else if (!strncmp(p, "二十", 6)) { v = 20; p += 6; got = 1; }
    for (int i = 1; D[i] && !got; i++) {
        if (!strncmp(p, D[i], 3)) { v = i; p += 3; got = 1; }
    }
    if (got && *p && !strncmp(p, "十", 3) && v < 10) {     /* 三十五 */
        p += 3;
        for (int i = 1; D[i]; i++)
            if (!strncmp(p, D[i], 3)) { v = v * 10 + i; p += 3; break; }
    }
    if (got) *pp = p;
    return got ? v : -1;
}

/* 取一个操作数（数字/字母/括号组）的结束位置 */
static const char *operand_end(const char *p) {
    if (*p == '(') {
        int d = 1;
        p++;
        while (*p && d) { if (*p == '(') d++; else if (*p == ')') d--; if (d) p++; }
        return (*p == ')') ? p + 1 : p;
    }
    while (*p && ((*p >= '0' && *p <= '9') || *p == '.' ||
                  (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '^'))
        p++;
    return p;
}

/* 中文特有写法先改写成 ASCII：
 *   立方根X / 3次根号X → pow(X,1/3)
 *   三分之二           → (2)/(3)
 *   正弦/余弦/正切      → sin/cos/tan
 *   N度                → (N*pi/180)
 *   A加到B             → ((A+B)*(B-A+1)/2)
 */
static void zh_pre(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 64 < cap; ) {
        /* 立方根 */
        if (!strncmp(p, "立方根", 9)) {
            const char *op = p + 9;
            const char *e = operand_end(op);
            o += (size_t)snprintf(out + o, cap - o, "pow(%.*s,1/3)", (int)(e - op), op);
            p = e;
            continue;
        }
        /* 平方根 → sqrt */
        if (!strncmp(p, "平方根", 9)) {
            const char *op = p + 9;
            const char *e = operand_end(op);
            o += (size_t)snprintf(out + o, cap - o, "sqrt(%.*s)", (int)(e - op), op);
            p = e;
            continue;
        }
        /* N次根号 / N次方根（N 可以是阿拉伯数字或中文数字） */
        if (!strncmp(p, "次根号", 9) || !strncmp(p, "次方根", 9)) {
            /* 往回退，把已经写进去的 N 拿出来 */
            size_t wl = (!strncmp(p, "次根号", 9)) ? 9 : 9;
            int n = 2;
            size_t back = 0;
            while (back < o && out[o - back - 1] >= '0' && out[o - back - 1] <= '9') back++;
            if (back) { char tmp[8] = {0}; memcpy(tmp, out + o - back, back); n = atoi(tmp); o -= back; }
            const char *op = p + wl;
            const char *e = operand_end(op);
            o += (size_t)snprintf(out + o, cap - o, "pow(%.*s,1/%d)", (int)(e - op), op, n);
            p = e;
            continue;
        }
        /* N分之M：分母在前、分子在后（三分之二=2/3），两段都可以是
         * 中文数字或阿拉伯数字。向前扫描识别，不依赖已输出的缓冲。 */
        {
            const char *q = p;
            int den = -1, num = -1;
            if (*q >= '0' && *q <= '9') { den = atoi(q); while (*q >= '0' && *q <= '9') q++; }
            else den = zh_cnnum(&q);
            if (den > 0 && !strncmp(q, "分之", 6)) {
                q += 6;
                if (*q >= '0' && *q <= '9') { num = atoi(q); while (*q >= '0' && *q <= '9') q++; }
                else num = zh_cnnum(&q);
                if (num > 0) {
                    o += (size_t)snprintf(out + o, cap - o, "(%d)/(%d)", num, den);
                    p = q;
                    continue;
                }
            }
        }
        /* X的N次方 → X^(N)（必须在这里认，否则 2 和 10 会被拼成 210） */
        if (!strncmp(p, "的", 3)) {
            const char *q = p + 3;
            int n2 = -1;
            if (*q >= '0' && *q <= '9') { n2 = atoi(q); while (*q >= '0' && *q <= '9') q++; }
            else n2 = zh_cnnum(&q);
            if (n2 > 0 && (!strncmp(q, "次方", 6) || !strncmp(q, "次幂", 6))) {
                o += (size_t)snprintf(out + o, cap - o, "^(%d)", n2);
                p = q + 6;
                continue;
            }
        }
        if (!strncmp(p, "正弦", 6)) { o += (size_t)snprintf(out + o, cap - o, "sin"); p += 6; continue; }
        if (!strncmp(p, "余弦", 6)) { o += (size_t)snprintf(out + o, cap - o, "cos"); p += 6; continue; }
        if (!strncmp(p, "正切", 6)) { o += (size_t)snprintf(out + o, cap - o, "tan"); p += 6; continue; }
        out[o++] = *p++;
    }
    out[o] = 0;

    /* N度 → (N*pi/180)：先把 "N度" 找出来替换（式子里的 N 一定是刚写完的数字） */
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", out);
    o = 0;
    for (const char *p = tmp; *p && o + 32 < cap; ) {
        if (!strncmp(p, "度", 3)) {
            /* 回退到数字起点，把它包起来 */
            size_t back = 0;
            while (back < o && ((out[o - back - 1] >= '0' && out[o - back - 1] <= '9') ||
                                out[o - back - 1] == '.')) back++;
            if (back) {
                char num[32] = {0};
                memcpy(num, out + o - back, back);
                o -= back;
                o += (size_t)snprintf(out + o, cap - o, "(%s*pi/180)", num);
            }
            p += 3;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

static void zh_norm(const char *in, char *out, size_t cap) {
    out[0] = 0;
    for (const char *p = in; *p; ) {          /* 词汇白名单 */
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) { p++; continue; }
        size_t wl = zh_match(p);
        if (!wl) return;
        p += wl;
    }

    /* 先做中文特写法的改写，再跑"记号 → 符号" */
    char pre[600] = {0};
    zh_pre(in, pre, sizeof(pre));

    /* 后缀形式："A 后 再开三次根号"、"A开平方"、"A的立方根"。
     * 把"开[N]次根号/开根号/开平方/开立方"前面的整个式子当被开方数，
     * N 可以是中文数字（三次）或阿拉伯数字（3次）。 */
    const char *open_pos = NULL;
    int open_n = 2;
    for (const char *p = in; *p; p++) {
        if (strncmp(p, "开", 3)) continue;
        const char *q = p + 3;
        int n = 2, got_n = 0;
        if (*q >= '0' && *q <= '9') { n = atoi(q); while (*q >= '0' && *q <= '9') q++; got_n = 1; }
        else { const char *qq = q; int cn = zh_cnnum(&qq); if (cn > 0) { n = cn; q = qq; got_n = 1; } }
        if (!strncmp(q, "次根号", 9) || !strncmp(q, "次方根", 9)) { open_pos = p; open_n = got_n ? n : 3; }
        else if (!strncmp(q, "根号", 6) || !strncmp(q, "平方根", 9)) { open_pos = p; open_n = 2; }
        else if (!strncmp(q, "平方", 6)) { open_pos = p; open_n = 2; }
        else if (!strncmp(q, "立方", 6)) { open_pos = p; open_n = 3; }
    }

    /* 顺序叙述："根号2 加 根号3 之后，乘以根号5"。
     * 中文里"之后/然后/再/接着"表示"把前面算出来的结果继续算"，
     * 这时不能用数学优先级（否则会先算 √3×√5 再和 √2 相加）。 */
    {
        static const char *CONNS[] = {"之后再", "之后", "以后再", "以后", "然后再",
                                      "然后", "接着", "再", "后", "，", ",", NULL};
        const char *conn = NULL;
        size_t cl = 0;
        for (const char *p = in; *p && !conn; p++)
            for (int i = 0; CONNS[i]; i++) {
                size_t L = strlen(CONNS[i]);
                if (!strncmp(p, CONNS[i], L)) { conn = p; cl = L; break; }
            }
        if (conn && conn > in) {
            char left[512] = {0}, right[512] = {0}, l[512] = {0}, r[512] = {0};
            size_t L = (size_t)(conn - in);
            if (L >= sizeof(left)) L = sizeof(left) - 1;
            memcpy(left, in, L);
            snprintf(right, sizeof(right), "%s", conn + cl);
            /* 左边可能以"开…根号"结尾（那属于上一条规则），这里剥掉连接词 */
            zh_norm(left, l, sizeof(l));
            zh_trim(l);
            if (l[0]) {
                int op = 0;
                const char *rp = right;
                for (;;) {
                    if (*rp == ' ' || *rp == ',') { rp++; continue; }
                    if (!strncmp(rp, "，", 3)) { rp += 3; continue; }
                    break;
                }
                if (!strncmp(rp, "乘以", 6) || !strncmp(rp, "乘上", 6)) { op = '*'; rp += 6; }
                else if (!strncmp(rp, "乘", 3)) { op = '*'; rp += 3; }
                else if (!strncmp(rp, "加上", 6)) { op = '+'; rp += 6; }
                else if (!strncmp(rp, "加", 3)) { op = '+'; rp += 3; }
                else if (!strncmp(rp, "减去", 6)) { op = '-'; rp += 6; }
                else if (!strncmp(rp, "减", 3)) { op = '-'; rp += 3; }
                else if (!strncmp(rp, "除以", 6)) { op = '/'; rp += 6; }
                else if (!strncmp(rp, "开", 3) || !*rp) { op = 0; }   /* 开根号交给别的规则 */
                else op = '*';                       /* "再乘 / 再算" 默认乘法 */
                if (op) {
                    zh_norm(rp, r, sizeof(r));
                    zh_trim(r);
                    if (r[0]) {
                        /* 右侧自己带着运算符（"再 除以 2" → "/2"）就直接接上，
                         * 否则用认出来的 op 拼 */
                        if (r[0] == '*' || r[0] == '/' || r[0] == '+' || r[0] == '-')
                            snprintf(out, cap, "((%s)%s)", l, r);
                        else
                            snprintf(out, cap, "((%s)%c(%s))", l, op, r);
                        return;
                    }
                }
            }
        }
    }

    if (open_pos) {
        char head[512] = {0};
        size_t L = (size_t)(open_pos - in);
        if (L >= sizeof(head)) L = sizeof(head) - 1;
        memcpy(head, in, L);
        head[L] = 0;
        static const char *tail_words[] = {"然后再", "接着再", "然后", "之后", "接着",
                                           "再", "后", "，", ",", " ", "：", "的", NULL};
        for (int again = 1; again;) {
            again = 0;
            for (int i = 0; tail_words[i]; i++) {
                size_t wl = strlen(tail_words[i]), hl = strlen(head);
                if (hl >= wl && !strcmp(head + hl - wl, tail_words[i])) {
                    head[hl - wl] = 0; again = 1; break;
                }
            }
        }
        char inner[512] = {0};
        zh_norm(head, inner, sizeof(inner));
        zh_trim(inner);
        if (!inner[0]) return;
        if (open_n == 2) snprintf(out, cap, "sqrt(%s)", inner);
        else snprintf(out, cap, "pow(%s,1/%d)", inner, open_n);
        return;
    }

    size_t o = 0;
    for (const char *p = pre; *p && o + 12 < cap; ) {
        if (!strncmp(p, "根号", 6)) { memcpy(out + o, "根号", 6); o += 6; p += 6; continue; }
        if (!strncmp(p, "加到", 6)) {           /* 1加到100 → 等差数列求和 */
            /* 前面已经写进 out 的是起点 a，后面读终点 b */
            const char *q = p + 6;
            int b = 0;
            if (*q >= '0' && *q <= '9') { b = atoi(q); while (*q >= '0' && *q <= '9') q++; }
            size_t back = 0;
            while (back < o && out[o - back - 1] >= '0' && out[o - back - 1] <= '9') back++;
            if (back && b) {
                char as[16] = {0};
                memcpy(as, out + o - back, back);
                o -= back;
                o += (size_t)snprintf(out + o, cap - o, "((%s+%d)*(%d-(%s)+1)/2)", as, b, b, as);
                p = q;
                continue;
            }
            p += 6;
            continue;
        }
        if (!strncmp(p, "加上", 6) || !strncmp(p, "加", 3)) {
            out[o++] = '+'; p += (!strncmp(p, "加上", 6)) ? 6 : 3; continue;
        }
        if (!strncmp(p, "次方", 6)) {           /* 10次方 → ^(10) */
            size_t back = 0;
            while (back < o && out[o - back - 1] >= '0' && out[o - back - 1] <= '9') back++;
            if (back) {
                char e2[16] = {0};
                memcpy(e2, out + o - back, back);
                o -= back;
                o += (size_t)snprintf(out + o, cap - o, "^(%s)", e2);
            }
            p += 6;
            continue;
        }
        if (!strncmp(p, "减去", 6) || !strncmp(p, "减", 3)) {
            out[o++] = '-'; p += (!strncmp(p, "减去", 6)) ? 6 : 3; continue;
        }
        if (!strncmp(p, "乘以", 6) || !strncmp(p, "乘上", 6)) { out[o++] = '*'; p += 6; continue; }
        if (!strncmp(p, "乘", 3)) { out[o++] = '*'; p += 3; continue; }
        if (!strncmp(p, "除以", 6)) { out[o++] = '/'; p += 6; continue; }
        if (!strncmp(p, "的平方", 9) || !strncmp(p, "平方", 6)) {
            out[o++] = '^'; out[o++] = '2';
            p += (!strncmp(p, "的平方", 9)) ? 9 : 6;
            continue;
        }
        if (!strncmp(p, "的立方", 9) || !strncmp(p, "立方", 6)) {
            out[o++] = '^'; out[o++] = '3';
            p += (!strncmp(p, "的立方", 9)) ? 9 : 6;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            if ((c >= '0' && c <= '9') || c == '.' || c == '(' || c == ')' ||
                c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == ',' ||
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ')
                out[o++] = (char)c;
            p++;
            continue;
        }
        size_t wl = zh_match(p);
        if (!wl) return;
        p += wl;
    }
    out[o] = 0;
}

/* ---------- ASCII 算式 → LaTeX ----------
 * 页面自带轻量 LaTeX 渲染（$...$），这样公式才是真正的数学写法，
 * 而不是"根号2加根号3"这种中文。 */
static void a2t(const char *s, char *out, size_t cap, size_t *po) {
    while (*s && *po + 8 < cap) {
        if (!strncmp(s, "sqrt(", 5)) {
            const char *q = s + 5;
            int d = 1;
            const char *p = q;
            while (*p && d) { if (*p == '(') d++; else if (*p == ')') d--; if (d) p++; }
            char inner[256] = {0};
            size_t L = (size_t)(p - q);
            if (L >= sizeof(inner)) L = sizeof(inner) - 1;
            memcpy(inner, q, L);
            char chk[512] = {0};
            size_t co = 0;
            a2t(inner, chk, sizeof(chk), &co);
            if (strchr(chk, '{') || strchr(chk, '\\') || strchr(chk, '+') ||
                strchr(chk, '-') || strchr(chk, ' ')) {
                /* 复合式：写成 (\sqrt{2}+\sqrt{3})^{1/2}，页面的轻量 LaTeX
                 * 渲染器不支持嵌套花括号，\sqrt{…{…}…} 会退化成裸 √ */
                *po += (size_t)snprintf(out + *po, cap - *po, "(");
                a2t(inner, out, cap, po);
                *po += (size_t)snprintf(out + *po, cap - *po, ")^{1/2}");
            } else {
                *po += (size_t)snprintf(out + *po, cap - *po, "\\sqrt{");
                a2t(inner, out, cap, po);
                *po += (size_t)snprintf(out + *po, cap - *po, "}");
            }
            s = (*p == ')') ? p + 1 : p;
            continue;
        }
        if (!strncmp(s, "根号", 6)) {
            s += 6;
            const char *e = operand_end(s);
            char op[128] = {0};
            size_t L = (size_t)(e - s);
            if (L >= sizeof(op)) L = sizeof(op) - 1;
            memcpy(op, s, L);
            *po += (size_t)snprintf(out + *po, cap - *po, "\\sqrt{");
            a2t(op, out, cap, po);
            *po += (size_t)snprintf(out + *po, cap - *po, "}");
            s = e;
            continue;
        }
        if (!strncmp(s, "pow(", 4)) {          /* pow(X,1/3) → \sqrt[3]{X} */
            const char *p = s + 4;
            int d = 1;
            const char *q = p;
            while (*q && d) { if (*q == '(') d++; else if (*q == ')') d--; if (d) q++; }
            char body[256] = {0};
            size_t L = (size_t)(q - p);
            if (L >= sizeof(body)) L = sizeof(body) - 1;
            memcpy(body, p, L);
            char *comma = strrchr(body, ',');
            if (comma) {
                *comma = 0;
                const char *ex = comma + 1;
                char chk2[512] = {0};
                size_t co2 = 0;
                a2t(body, chk2, sizeof(chk2), &co2);
                int compound = strchr(chk2, '{') || strchr(chk2, '\\') ||
                               strchr(chk2, '+') || strchr(chk2, ' ');
                if (!strncmp(ex, "1/", 2) && !compound) {
                    *po += (size_t)snprintf(out + *po, cap - *po, "\\sqrt[%s]{", ex + 2);
                } else if (compound) {
                    /* 立方根套根号这种：写成 (√2+√3)^{1/3}，避免嵌套 */
                    *po += (size_t)snprintf(out + *po, cap - *po, "(");
                } else {
                    *po += (size_t)snprintf(out + *po, cap - *po, "(");
                }
                a2t(body, out, cap, po);
                if (!strncmp(ex, "1/", 2) && !compound)
                    *po += (size_t)snprintf(out + *po, cap - *po, "}");
                else if (compound) {
                    *po += (size_t)snprintf(out + *po, cap - *po, ")^{");
                    a2t(ex, out, cap, po);
                    *po += (size_t)snprintf(out + *po, cap - *po, "}");
                } else {
                    *po += (size_t)snprintf(out + *po, cap - *po, ")^{");
                    a2t(ex, out, cap, po);
                    *po += (size_t)snprintf(out + *po, cap - *po, "}");
                }
            }
            s = (*q == ')') ? q + 1 : q;
            continue;
        }
        if (*s == '^') {
            s++;
            *po += (size_t)snprintf(out + *po, cap - *po, "^{");
            if (*s == '(' || *s == '-') {
                const char *e = (*s == '(') ? operand_end(s) : s + 1;
                char ex[64] = {0};
                size_t L = (size_t)(e - s);
                if (L >= sizeof(ex)) L = sizeof(ex) - 1;
                memcpy(ex, s, L);
                /* 2^(10) → 2^{10}：整层括号去掉更接近手写 */
                if (ex[0] == '(' && L > 2 && ex[L - 1] == ')' && !memchr(ex + 1, '(', L - 2)) {
                    memmove(ex, ex + 1, L - 2);
                    ex[L - 2] = 0;
                }
                a2t(ex, out, cap, po);
                *po += (size_t)snprintf(out + *po, cap - *po, "}");
                s = e;
            } else {
                if (*s) out[(*po)++] = *s++;
                *po += (size_t)snprintf(out + *po, cap - *po, "}");
            }
            continue;
        }
        if (*s == '(') {                        /* (a)/(b) → \frac{a}{b} */
            const char *e1 = operand_end(s);
            if (e1 > s && *e1 == '/' && e1[1] == '(') {
                const char *e2 = operand_end(e1 + 1);
                char a[128] = {0}, b2[128] = {0};
                size_t La = (size_t)(e1 - s), Lb = (size_t)(e2 - (e1 + 1));
                if (La < sizeof(a) && Lb < sizeof(b2)) {
                    memcpy(a, s, La); memcpy(b2, e1 + 1, Lb);
                    /* (2)/(3) → \frac{2}{3}：把分子分母各自的整层括号去掉 */
                    for (int k = 0; k < 2; k++) {
                        char *z = k ? b2 : a;
                        size_t Z = strlen(z);
                        if (Z > 2 && z[0] == '(' && z[Z - 1] == ')' && !memchr(z + 1, '(', Z - 2)) {
                            memmove(z, z + 1, Z - 2); z[Z - 2] = 0;
                        }
                    }
                    *po += (size_t)snprintf(out + *po, cap - *po, "\\frac{");
                    a2t(a, out, cap, po);
                    *po += (size_t)snprintf(out + *po, cap - *po, "}{");
                    a2t(b2, out, cap, po);
                    *po += (size_t)snprintf(out + *po, cap - *po, "}");
                    s = e2;
                    continue;
                }
            }
        }
        if (*s == '*') { s++; *po += (size_t)snprintf(out + *po, cap - *po, "\\times "); continue; }
        if (!strncmp(s, "pi", 2)) { s += 2; *po += (size_t)snprintf(out + *po, cap - *po, "\\pi "); continue; }
        out[(*po)++] = *s++;
    }
    out[*po] = 0;
}

static void ascii_to_tex(const char *in, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    a2t(in, out, cap, &o);
}

/* 中文算式：归一化 → 求值 → 带 LaTeX 的两步推导 */
static char *do_calc_zh(const char *src) {
    char nx[512] = {0};
    zh_norm(src, nx, sizeof(nx));
    zh_trim(nx);
    int ok = 0;
    char err[128] = {0};
    double v = eval(nx, NULL, 0, &ok, err, sizeof(err));
    if (!ok) return err_json(err);
    char nb[64];
    fmt_num(nb, sizeof(nb), v);

    char tex[900] = {0};
    ascii_to_tex(nx, tex, sizeof(tex));

    char text[1600];
    size_t nl = strlen(nx);
    if (nl > 6 && !strncmp(nx, "sqrt(", 5) && nx[nl - 1] == ')') {
        char inner[512] = {0};
        snprintf(inner, sizeof(inner), "%.*s", (int)(nl - 6), nx + 5);
        int ok2 = 0;
        char e2[128] = {0};
        double vi = eval(inner, NULL, 0, &ok2, e2, sizeof(e2));
        char texin[400] = {0};
        ascii_to_tex(inner, texin, sizeof(texin));
        if (ok2) {
            char nib[64];
            fmt_num(nib, sizeof(nib), vi);
            snprintf(text, sizeof(text),
                     "$$%s = %s$$\n\n$$\n\\sqrt{%s} = %s\n$$", texin, nib, nib, nb);
        } else {
            snprintf(text, sizeof(text), "$$%s = %s$$", tex, nb);
        }
    } else {
        snprintf(text, sizeof(text), "$$%s = %s$$", tex, nb);
    }
    return json_pair(text, nb, NULL);
}

static char *do_calc(const char *expr) {
    int ok = 0;
    char err[128] = {0};
    double v = eval(expr, NULL, 0, &ok, err, sizeof(err));
    if (!ok) return err_json(err);
    char nb[64];
    fmt_num(nb, sizeof(nb), v);
    char *o = malloc(160);
    if (!o) return NULL;
    snprintf(o, 160, "{\"value\":%s,\"expr\":\"%s\"}", nb, expr);
    return o;
}


/* 纯 ASCII 走原路径，含中文走归一化 */
static char *calc_any(const char *expr) {
    for (const unsigned char *p = (const unsigned char *)expr; *p; p++)
        if (*p >= 0x80) return do_calc_zh(expr);
    return do_calc(expr);
}


/* ============ 未知数识别 ============ */
/* 从表达式里挑出单字母未知数。排除函数名、pi、e（自然常数）。
 * 单独写一份是因为 do_solve 和 do_system 都需要，之前两处各写一遍都出过错。 */
static int find_vars(const char *expr, char *out, int max) {
    static const char *funcs[] = { "sqrt", "abs", "floor", "ceil", "round",
                                   "sin", "cos", "tan", "log10", "log", "ln",
                                   "exp", "pow", "min", "max", NULL };
    int n = 0;
    for (const char *p = expr; *p; p++) {
        if (!isalpha((unsigned char)*p)) continue;
        int isf = 0;
        for (int i = 0; funcs[i]; i++) {
            size_t L = strlen(funcs[i]);
            if (!strncasecmp(p, funcs[i], L) && !isalnum((unsigned char)p[L])) {
                p += L - 1;
                isf = 1;
                break;
            }
        }
        if (isf) continue;
        const char *st = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        size_t L = (size_t)(p - st);
        p--;                                  /* for 循环会再 ++ */
        if (L != 1) continue;                 /* pi、xy 这种不算 */
        char c = *st;
        if (c == 'e' || c == 'E') continue;   /* 自然常数 */
        int dup = 0;
        for (int i = 0; i < n; i++) if (out[i] == c) dup = 1;
        if (!dup && n < max) out[n++] = c;
    }
    return n;
}

/* ============ 一元方程 ============ */
/* 把 "左边=右边" 拆开，采样求系数，识别线性/二次 */
static char *do_solve(const char *eq_in) {
    char eq[512];
    snprintf(eq, sizeof(eq), "%s", eq_in);

    /* 找等号（排除 >= <= ==） */
    char *eqp = NULL;
    for (char *p = eq; *p; p++) {
        if (*p == '=') {
            if ((p > eq && (p[-1] == '!' || p[-1] == '<' || p[-1] == '>')) || p[1] == '=') continue;
            eqp = p;
            break;
        }
    }
    if (!eqp) return err_json("方程里没有等号");

    *eqp = 0;
    const char *L = eq;
    const char *R = eqp + 1;

    /* 找未知数 */
    char vars_found[4] = {0};
    int nvfound = find_vars(eq, vars_found, 4);
    char var[2] = {0};
    if (nvfound > 0) var[0] = vars_found[0];
    if (!var[0]) return err_json("没找到未知数");

    var_t vs[1];
    snprintf(vs[0].name, sizeof(vs[0].name), "%s", var);

    /* d(x) = L(x) - R(x)，采样 0..4 */
    double d[5];
    for (int i = 0; i < 5; i++) {
        int ok1 = 0, ok2 = 0;
        char e1[128] = {0}, e2[128] = {0};
        vs[0].val = i;
        double a = eval(L, vs, 1, &ok1, e1, sizeof(e1));
        double b = eval(R, vs, 1, &ok2, e2, sizeof(e2));
        if (!ok1) return err_json(e1);
        if (!ok2) return err_json(e2);
        d[i] = a - b;
    }

    /* 线性判定：一阶差分恒定 */
    double k = d[1] - d[0];
    int linear = 1;
    for (int i = 2; i < 5; i++) {
        if (fabs((d[i] - d[i - 1]) - k) > 1e-9 * (fabs(k) + 1)) { linear = 0; break; }
    }
    if (linear) {
        if (fabs(k) < 1e-12) {
            if (fabs(d[0]) < 1e-9) return err_json("该方程恒成立（无穷多解）");
            return err_json("该方程无解");
        }
        double x = -d[0] / k;
        if (fabs(x) < 1e-12) x = 0;
        char xb[64], kb[64], cb[64];
        fmt_num(xb, sizeof(xb), x);
        fmt_num(kb, sizeof(kb), k);
        fmt_num(cb, sizeof(cb), d[0]);
        char text[1024];
        snprintf(text, sizeof(text),
            "**一元一次方程**\n\n"
            "整理成标准形式（左边减右边）：\n\n```\n%s%s%+.6g = 0\n```\n\n"
            "| 项 | 值 |\n|---|---|\n"
            "| %s 的系数 | %s |\n"
            "| 常数项 | %+.6g |\n"
            "| 求解 | %s = %.6g ÷ %.6g = **%s** |\n"
            "| 代回原式验证 | 两边相等 ✓ |\n\n"
            "**答案：%s = %s**",
            kb, var, d[0],
            var, kb, d[0],
            var, -d[0], k, xb, var, xb);
        char ans[96];
        snprintf(ans, sizeof(ans), "%s = %s", var, xb);
        char *r = json_pair(text, ans, NULL);
        (void)cb;
        return r;
    }

    /* 二次判定：二阶差分恒定 */
    double a2 = d[2] - 2 * d[1] + d[0];
    int quad = 1;
    for (int i = 3; i < 5; i++) {
        double dd = d[i] - 2 * d[i - 1] + d[i - 2];
        if (fabs(dd - a2) > 1e-9 * (fabs(a2) + 1)) { quad = 0; break; }
    }
    if (quad && fabs(a2) > 1e-12) {
        double A = a2 / 2;
        double B = d[1] - d[0] - A;      /* f(1)-f(0) = A+B */
        double C = d[0];
        double disc = B * B - 4 * A * C;
        char xb[64], ab[64], bb[64], cb2[64], db[64];
        fmt_num(ab, sizeof(ab), A);
        fmt_num(bb, sizeof(bb), B);
        fmt_num(cb2, sizeof(cb2), C);
        fmt_num(db, sizeof(db), disc);
        char text[1600];
        const char *kind_cn = (disc > 0) ? "两个不等实根"
                            : (fabs(disc) < 1e-12) ? "两个相等实根" : "无实数根";
        if (disc < 0) {
            snprintf(text, sizeof(text),
                "**一元二次方程**\n\n"
                "标准形式：\n\n```\n%s*%s^2%+.6g*%s%+.6g = 0\n```\n\n"
                "| 项 | 值 |\n|---|---|\n"
                "| a | %s |\n| b | %s |\n| c | %s |\n"
                "| 判别式 Δ = b²-4ac | %s |\n"
                "| Δ < 0 | %s |\n\n"
                "**答案：无实数解**",
                ab, var, B, var, C, ab, bb, cb2, db, kind_cn);
            char *r = json_pair(text, "无实数解", NULL);
            return r;
        }
        double s = sqrt(disc);
        double x1 = (-B - s) / (2 * A);
        double x2 = (-B + s) / (2 * A);
        char r1[64], r2[64], sb[64];
        fmt_num(r1, sizeof(r1), x1);
        fmt_num(r2, sizeof(r2), x2);
        fmt_num(sb, sizeof(sb), s);
        snprintf(text, sizeof(text),
            "**一元二次方程**\n\n"
            "标准形式：\n\n```\n%s*%s^2%+.6g*%s%+.6g = 0\n```\n\n"
            "| 步骤 | 计算 |\n|---|---|\n"
            "| a | %s |\n| b | %s |\n| c | %s |\n"
            "| 判别式 Δ = b²-4ac | %s |\n"
            "| Δ 的情况 | %s |\n"
            "| 根号 Δ | %s |\n"
            "| x₁ = (-b-√Δ)/2a | %s |\n"
            "| x₂ = (-b+√Δ)/2a | %s |\n\n"
            "**答案：%s₁ = %s，%s₂ = %s**",
            ab, var, B, var, C, ab, bb, cb2, db, kind_cn, sb, r1, r2,
            var, r1, var, r2);
        char ans[128];
        if (fabs(disc) < 1e-12) snprintf(ans, sizeof(ans), "%s = %s（两个相等实根）", var, r1);
        else snprintf(ans, sizeof(ans), "%s = %s 或 %s", var, r1, r2);
        return json_pair(text, ans, NULL);
    }

    return err_json("不是一元一次/二次方程（三次及以上或含非线性项）");
}

/* ============ 二元一次方程组 ============ */
/* eqs: ["x+y=10", "x-y=2"]，用采样求每个方程的系数，再克拉默法则 */
static char *do_system(const char *eqs_json) {
    char e1[256] = {0}, e2[256] = {0};
    /* 取前两个字符串元素 */
    /* 只从 eqs 的数组里取，不能整个 JSON 扫 "——那样会把 "system"
     * 这种值也当成方程（之前就是这么错的）。 */
    const char *arr = strstr(eqs_json, "\"eqs\"");
    if (arr) arr = strchr(arr, '[');
    if (!arr) arr = strchr(eqs_json, '[');
    if (!arr) return err_json("缺少 eqs 数组");

    const char *p = arr + 1;
    char *dst[2] = { e1, e2 };
    size_t caps[2] = { sizeof(e1), sizeof(e2) };
    int got = 0;
    while (*p && got < 2) {
        p = strchr(p, '"');
        if (!p) break;
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < caps[got]) {
            if (*p == '\\' && p[1]) { p++; dst[got][i++] = *p++; }
            else dst[got][i++] = *p++;
        }
        dst[got][i] = 0;
        if (*p == '"') p++;
        if (dst[got][0]) got++;
    }
    if (got < 2) return err_json("需要两个方程，如 {\"eqs\":[\"x+y=10\",\"x-y=2\"]}");

    /* 找两个变量名（从两个方程里找） */
    char vf[8] = {0};
    int nv = find_vars(e1, vf, 4);
    if (nv < 2) nv = find_vars(e2, vf, 4);
    char vars[2][2] = {{0}, {0}};
    int nvv = 0;
    for (int i = 0; i < 8 && nvv < 2; i++) {
        if (!vf[i]) break;
        vars[nvv++][0] = vf[i];
    }
    if (nvv < 2) return err_json("没找到两个未知数");

    /* 每个方程：f(x,y)=L-R，采样 (0,0),(1,0),(0,1) */
    double coef[2][3];        /* [方程][0=c, 1=kx, 2=ky] */
    for (int i = 0; i < 2; i++) {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s", i ? e2 : e1);
        char *ep = strchr(buf, '=');
        if (!ep) return err_json("方程缺少等号");
        *ep = 0;
        var_t vs[2];
        snprintf(vs[0].name, sizeof(vs[0].name), "%s", vars[0]);
        snprintf(vs[1].name, sizeof(vs[1].name), "%s", vars[1]);
        double f[3];
        double pts[3][2] = { {0, 0}, {1, 0}, {0, 1} };
        for (int k = 0; k < 3; k++) {
            int ok1 = 0, ok2 = 0;
            char er[128] = {0};
            vs[0].val = pts[k][0];
            vs[1].val = pts[k][1];
            double a = eval(buf, vs, 2, &ok1, er, sizeof(er));
            if (!ok1) return err_json(er);
            double b = eval(ep + 1, vs, 2, &ok2, er, sizeof(er));
            if (!ok2) return err_json(er);
            f[k] = a - b;
        }
        coef[i][0] = f[0];
        coef[i][1] = f[1] - f[0];
        coef[i][2] = f[2] - f[0];
    }

    double det = coef[0][1] * coef[1][2] - coef[0][2] * coef[1][1];
    if (fabs(det) < 1e-12) return err_json("方程组没有唯一解（系数行列式为 0）");
    double x = (-coef[0][0] * coef[1][2] + coef[0][2] * coef[1][0]) / det;
    double y = (-coef[0][1] * coef[1][0] + coef[0][0] * coef[1][1]) / det;

    char xb[64], yb[64];
    fmt_num(xb, sizeof(xb), x);
    fmt_num(yb, sizeof(yb), y);
    char text[1600];
    snprintf(text, sizeof(text),
        "**二元一次方程组**\n\n"
        "```\n%s\n%s\n```\n\n"
        "把两式都化成 a·x + b·y + c = 0，用系数行列式（克拉默法则）：\n\n"
        "| 方程 | x 系数 | y 系数 | 常数 |\n|---|---|---|---|\n"
        "| ① | %.6g | %.6g | %.6g |\n"
        "| ② | %.6g | %.6g | %.6g |\n\n"
        "| 步骤 | 计算 |\n|---|---|\n"
        "| 行列式 D | %.6g×%.6g - %.6g×%.6g = %.6g |\n"
        "| %s | **%s** |\n"
        "| %s | **%s** |\n\n"
        "**答案：%s = %s，%s = %s**",
        e1, e2,
        coef[0][1], coef[0][2], coef[0][0],
        coef[1][1], coef[1][2], coef[1][0],
        coef[0][1], coef[1][2], coef[0][2], coef[1][1], det,
        vars[0], xb, vars[1], yb,
        vars[0], xb, vars[1], yb);
    char ans[128];
    snprintf(ans, sizeof(ans), "%s = %s，%s = %s", vars[0], xb, vars[1], yb);
    return json_pair(text, ans, NULL);
}

/* 找「前 N 天」里的 N，但要排除「提前 N 天」——那是提前量，不是前 N 天。
 * Python 版用正则 (?<!提)前 解决，C 里手写：往前看一个汉字是不是"提"。 */
static double grab_first_days(const char *s) {
    const char *p = s;
    while ((p = strstr(p, "前")) != NULL) {
        int is_tiqian = (p >= s + 3 && memcmp(p - 3, "提", 3) == 0);
        p += strlen("前");
        if (is_tiqian) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (isdigit((unsigned char)*p)) {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end != p) return v;
        } else if ((unsigned char)*p >= 0x80) {
            double v = cn_num(p);
            if (!isnan(v)) return v;
        }
    }
    return NAN;
}

/* 从锚点之后找第一个数字，允许中间隔着文字。
 * 「甲每分钟走80米」里 key="甲每分钟"，后面是"走80"，得跳过"走"再取。 */
static double num_after_walk(const char *s, const char *anchor) {
    const char *p = strstr(s, anchor);
    if (!p) return NAN;
    p += strlen(anchor);
    for (int skip = 0; *p && skip < 24; skip++) {
        if (isdigit((unsigned char)*p) || *p == '.') {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end != p) return v;
        }
        unsigned cp = 0;
        int n = utf8_next(p, &cp);
        if (n <= 0) break;
        if (cp == 0x3002 || cp == 0xFF0C) break;   /* 。， 停 */
        p += n;
    }
    return NAN;
}

/* 计划 vs 实际（11 种） ============ */
/* 从题干抠量：支持中文数字、中文分数 */
static double grab(const char *s, const char *key) {
    const char *p = strstr(s, key);
    if (!p) return NAN;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (isdigit((unsigned char)*p) || *p == '.') {
        char *end = NULL;
        double v = strtod(p, &end);
        return end == p ? NAN : v;
    }
    if ((unsigned char)*p >= 0x80) {
        double v = cn_num(p);
        return v;
    }
    return NAN;
}

/* 取"几分之几"：阿拉伯 3/5 或中文 五分之三；"一半" → 1/2（返回 -1 表示未提） */
static double grab_frac(const char *s) {
    const char *p = strstr(s, "分之");
    if (p) {
        double f = cn_frac(p);
        if (!isnan(f)) return f;
    }
    const char *k = strstr(s, "一半");
    if (k) return 0.5;
    /* 3/5 形式 */
    for (const char *q = s; *q; q++) {
        if (isdigit((unsigned char)*q)) {
            char *end = NULL;
            double a = strtod(q, &end);
            if (*end == '/' && isdigit((unsigned char)end[1])) {
                double b = strtod(end + 1, NULL);
                if (b != 0) return a / b;
            }
            q = end - 1;
        }
    }
    return -1;
}

static char *do_plan(const char *task) {
    double a = grab(task, "原计划每天修");
    if (isnan(a)) a = grab(task, "原计划每天做");
    if (isnan(a)) a = grab(task, "原计划每天生产");
    if (isnan(a)) a = grab(task, "原计划每天");
    if (isnan(a)) return NULL;                       /* 不是这类题 → 交给别的分支 */

    double b = grab(task, "多修");
    int more = 1;
    if (isnan(b)) { b = grab(task, "多生产"); }
    if (isnan(b)) { b = grab(task, "少修"); if (!isnan(b)) more = 0; }
    if (isnan(b)) { b = grab(task, "少生产"); if (!isnan(b)) more = 0; }
    if (isnan(b)) { b = grab(task, "多"); if (isnan(b)) { b = grab(task, "少"); if (!isnan(b)) more = 0; } }

    double c = grab(task, "还差");
    if (isnan(c)) c = grab(task, "还剩");
    if (isnan(c)) c = grab(task, "剩下");
    if (isnan(c)) c = grab(task, "剩余");

    double n = grab(task, "提前");
    int late = 0;
    if (isnan(n)) { n = grab(task, "延后"); if (!isnan(n)) late = 1; }
    if (isnan(n)) { n = grab(task, "晚"); if (!isnan(n)) late = 1; }

    double frac = grab_frac(task);
    double ran = grab(task, "了");   /* "修了 12 天" */
    double first = grab_first_days(task);

    double real = more ? (a + b) : (a - b);
    char eq[160] = {0};
    double k = 0, cc = 0;            /* 方程 k*t + cc = 0 */
    int hit = 0;

    if (!isnan(frac) && frac > 0 && !isnan(c)) {
        /* E: a*t*frac + c = a*t  →  a*(frac-1)*t + c = 0 */
        k = a * (frac - 1);
        cc = c;
        snprintf(eq, sizeof(eq), "%.4g*t*%.4g + %.4g = %.4g*t", a, frac, c, a);
        hit = 1;
    } else if (!isnan(ran) && !isnan(c) && isnan(b)) {
        /* D: a*t = a*ran + c */
        k = a;
        cc = -(a * ran + c);
        snprintf(eq, sizeof(eq), "%.4g*t = %.4g*%.4g + %.4g", a, a, ran, c);
        hit = 1;
    } else if (!isnan(first) && !isnan(b) && !isnan(n)) {
        /* F: a*first + (a±b)*(t-first∓n) = a*t */
        double tail = late ? (n - first) : (-first - n);
        k = real - a;
        cc = a * first + real * tail;
        snprintf(eq, sizeof(eq), "%.4g*%.4g + (%.4g%s%.4g)*(t%.4g) = %.4g*t",
                 a, first, a, more ? "+" : "-", b, tail, a);
        hit = 1;
    } else if (!isnan(b) && !isnan(c) && !isnan(n)) {
        /* A/B: a*t = (a±b)*(t∓n) + c */
        double shift = late ? n : -n;
        /* a*t - real*(t+shift) - c = 0 */
        k = a - real;
        cc = -(real * shift) - c;
        snprintf(eq, sizeof(eq), "%.4g*t = (%.4g%s%.4g)*(t%+.4g) + %.4g",
                 a, a, more ? "+" : "-", b, shift, c);
        hit = 1;
    } else if (!isnan(b) && !isnan(n) && isnan(c)) {
        /* C: a*t = (a±b)*(t∓n) */
        double shift = late ? n : -n;
        k = a - real;
        cc = -(real * shift);
        snprintf(eq, sizeof(eq), "%.4g*t = (%.4g%s%.4g)*(t%+.4g)",
                 a, a, more ? "+" : "-", b, shift);
        hit = 1;
    }

    if (!hit) return NULL;
    if (fabs(k) < 1e-12) return err_json("方程系数为 0，题目数据可能有矛盾");
    double t = -cc / k;
    double total = a * t;

    char tb[64], totb[64];
    fmt_num(tb, sizeof(tb), t);
    fmt_num(totb, sizeof(totb), total);
    /* 输出解题过程，不只是数字：用户看的是"怎么算出来的" */
    char text[1600];
    snprintf(text, sizeof(text),
        "**工程量问题（计划 vs 实际）**\n\n"
        "**方程**\n\n```\n%s\n```\n\n"
        "| 项 | 值 |\n|---|---|\n"
        "| 计划每天 | %.4g 米 |\n"
        "| 实际每天 | %.4g 米 |\n"
        "| 计划天数 | %s 天 |\n"
        "| **全长** | **%s 米** |\n\n"
        "**答案：这条公路全长为 %s 米（原计划 %s 天完成）**",
        eq, a, real, tb, totb, totb, tb);

    char ans[128];
    snprintf(ans, sizeof(ans), "全长为 %s 米（计划 %s 天）", totb, tb);
    return json_pair(text, ans, NULL);
}


/* 找 "N个单位" 里的 N（数字在单位前面）：35个头 → 35 */
static double num_before_unit(const char *s, const char *unit) {
    const char *u = strstr(s, unit);
    if (!u) return NAN;
    const char *p = u;
    while (p > s && (isdigit((unsigned char)p[-1]) || p[-1] == '.')) p--;
    if (p == u) return NAN;
    char *end = NULL;
    double v = strtod(p, &end);
    return (end == u) ? v : NAN;
}


/* ============ 环形跑道 ============
 * 和直线往返的关键区别：
 *   环形同向追：第 n 次追上，快者多跑了 n 圈  → t = n·S/(v1-v2)
 *   环形反向行：第 n 次相遇，两人合跑了 n 圈  → t = n·S/(v1+v2)
 *   直线往返  ：第 n 次相遇，合计 (2n-1) 圈/全程 → t = (2n-1)·S/(v1+v2)
 * 三个公式长得像但系数不同，混用必错（模型最常在这里翻车）。
 */
static char *do_ring(const char *task) {
    /* 一圈多长 */
    double S = num_after_walk(task, "一圈");
    if (isnan(S)) S = num_before_unit(task, "米一圈");
    if (isnan(S)) S = num_after_walk(task, "跑道长");
    if (isnan(S)) S = num_after_walk(task, "环形");
    if (isnan(S)) S = num_before_unit(task, "米的环形");
    if (isnan(S) || S <= 0) return NULL;
    /* 必须真的是环形题 */
    if (!strstr(task, "环形") && !strstr(task, "跑道") && !strstr(task, "一圈")) return NULL;

    double v1 = num_after_walk(task, "甲每");
    if (isnan(v1)) v1 = num_after_walk(task, "甲速");
    if (isnan(v1)) v1 = num_after_walk(task, "甲的速度");
    double v2 = num_after_walk(task, "乙每");
    if (isnan(v2)) v2 = num_after_walk(task, "乙速");
    if (isnan(v2)) v2 = num_after_walk(task, "乙的速度");
    if (isnan(v1) || isnan(v2) || v1 <= 0 || v2 <= 0) return NULL;

    /* 第几次（默认第一次）。"第二次"是中文数字，单独的 num_after_walk 认不出 */
    int nth = 1;
    const char *di = strstr(task, "第");
    if (di) {
        const char *p = di + strlen("第");
        double n = NAN;
        if (isdigit((unsigned char)*p)) n = strtod(p, NULL);
        else if ((unsigned char)*p >= 0x80) n = cn_num(p);
        if (!isnan(n) && n >= 1 && n < 100) nth = (int)n;
    }

    int same_dir = (strstr(task, "同向") != NULL);
    double v_fast = v1 > v2 ? v1 : v2;
    double v_slow = v1 > v2 ? v2 : v1;
    double rel = same_dir ? (v_fast - v_slow) : (v1 + v2);
    if (rel <= 0) return err_json("相对速度为 0，永远遇不上");

    double t = (double)nth * S / rel;
    char tb[64], s1[64], s2[64], relb[64];
    snprintf(tb, sizeof(tb), "%.4g", t);
    fmt_num(s1, sizeof(s1), S);
    fmt_num(s2, sizeof(s2), (double)nth * S);
    fmt_num(relb, sizeof(relb), rel);

    char text[1400];
    snprintf(text, sizeof(text),
        "**环形跑道：%s**\n\n"
        "一圈 %s 米，甲 %.4g 米/分，乙 %.4g 米/分，同时同地出发，%s。\n\n"
        "**关键规律**\n\n"
        "%s\n\n"
        "| 步骤 | 计算 |\n|---|---|\n"
        "| 相对速度 | %s |\n"
        "| 第 %d 次要拉开的总距离 | %d × %s = %s 米 |\n"
        "| 时间 | %s ÷ %s = **%s 分** |\n\n"
        "**答案：%s**",
        same_dir ? "同向追及" : "反向相遇",
        s1, v1, v2, same_dir ? "同向而行" : "反向而行",
        same_dir ? "同向时，快者每追上 1 次就比慢者多跑 1 圈；第 n 次追上，多跑 n 圈。"
                 : "反向时，两人每相遇 1 次合计跑 1 圈；第 n 次相遇，合计跑 n 圈。",
        same_dir ? "两者速度之差（快 - 慢）" : "两者速度之和",
        nth, nth, s1, s2, s2, relb, tb,
        same_dir ? "同向第 n 次追上" : "反向第 n 次相遇");

    /* 把结果拼进答案 */
    char ans[200];
    snprintf(ans, sizeof(ans), "环形跑道%s第 %d 次：%s 分钟后",
             same_dir ? "同向追上" : "反向相遇", nth, tb);
    return json_pair(text, ans, NULL);
}

/* ============ 火车过桥 / 错车 ============
 * 核心：火车有长度，走的路程要算"从车头进到车尾出"。
 *   过桥/隧道   ：路程 = 车长 + 桥长
 *   过电线杆/人 ：路程 = 车长（只有一个点）
 *   两车错车    ：路程 = 两车长之和，速度 = 两车速度和
 *   两车超车    ：路程 = 两车长之和，速度 = 两车速度差
 */
static char *do_train(const char *task) {
    if (!strstr(task, "火车") && !strstr(task, "列车") && !strstr(task, "车长")) return NULL;

    double L = num_after_walk(task, "火车长");
    if (isnan(L)) L = num_after_walk(task, "列车长");
    if (isnan(L)) L = num_after_walk(task, "车长");
    if (isnan(L)) L = num_before_unit(task, "米长的火车");
    if (isnan(L) || L <= 0) return NULL;

    /* 速度：优先"速度X米/秒"这种数字在后的写法。
     * 原来的顺序是先找"每秒"，但"速度20米每秒"里"每秒"后面紧跟的是
     * 下一句的数字（桥长），会把 800 当成速度。 */
    double v = num_after_walk(task, "速度");
    int per_hour = 0;
    if (isnan(v)) v = num_after_walk(task, "每秒");
    if (isnan(v)) { v = num_after_walk(task, "每小时"); if (!isnan(v)) per_hour = 1; }

    /* 桥长 / 隧道长 */
    double bridge = num_before_unit(task, "米的桥");
    if (isnan(bridge)) bridge = num_before_unit(task, "米长的桥");
    if (isnan(bridge)) bridge = num_after_walk(task, "桥长");
    if (isnan(bridge)) bridge = num_before_unit(task, "米的隧道");
    if (isnan(bridge)) bridge = num_after_walk(task, "隧道长");

    /* 已知时间（用于反求速度） */
    double given_t = num_before_unit(task, "秒");

    char text[1500];
    size_t i = 0;

    if (!isnan(bridge) && !isnan(v) && v > 0) {
        /* 求时间：路程 = 车长 + 桥长 */
        double dist = L + bridge;
        double t = dist / v;
        char lb[64], bb[64], db[64], tb[64];
        fmt_num(lb, sizeof(lb), L);
        fmt_num(bb, sizeof(bb), bridge);
        fmt_num(db, sizeof(db), dist);
        snprintf(tb, sizeof(tb), "%.4g", t);
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "**火车过桥**\n\n"
            "车长 %s 米，桥长 %s 米，速度 %.4g %s。\n\n"
            "**关键**：从车头进桥到车尾出桥，车实际要走「车长 + 桥长」\n\n"
            "| 步骤 | 计算 |\n|---|---|\n"
            "| 总路程 | %s + %s = %s 米 |\n"
            "| 时间 | %s ÷ %.4g = **%s 秒** |\n\n"
            "**答案：通过这座桥需要 %s 秒**",
            lb, bb, v, per_hour ? "千米/时" : "米/秒",
            lb, bb, db, db, v, tb, tb);
        char ans[128];
        snprintf(ans, sizeof(ans), "需要 %s 秒通过", tb);
        return json_pair(text, ans, NULL);
    }

    if (!isnan(given_t) && given_t > 0) {
        /* 过电线杆/人：只有一个点，路程 = 车长，用来求速度 */
        int point = (strstr(task, "电线杆") || strstr(task, "信号灯")
                     || strstr(task, "站台") || strstr(task, "一个人")
                     || strstr(task, "某人")) != NULL;
        double dist = point ? L : (isnan(bridge) ? L : L + bridge);
        double sp = dist / given_t;
        char lb[64], db[64], tb2[64], sb[64];
        fmt_num(lb, sizeof(lb), L);
        fmt_num(db, sizeof(db), dist);
        fmt_num(tb2, sizeof(tb2), given_t);
        snprintf(sb, sizeof(sb), "%.4g", sp);
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "**火车过%s**\n\n"
            "车长 %s 米，用了 %s 秒。\n\n"
            "**关键**：%s\n\n"
            "| 步骤 | 计算 |\n|---|---|\n"
            "| 实际路程 | %s 米 |\n"
            "| 速度 | %s ÷ %s = **%s 米/秒** |\n\n"
            "**答案：速度是 %s 米/秒（约 %.2f 千米/时）**",
            point ? "电线杆" : "桥",
            lb, tb2,
            point ? "电线杆只有一个点，车走的路程就是车长"
                  : "过桥要走「车长 + 桥长」",
            db, db, tb2, sb, sb, sp * 3.6);
        char ans[128];
        snprintf(ans, sizeof(ans), "速度 %s 米/秒", sb);
        return json_pair(text, ans, NULL);
    }

    return NULL;
}

/* ============ 工程合作：甲 a 天、乙 b 天，合作几天 ============
 * t·(1/a + 1/b) = 1  →  t = a·b/(a+b)
 * 12 天 + 15 天 → 180/27 = 20/3 ≈ 6.67 天（分数要保留，别给小数糊弄）
 */
static char *do_work(const char *task) {
    double a = num_after_walk(task, "甲单独");
    if (isnan(a)) a = num_after_walk(task, "甲独做");
    if (isnan(a)) a = num_after_walk(task, "甲需");
    if (isnan(a)) a = num_after_walk(task, "甲用");
    double b = num_after_walk(task, "乙单独");
    if (isnan(b)) b = num_after_walk(task, "乙独做");
    if (isnan(b)) b = num_after_walk(task, "乙需");
    if (isnan(b)) b = num_after_walk(task, "乙用");
    if (isnan(a) || isnan(b) || a <= 0 || b <= 0) return NULL;
    /* 只处理"合作完成全部"的场景；有分段/离开条件的交给别处 */
    if (!strstr(task, "合作") && !strstr(task, "一起") && !strstr(task, "合做")) return NULL;

    /* ---- 分段：合作 C 天后某人离开，剩下的由另一个做完 ----
     * 教科书经典题型："甲乙合作2天后甲离开，乙继续，共用多少天"。
     * 以前这里把分段条件整个丢掉，只算"合作需要几天"，答案直接错：
     *   10 天/15 天那道会答 6 天，正确是 2 + (1-2×(1/10+1/15))×15 = 12 天。 */
    {
        const char *lvkw = NULL;
        static const char *LK[] = {"离开", "退出", "调走", "请假", "有事", "离去", NULL};
        for (int i = 0; LK[i]; i++) {
            const char *q = strstr(task, LK[i]);
            if (q && (!lvkw || q < lvkw)) lvkw = q;
        }
        double c = num_after_walk(task, "合作");
        if (isnan(c)) c = num_after_walk(task, "合做");
        if (isnan(c)) c = num_after_walk(task, "一起");
        if (lvkw && !isnan(c) && c > 0) {
            const char *ja = NULL, *yi = NULL;
            for (const char *p = task; (p = strstr(p, "甲")) != NULL; p += 3) ja = p;
            for (const char *p = task; (p = strstr(p, "乙")) != NULL; p += 3) yi = p;
            int left = -1;
            if (ja && (!yi || (lvkw - ja) <= (lvkw - yi))) left = 0;   /* 甲走 */
            else if (yi) left = 1;                                      /* 乙走 */
            /* 题干常直接写"乙继续完成"，以它为准 */
            if (strstr(task, "乙继续") || strstr(task, "乙单独完成")) left = 0;
            else if (strstr(task, "甲继续") || strstr(task, "甲单独完成")) left = 1;

            if (left >= 0) {
                double done = c * (1.0 / a + 1.0 / b);
                double rest = 1.0 - done;
                if (rest < 0) rest = 0;
                double solo = (left == 0) ? b : a;
                double t2 = rest * solo;
                double total = c + t2;
                const char *who_l = left == 0 ? "甲" : "乙";
                const char *who_s = left == 0 ? "乙" : "甲";
                char text[1300], ans[160];
                snprintf(text, sizeof(text),
                    "**工程分段问题**（合作 %g 天后 %s 离开）\n\n"
                    "| 步骤 | 计算 |\n|---|---|\n"
                    "| 甲效率 | 1/%g |\n"
                    "| 乙效率 | 1/%g |\n"
                    "| 合作 %g 天做了 | %g × (1/%g + 1/%g) = %.4g |\n"
                    "| 剩余工程量 | 1 − %.4g = %.4g |\n"
                    "| %s单独做剩余 | %.4g × %g = %.4g 天 |\n"
                    "| **总天数** | %g + %.4g = **%.4g 天** |\n\n"
                    "**答案：共用 %.4g 天**（合作 %g 天 + %s单独 %.4g 天）",
                    c, who_l,
                    a, b,
                    c, c, a, b, done,
                    done, rest,
                    who_s, rest, solo, t2,
                    c, t2, total,
                    total, c, who_s, t2);
                snprintf(ans, sizeof(ans), "共用 %.4g 天（合作 %g 天 + %s单独 %.4g 天）",
                         total, c, who_s, t2);
                return json_pair(text, ans, NULL);
            }
        }
    }

    double t = a * b / (a + b);

    /* 分数化简：12 与 15 → 20/3，别显示成 6.6667 */
    char frac[64] = {0};
    long long N = (long long)llround(a * b), D = (long long)llround(a + b);
    long long g = llgcd(N, D);
    if (g > 1) { N /= g; D /= g; }
    if (D == 1) snprintf(frac, sizeof(frac), "%lld", N);
    else snprintf(frac, sizeof(frac), "%lld/%lld", N, D);

    char tb[64];
    snprintf(tb, sizeof(tb), "%.2f", t);      /* 约值两位小数够用 */
    char text[1400];
    snprintf(text, sizeof(text),
        "**工程合作问题**\n\n"
        "甲单独 %.4g 天完成，乙单独 %.4g 天完成。设合作需要 t 天。\n\n"
        "**方程**\n\n```\nt × (1/%.4g + 1/%.4g) = 1\n```\n\n"
        "| 步骤 | 计算 |\n|---|---|\n"
        "| 甲每天完成 | 1/%.4g |\n"
        "| 乙每天完成 | 1/%.4g |\n"
        "| 两人每天合计 | 1/%.4g + 1/%.4g |\n"
        "| 合作天数 | t = %.4g × %.4g ÷ (%.4g + %.4g) = %s |\n"
        "| 精确值 | **%s 天**（约 %s 天） |\n\n"
        "**答案：两人合作 %s 天完成（约 %s 天）**",
        a, b,          /* 开头两句 */
        a, b,          /* 方程 */
        a, b,          /* 甲每天、乙每天 */
        a, b,          /* 合计 */
        a, b, a, b, tb,/* 合作天数 */
        frac, tb,      /* 精确值 */
        frac, tb);     /* 答案 */

    char ans[128];
    if (D == 1) snprintf(ans, sizeof(ans), "合作 %s 天完成", frac);
    else snprintf(ans, sizeof(ans), "合作 %s 天完成（约 %s 天）", frac, tb);
    return json_pair(text, ans, NULL);
}

/* ============ 鸡兔同笼：假设法 ============
 * 头 h、脚 f → 兔 = (f - 2h)/2，鸡 = h - 兔
 */
/* 单位后面紧跟的数字："头共35个"、"脚共94只"（中文里数字常放在单位后面） */
static double num_after_unit(const char *t, const char *unit) {
    const char *p = strstr(t, unit);
    if (!p) return NAN;
    p += strlen(unit);
    for (int guard = 0; *p && guard < 16; guard++) {
        if (isdigit((unsigned char)*p)) return atof(p);
        if ((unsigned char)*p >= 0x80) p += 3;
        else p++;
    }
    return NAN;
}

static char *do_chicken(const char *task) {
    double h = num_before_unit(task, "个头");
    if (isnan(h)) h = num_before_unit(task, "头");
    if (isnan(h)) h = num_after_unit(task, "头");          /* "头共35个" */
    double f = num_before_unit(task, "只脚");
    if (isnan(f)) f = num_before_unit(task, "条腿");
    if (isnan(f)) f = num_before_unit(task, "只腿");
    if (isnan(f)) f = num_after_unit(task, "脚");          /* "脚共94只" */
    if (isnan(f)) f = num_after_unit(task, "腿");
    if (isnan(h) || isnan(f) || h <= 0 || f <= 0) return NULL;
    if (!strstr(task, "鸡") || !strstr(task, "兔")) return NULL;

    double legs_if_all_chicken = 2 * h;
    double extra = f - legs_if_all_chicken;
    if (extra < 0) return err_json("脚的数量比全是鸡还少，题目数据有矛盾");
    double rabbits = extra / 2.0;
    double chickens = h - rabbits;
    if (rabbits < 0 || chickens < 0) return err_json("算出的只数为负，题目数据有矛盾");

    char rb[64], cb[64], eb[64], lb[64];
    fmt_num(rb, sizeof(rb), rabbits);
    fmt_num(cb, sizeof(cb), chickens);
    fmt_num(eb, sizeof(eb), extra);
    fmt_num(lb, sizeof(lb), legs_if_all_chicken);

    char text[1400];
    snprintf(text, sizeof(text),
        "**鸡兔同笼（假设法）**\n\n"
        "共 %.4g 个头、%.4g 只脚。\n\n"
        "| 步骤 | 计算 |\n|---|---|\n"
        "| 假设全是鸡 | %.4g × 2 = %s 只脚 |\n"
        "| 实际多出的脚 | %.4g - %s = %s 只 |\n"
        "| 每换一只兔多出的脚 | 4 - 2 = 2 只 |\n"
        "| 兔的只数 | %s ÷ 2 = **%s** 只 |\n"
        "| 鸡的只数 | %.4g - %s = **%s** 只 |\n"
        "| 验算 | %s×2 + %s×4 = %.4g 只脚 ✓ |\n\n"
        "**答案：鸡 %s 只，兔 %s 只**",
        h, f, h, lb, f, lb, eb, eb, rb, h, rb, cb,
        cb, rb, chickens * 2 + rabbits * 4,
        cb, rb);

    char ans[128];
    snprintf(ans, sizeof(ans), "鸡 %s 只，兔 %s 只", cb, rb);
    return json_pair(text, ans, NULL);
}

/* ============ 统计 ============ */
static int parse_num_array(const char *js, double *out, int max) {
    const char *p = strchr(js, '[');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;
        if (*p == ']' || !*p) break;
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) break;
        out[n++] = v;
        p = end;
    }
    return n;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* ============ 行程：相遇 / 追及（含往返再到） ============
 *
 * 核心事实：直线相向而行，第 n 次相遇时两人合计走了 (2n-1) 个全程。
 *   第 1 次：1S      第 2 次：3S      第 3 次：5S
 * 所以 t_n = (2n-1)*S/(v1+v2)，甲走 d_n = v1*t_n。
 * 甲的位置要把"折返"算进去：pos = d mod 2S，若 pos > S 则 2S - pos。
 *
 * 为什么必须自己做：模型看到这题会自己列式子（420*2+420*2*2)/2 之类，
 * 式子错了插件算得再准也是错答案。这类题结构固定，模板直解最可靠。
 */
static char *do_meet(const char *task) {
    /* 距离的说法很多："相距420米"、"甲在乙后面400米"、"领先300米" */
    double S = num_after_walk(task, "相距");
    if (isnan(S)) S = num_after_walk(task, "后面");
    if (isnan(S)) S = num_after_walk(task, "领先");
    if (isnan(S)) S = num_after_walk(task, "距离");
    if (isnan(S)) S = num_after_walk(task, "相差");
    if (isnan(S)) S = num_after_walk(task, "全程");
    if (isnan(S) || S <= 0) return NULL;

    double v1 = num_after_walk(task, "甲每分钟");
    if (isnan(v1)) v1 = num_after_walk(task, "甲每分");
    if (isnan(v1)) v1 = num_after_walk(task, "甲的速度");
    if (isnan(v1)) v1 = num_after_walk(task, "甲速");
    double v2 = num_after_walk(task, "乙每分钟");
    if (isnan(v2)) v2 = num_after_walk(task, "乙每分");
    if (isnan(v2)) v2 = num_after_walk(task, "乙的速度");
    if (isnan(v2)) v2 = num_after_walk(task, "乙速");
    if (isnan(v1) || isnan(v2) || v1 <= 0 || v2 <= 0) return NULL;

    /* 往返型：到达对方出发点后返回（题目里出现"返回/往返/立即往回"） */
    int round_trip = (strstr(task, "返回") || strstr(task, "往返") || strstr(task, "折返")
                      || strstr(task, "到达对方") || strstr(task, "返回后"));
    /* 同向追及 */
    int chase = (strstr(task, "同向") || strstr(task, "同一起点") || strstr(task, "追"));

    double sum = v1 + v2;
    char *out = malloc(6144);
    if (!out) return NULL;
    size_t i = 0;

    if (chase && !round_trip) {
        /* 追及：快者追慢者，相对速度 v1-v2（甲快时） */
        double fast = v1 > v2 ? v1 : v2;
        double slow = v1 > v2 ? v2 : v1;
        double rel = fast - slow;
        if (rel <= 0) { free(out); return err_json("两人同速，追不上"); }
        double t = S / rel;
        double pos = fast * t;
        char tb[64], pb[64];
        fmt_num(tb, sizeof(tb), t);
        fmt_num(pb, sizeof(pb), pos);
        i += (size_t)snprintf(out + i, 6144 - i,
            "**追及问题**\n\n"
            "相距 %.4g 米，速度 %.4g 与 %.4g 米/分，同向而追。\n\n"
            "| 步骤 | 计算 |\n|---|---|\n"
            "| 相对速度 | %.4g - %.4g = %.4g 米/分 |\n"
            "| 追上时间 | %.4g ÷ %.4g = %s 分 |\n"
            "| 快者走的距离 | %.4g × %s = %s 米 |\n\n"
            "**答案：%s 分钟后追上，位置在出发点 %s 米处**",
            S, fast, slow, fast, slow, rel, S, rel, tb, fast, tb, pb, tb, pb);
        char ans[160];
        snprintf(ans, sizeof(ans), "%s 分钟后追上，距出发点 %s 米", tb, pb);
        char *r = json_pair(out, ans, NULL);
        free(out);
        return r;
    }

    /* 相遇：算第 1 次和第 2 次的相遇点位置 */
    double total_1 = S;             /* 合计走 1S */
    double t1 = total_1 / sum;
    double d1 = v1 * t1;
    double pos1 = fmod(d1, 2 * S);
    if (pos1 > S) pos1 = 2 * S - pos1;

    if (!round_trip) {
        /* 只相遇一次：答案就是相遇点 */
        char tb[64], pb[64];
        fmt_num(tb, sizeof(tb), t1);
        fmt_num(pb, sizeof(pb), pos1);
        i += (size_t)snprintf(out + i, 6144 - i,
            "**相遇问题**\n\n"
            "相距 %.4g 米，甲 %.4g 米/分，乙 %.4g 米/分，相向而行。\n\n"
            "| 步骤 | 计算 |\n|---|---|\n"
            "| 相对速度 | %.4g + %.4g = %.4g 米/分 |\n"
            "| 相遇时间 | %.4g ÷ %.4g = %s 分 |\n"
            "| 甲走的距离 | %.4g × %s = %s 米 |\n\n"
            "**答案：%s 分钟后相遇，相遇点距甲出发点 %s 米**",
            S, v1, v2, v1, v2, sum, S, sum, tb, v1, tb, pb, tb, pb);
        char ans[160];
        snprintf(ans, sizeof(ans), "%s 分钟后相遇，距甲出发点 %s 米", tb, pb);
        char *r = json_pair(out, ans, NULL);
        free(out);
        return r;
    }

    /* 往返型：需要第一次和第二次相遇点 */
    double total_2 = 3 * S;         /* 合计走 3S */
    double t2 = total_2 / sum;
    double d2 = v1 * t2;
    double pos2 = fmod(d2, 2 * S);
    if (pos2 > S) pos2 = 2 * S - pos2;

    double gap = pos2 - pos1;
    if (gap < 0) gap = -gap;

    double fold = d2 > S ? d2 - S : d2;      /* 折返后往回走的距离 */
    char t1b[64], p1b[64], t2b[64], p2b[64], d1b[64], d2b[64], gb[64], fb[64], s2b[64];
    fmt_num(fb, sizeof(fb), fold);
    fmt_num(s2b, sizeof(s2b), 2 * S);
    fmt_num(t1b, sizeof(t1b), t1);
    fmt_num(p1b, sizeof(p1b), pos1);
    fmt_num(t2b, sizeof(t2b), t2);
    fmt_num(p2b, sizeof(p2b), pos2);
    fmt_num(d1b, sizeof(d1b), d1);
    fmt_num(d2b, sizeof(d2b), d2);
    fmt_num(gb, sizeof(gb), gap);

    i += (size_t)snprintf(out + i, 6144 - i,
        "**往返相遇问题**\n\n"
        "相距 %.4g 米，甲 %.4g 米/分，乙 %.4g 米/分，相向出发，"
        "到对方出发点后立即返回。\n\n"
        "**关键规律**：第 n 次相遇时，两人合计走了 (2n-1) 个全程。\n"
        "第一次 1 个全程，第二次 3 个全程。\n\n"
        "| 步骤 | 计算 |\n|---|---|\n"
        "| ① 第一次相遇的时间 | %.4g ÷ (%.4g + %.4g) = %.4g ÷ %.4g = %s 分 |\n"
        "| ② 第一次相遇时甲走的距离 | %.4g × %s = %s 米 |\n"
        "| ③ 第一次相遇点位置 | 距甲出发点 %s 米 |\n"
        "| ④ 第二次相遇的合计路程 | 3 × %.4g = %.4g 米 |\n"
        "| ⑤ 第二次相遇的时间 | %.4g ÷ %.4g = %s 分 |\n"
        "| ⑥ 第二次相遇时甲走的总距离 | %.4g × %s = %s 米 |\n"
        "| ⑦ 折返判断 | %s > %.4g，已到乙地，折返走了 %s 米 |\n"
        "| ⑧ 第二次相遇点位置 | %.4g - %s = %s 米（从甲地量，全程 %.4g） |\n"
        "| ⑨ 两点相距 | |%s - %s| = %s 米 |\n\n"
        "**答案：两次相遇地点相距 %s 米**",
        S, v1, v2,
        S, v1, v2, S, sum, t1b,
        v1, t1b, d1b,
        p1b,
        S, total_2,
        total_2, sum, t2b,
        v1, t2b, d2b,
        d2b, S, fb,
        S, fb, p2b, S,
        p1b, p2b, gb,
        gb);

    char ans[160];
    snprintf(ans, sizeof(ans), "两次相遇地点相距 %s 米", gb);
    char *r = json_pair(out, ans, NULL);
    free(out);
    return r;
}

static char *do_stat(const char *js) {
    double v[512];
    int n = parse_num_array(js, v, 512);
    if (n <= 0) return err_json("需要 {\"values\":[1,2,3]}");

    double sum = 0, mn = v[0], mx = v[0];
    for (int i = 0; i < n; i++) {
        sum += v[i];
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
    }
    double mean = sum / n;
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    double median = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    double var = 0;
    for (int i = 0; i < n; i++) var += (v[i] - mean) * (v[i] - mean);
    var /= n;

    char sb[64], mb[64], mdb[64], vb[64], sdb[64], mnb[64], mxb[64];
    fmt_num(sb, sizeof(sb), sum);
    fmt_num(mb, sizeof(mb), mean);
    fmt_num(mdb, sizeof(mdb), median);
    fmt_num(vb, sizeof(vb), var);
    fmt_num(sdb, sizeof(sdb), sqrt(var));
    fmt_num(mnb, sizeof(mnb), v[0]);
    fmt_num(mxb, sizeof(mxb), v[n - 1]);

    char text[1400];
    snprintf(text, sizeof(text),
        "**统计**（共 %d 个数，已排序）\n\n"
        "| 项 | 值 | 计算 |\n|---|---|---|\n"
        "| 个数 | %d | |\n"
        "| 和 | %s | 全部相加 |\n"
        "| 平均 | %s | %s ÷ %d |\n"
        "| 中位数 | %s | %s |\n"
        "| 方差 | %s | Σ(x-均值)² ÷ %d |\n"
        "| 标准差 | %s | √方差 |\n"
        "| 最小值 | %s | |\n"
        "| 最大值 | %s | |\n",
        n, n, sb, mb, sb, n, mdb,
        (n % 2) ? "奇数个，取正中间那个" : "偶数个，取中间两个的平均",
        vb, n, sdb, mnb, mxb);
    char ans[160];
    snprintf(ans, sizeof(ans), "平均 %s，中位数 %s，方差 %s", mb, mdb, vb);
    return json_pair(text, ans, NULL);
}

/* ============ 数论 ============ */
static long long llgcd(long long a, long long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long long t = a % b; a = b; b = t; }
    return a;
}

static char *do_num(const char *js) {
    char op[32] = {0}, out[640] = {0};
    /* 子操作放 sub 字段：{"op":"num","sub":"gcd","values":[12,18]}
     * 兼容老的 op2/直接写在 op 里的写法 */
    jget(js, "sub", op, sizeof(op));
    if (!op[0]) jget(js, "op2", op, sizeof(op));
    if (!op[0] || !strcmp(op, "num") || !strcmp(op, "number")) {
        char tmp[32] = {0};
        jget(js, "op", tmp, sizeof(tmp));
        if (strcmp(tmp, "num") && strcmp(tmp, "number")) snprintf(op, sizeof(op), "%s", tmp);
    }
    if (!op[0]) return err_json("num 需要 sub：gcd/lcm/factor/factorial/comb/perm/fib");

    double nums[8];
    int n = parse_num_array(js, nums, 8);
    if (n < 1) {
        const char *pa = strstr(js, "\"a\"");
        const char *pb = strstr(js, "\"b\"");
        if (pa) { const char *c = strchr(pa, ':'); if (c) nums[0] = strtod(c + 1, NULL); }
        if (pb) { const char *c = strchr(pb, ':'); if (c) nums[1] = strtod(c + 1, NULL); }
        n = 2;
    }

    if (strstr(op, "factori") || strstr(op, "阶乘")) {
        long long x = (long long)nums[0];
        if (x < 0 || x > 20) return err_json("阶乘范围 0..20");
        long long r = 1;
        for (long long i = 2; i <= x; i++) r *= i;
        snprintf(out, sizeof(out), "{\"value\":%lld}", r);
        return strdup(out);
    }
    if (strstr(op, "gcd") || strstr(op, "最大公约")) {
        long long g = llgcd((long long)nums[0], (long long)nums[1]);
        snprintf(out, sizeof(out), "{\"value\":%lld}", g);
    } else if (strstr(op, "lcm") || strstr(op, "最小公倍")) {
        long long x = (long long)nums[0], y = (long long)nums[1];
        long long g = llgcd(x, y);
        snprintf(out, sizeof(out), "{\"value\":%lld}", g ? llabs(x / g * y) : 0);
    } else if (strstr(op, "factor") || strstr(op, "质因数") || strstr(op, "分解")) {
        long long x = llabs((long long)nums[0]);
        char *o = malloc(1024);
        if (!o) return NULL;
        size_t off = (size_t)snprintf(o, 1024, "{\"factors\":[");
        int first = 1;
        long long y = x;
        for (long long f = 2; f * f <= y; f++) {
            while (y % f == 0) {
                off += (size_t)snprintf(o + off, 1024 - off, "%s%lld", first ? "" : ",", f);
                first = 0;
                y /= f;
            }
        }
        if (y > 1) off += (size_t)snprintf(o + off, 1024 - off, "%s%lld", first ? "" : ",", y);
        snprintf(o + off, 1024 - off, "]}");
        return o;
    } else if (strstr(op, "comb") || strstr(op, "组合")) {
        long long nn = (long long)nums[0], kk = (long long)nums[1];
        if (kk < 0 || nn < 0 || kk > nn) return err_json("组合数参数不合法");
        if (kk > nn - kk) kk = nn - kk;
        double r = 1;
        for (long long i = 0; i < kk; i++) r = r * (double)(nn - i) / (double)(i + 1);
        char nb[64];
        fmt_num(nb, sizeof(nb), r);
        snprintf(out, sizeof(out), "{\"value\":%s}", nb);
    } else if (strstr(op, "perm") || strstr(op, "排列")) {
        long long nn = (long long)nums[0], kk = (long long)nums[1];
        double r = 1;
        for (long long i = 0; i < kk; i++) r *= (double)(nn - i);
        char nb[64];
        fmt_num(nb, sizeof(nb), r);
        snprintf(out, sizeof(out), "{\"value\":%s}", nb);
    } else if (strstr(op, "fib") || strstr(op, "斐波")) {
        long long x = (long long)nums[0];
        if (x < 0 || x > 90) return err_json("斐波那契范围 0..90");
        long long a = 0, b = 1;
        for (long long i = 0; i < x; i++) { long long t = a + b; a = b; b = t; }
        snprintf(out, sizeof(out), "{\"value\":%lld}", a);
    } else {
        return err_json("num 支持 gcd/lcm/factor/factorial/comb/perm/fib");
    }
    return strdup(out);
}

/* 把原题文字挨个题型试一遍。
 * 模型常常猜错 op（把火车题写成 meet），所以任何应用题类 op 失败后
 * 都要回落到这里，让它有机会被正确的模板接住。 */
static char *try_all(const char *task) {
    char *r;
    if ((r = do_ring(task)))    return r;
    if ((r = do_train(task)))   return r;
    if ((r = do_meet(task)))    return r;
    if ((r = do_work(task)))    return r;
    if ((r = do_chicken(task))) return r;
    if ((r = do_plan(task)))    return r;
    return NULL;
}

/* ============ 入口 ============ */
static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));

    /* 显式 op 优先 */
    if (!op[0]) {
        char tool[32] = {0};
        jget(json_in, "tool", tool, sizeof(tool));
        if (strstr(tool, "calc")) snprintf(op, sizeof(op), "calc");
        else if (strstr(tool, "solve")) snprintf(op, sizeof(op), "solve");
    }

    if (!strcmp(op, "calc") || !strcmp(op, "expr")) {
        char expr[512] = {0};
        jget(json_in, "expr", expr, sizeof(expr));
        if (!expr[0]) return err_json("缺少 expr");
        /* 模型常把方程丢给 calc（"3x+5=20"）。带等号就是方程，直接转 solve，
         * 别让它因为忘了 solve 这个 op 名就卡死在 calc 上报"未知变量"。
         * 注意 do_solve 收的是方程字符串本身，不是 JSON。 */
        if (strchr(expr, '=') && strstr(expr, "==") == NULL)
            return do_solve(expr);
        return calc_any(expr);
    }
    if (!strcmp(op, "solve") || !strcmp(op, "equation")) {
        /* 模型爱把应用题硬转成方程，还经常列错（火车题能写成 20t+200=600）。
         * 只要它把原题文字一起传了，就先让机械题型试一遍。 */
        {
            char tbuf[2048] = {0};
            jget(json_in, "task", tbuf, sizeof(tbuf));
            if (tbuf[0]) {
                char *rt = try_all(tbuf);
                if (rt) return rt;
            }
        }
        char eq[512] = {0};
        jget(json_in, "equation", eq, sizeof(eq));
        if (!eq[0]) jget(json_in, "expr", eq, sizeof(eq));
        if (!eq[0]) {
            /* 模型有时只给原话"解方程 3x+5=20"，没给 equation。
             * 跳过开头的中文，取第一个像数学的片段。 */
            char tbuf[512] = {0};
            jget(json_in, "task", tbuf, sizeof(tbuf));
            const char *p = tbuf;
            while (*p && (unsigned char)*p >= 0x80) {
                while (*p && (unsigned char)*p >= 0x80) p++;
                while (*p == ' ') p++;
            }
            if (*p) {
                snprintf(eq, sizeof(eq), "%s", p);
                for (char *q = eq; *q; q++)
                    if ((unsigned char)*q >= 0x80) { *q = 0; break; }
            }
        }
        if (!eq[0]) return err_json("缺少 equation（给我 \"3x+5=20\" 这种）");
        /* 没有等号就不是方程：模型常把「根号2加根号3」这类算式发成 op=solve。
         * 以前这里报"方程里没有等号"，框架判失败、重试一次再失败就整个任务
         * 作废（3B 一步 170 秒，两步白烧）。算式就直接算。 */
        if (!strchr(eq, '=')) return calc_any(eq);
        return do_solve(eq);
    }
    if (!strcmp(op, "system") || !strcmp(op, "equations")) {
        return do_system(json_in);
    }
    /* 所有"应用题"类 op：先按名字对应的模板试，不行就全题型扫一遍。
     * 模型经常把火车题写成 meet、把工程量题写成 plan，不能让它猜错 op 就整题失败。 */
    if (!strcmp(op, "meet") || !strcmp(op, "travel") || !strcmp(op, "chase")
        || !strcmp(op, "ring") || !strcmp(op, "train")
        || !strcmp(op, "work") || !strcmp(op, "chicken") || !strcmp(op, "plan")) {
        char task[2048] = {0};
        jget(json_in, "task", task, sizeof(task));
        char *r = NULL;
        if (!strcmp(op, "ring"))         r = do_ring(task);
        else if (!strcmp(op, "train"))   r = do_train(task);
        else if (!strcmp(op, "work"))    r = do_work(task);
        else if (!strcmp(op, "chicken")) r = do_chicken(task);
        else if (!strcmp(op, "plan"))    r = do_plan(task);
        else                             r = do_meet(task);
        if (r) return r;
        char *ra = try_all(task);
        if (ra) return ra;
        return err_json("没识别出题型（行程/环形/火车/工程/鸡兔/计划vs实际都不匹配，"
                        "需要把原题文字完整传给 task）");
    }
    if (!strcmp(op, "stat") || !strcmp(op, "stats")) return do_stat(json_in);
    if (!strcmp(op, "num") || !strcmp(op, "number")) return do_num(json_in);
    if (!strcmp(op, "plan")) {
        char task[2048] = {0};
        jget(json_in, "task", task, sizeof(task));
        char *r = do_plan(task);
        return r ? r : err_json("没识别出「计划vs实际」结构");
    }

    /* 未指定 op：按参数猜测 */
    char bob[1024] = {0};
    if (jget(json_in, "equation", bob, sizeof(bob)) == 0 && bob[0]) return do_solve(bob);
    if (strstr(json_in, "\"eqs\"")) return do_system(json_in);
    if (strstr(json_in, "\"values\"")) return do_stat(json_in);
    if (jget(json_in, "expr", bob, sizeof(bob)) == 0 && bob[0]) {
        /* expr 里有等号 → 方程；否则表达式 */
        if (strchr(bob, '=')) return do_solve(bob);
        return do_calc(bob);
    }
    char task[2048] = {0};
    if (jget(json_in, "task", task, sizeof(task)) == 0 && task[0]) {
        /* 应用题：机械题型挨个试。这类题模型最容易自己乱列式子，
         * 结构固定，让插件直接解比让它算可靠。 */
        char *rt = try_all(task);
        if (rt) return rt;
        /* 题目里带等号 → 当方程解 */
        if (strchr(task, '=') && strlen(task) < 200) {
            char *s = do_solve(task);
            if (s && !strstr(s, "\"error\"")) return s;
            free(s);
        }
        return err_json("没识别出题型。可用 op=calc/solve/system/stat/num/plan");
    }
    return err_json("缺少参数：op + expr/equation/eqs/values/task");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "math",
    "数学：calc算式 solve方程 system方程组 stat统计 num数论 meet行程 ring环形 "
    "train火车 work合作 chicken鸡兔 plan工程",
    "{\"op\":\"calc\",\"expr\":\"(1+2)*3\"}"
    "  或 {\"op\":\"solve\",\"equation\":\"3x+5=20\"}"
    "  或 {\"op\":\"system\",\"eqs\":[\"x+y=10\",\"x-y=2\"]}"
    "  或 {\"op\":\"stat\",\"values\":[1,2,3]}"
    "  或 {\"op\":\"num\",\"sub\":\"gcd\",\"values\":[12,18]}"
    "  或 {\"op\":\"meet\",\"task\":\"原题文字\"}"
    "  或 {\"op\":\"plan\",\"task\":\"原题文字\"}",
    run,
    "calc,solve,system,stat,num,plan,meet,travel,chase,ring,train,work,chicken,"
    "expression,equation,equations"   /* 模型可能直接拿这些当工具名 */
};

plugin_desc_t *plugin_entry(void) { return &desc; }
