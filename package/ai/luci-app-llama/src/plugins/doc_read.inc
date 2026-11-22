/* ============================================================
 * doc_read.inc — 文档读取的纯 C 实现（不依赖 Python）
 *
 * docx / xlsx / pptx 本质是 ZIP 包里的 XML，PDF 是带 Flate 压缩流
 * 的格式，两者都能用 zlib 自己啃下来。
 *
 * 对外只有两个函数：
 *   doc_read_zip(path, entry, wildcard)  → 取 zip 内某个条目的内容
 *   doc_extract(path, ext)               → 按扩展名抽出纯文本
 * ============================================================ */

#include <zlib.h>
/* ---------------- 小工具 ---------------- */

typedef struct { char *p; size_t len, cap; } buf_t;

static int buf_init(buf_t *b, size_t cap) {
    memset(b, 0, sizeof(*b));
    b->p = malloc(cap);
    if (!b->p) return -1;
    b->p[0] = 0;
    b->cap = cap;
    return 0;
}
static void buf_grow(buf_t *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + need + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) return;
    b->p = np;
    b->cap = nc;
}
static void buf_putb(buf_t *b, char c) {
    buf_grow(b, 1);
    if (!b->p || b->len + 2 > b->cap) return;
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}
static void buf_putn(buf_t *b, const char *s, size_t n) {
    buf_grow(b, n);
    if (!b->p || b->len + n + 1 > b->cap) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_puts(buf_t *b, const char *s) { buf_putn(b, s, strlen(s)); }
static char *buf_detach(buf_t *b) { char *r = b->p; b->p = NULL; b->len = b->cap = 0; return r; }
static void buf_free(buf_t *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* 在二进制缓冲里找子串（strstr 遇 \0 会停，memmem 又依赖 _GNU_SOURCE） */
static const char *find_bytes(const char *hay, size_t hlen,
                              const char *needle, size_t nlen) {
    if (!hay || !needle || nlen == 0 || hlen < nlen) return NULL;
    size_t last = hlen - nlen;
    for (size_t i = 0; i <= last; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
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


/* ---------------- ZIP ---------------- */

/* 小端读取 */
static unsigned zip_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned long zip_u32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* 解压 raw deflate。usize=0 表示大小未知 → 边解边扩容。
 * PDF 的流长度常写成 /Length 9 0 R（间接引用），解压后多大完全不知道，
 * 猜缓冲会截断内容（实测整份 PDF 一个字符都抽不出来）。 */
static char *inflate_raw(const unsigned char *src, size_t slen, size_t usize) {
    if (slen == 0) return NULL;
    size_t cap = usize ? usize : (slen * 6 + 8192);
    char *out = malloc(cap + 1);
    if (!out) return NULL;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    /* 两种格式都要认：
     *   ZIP 里的条目是 raw deflate（无头）→ -MAX_WBITS
     *   PDF 里的流通常带 zlib 头（0x78 0x9c）→ MAX_WBITS
     * 用错一个就一个字节都解不出来（实测 PDF 全空）。 */
    int wbits = (slen > 1 && src[0] == 0x78) ? MAX_WBITS : -MAX_WBITS;
    if (inflateInit2(&zs, wbits) != Z_OK) { free(out); return NULL; }
    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)slen;
    size_t total = 0;
    for (;;) {
        if (total >= cap) {                     /* 满了就翻倍 */
            size_t nc = cap * 2;
            char *np = realloc(out, nc + 1);
            if (!np) { free(out); inflateEnd(&zs); return NULL; }
            out = np;
            cap = nc;
        }
        zs.next_out = (Bytef *)(out + total);
        zs.avail_out = (uInt)(cap - total);
        int rc = inflate(&zs, Z_NO_FLUSH);
        total = cap - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR) break;           /* 输入耗尽 */
        if (rc != Z_OK) { free(out); inflateEnd(&zs); return NULL; }
    }
    inflateEnd(&zs);
    out[total] = 0;
    return out;
}

/* 从 zip 里读一个条目。want 精确匹配名字；want=NULL 时用 suffix 后缀匹配。
 * 返回 malloc 的文本（已解压），失败 NULL。 */
static char *doc_read_zip(const char *zpath, const char *want, const char *suffix) {
    FILE *f = fopen(zpath, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 200L * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);

    char *result = NULL;
    /* 暴扫本地文件头 PK\x03\x04：比走中央目录简单，且能容忍一些不规范写法 */
    for (long i = 0; i + 30 < sz; i++) {
        if (!(buf[i] == 'P' && buf[i+1] == 'K' && buf[i+2] == 3 && buf[i+3] == 4)) continue;
        unsigned method  = zip_u16(buf + i + 8);
        unsigned long csz = zip_u32(buf + i + 18);
        unsigned long usz = zip_u32(buf + i + 22);
        unsigned nlen = zip_u16(buf + i + 26);
        unsigned elen = zip_u16(buf + i + 28);
        if (i + 30 + (long)nlen > sz) break;
        const char *name = (const char *)(buf + i + 30);
        long datapos = i + 30 + nlen + elen;
        if (datapos > sz) break;

        int hit = 0;
        if (want) {
            hit = ((unsigned)strlen(want) == nlen && !memcmp(name, want, nlen));
        } else if (suffix) {
            size_t sl = strlen(suffix);
            hit = (nlen >= sl && !memcmp(name + nlen - sl, suffix, sl));
        }
        if (!hit) continue;
        if (datapos + (long)csz > sz) break;

        if (method == 0) {                      /* stored */
            char *o = malloc((size_t)csz + 1);
            if (o) { memcpy(o, buf + datapos, (size_t)csz); o[csz] = 0; result = o; }
        } else if (method == 8) {               /* deflate */
            result = inflate_raw(buf + datapos, (size_t)csz, (size_t)usz);
        }
        if (result) break;
    }
    free(buf);
    return result;
}

/* ---------------- XML 辅助 ---------------- */

/* 把 &amp; &lt; &gt; &quot; &apos; &#NNN; 还原成字节 */
static void xml_unescape_into(const char *s, size_t n, buf_t *b) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { buf_putb(b, s[i]); continue; }
        size_t rest = n - i;
        if (rest > 5 && !memcmp(s + i, "&amp;", 5))  { buf_putb(b, '&');  i += 4; }
        else if (rest > 4 && !memcmp(s + i, "&lt;", 4))  { buf_putb(b, '<');  i += 3; }
        else if (rest > 4 && !memcmp(s + i, "&gt;", 4))  { buf_putb(b, '>');  i += 3; }
        else if (rest > 6 && !memcmp(s + i, "&quot;", 6)){ buf_putb(b, '"');  i += 5; }
        else if (rest > 6 && !memcmp(s + i, "&apos;", 6)){ buf_putb(b, '\''); i += 5; }
        else if (rest > 3 && s[i+1] == '#') {
            /* &#NNN; 或 &#xHH; */
            size_t j = i + 2;
            unsigned cp = 0;
            int hex = 0;
            if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
            size_t st = j;
            while (j < n && s[j] != ';' && j - st < 8) {
                int d;
                if (s[j] >= '0' && s[j] <= '9') d = s[j] - '0';
                else if (hex && s[j] >= 'a' && s[j] <= 'f') d = s[j] - 'a' + 10;
                else if (hex && s[j] >= 'A' && s[j] <= 'F') d = s[j] - 'A' + 10;
                else break;
                cp = cp * (hex ? 16 : 10) + (unsigned)d;
                j++;
            }
            if (j < n && s[j] == ';' && cp) {
                char u8[4];
                int k = utf8_emit(u8, cp);
                for (int q = 0; q < k; q++) buf_putb(b, u8[q]);
                i = j;
            } else buf_putb(b, '&');
        } else buf_putb(b, '&');
    }
}

/* 抽出所有 <tag ...>内容</tag> 里的文本（tag 传 "w:t" 这种） */
static void xml_collect_tag_lim(const char *xml, const char *stop, const char *tag,
                                buf_t *out, int newline_each) {
    char open[64], close[64];
    snprintf(open,  sizeof(open),  "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    size_t ol = strlen(open), cl = strlen(close);
    const char *p = xml;
    while ((p = strstr(p, open)) != NULL) {
        if (stop && p >= stop) break;
        /* <w:t 或 <w:t> 都算，但 <w:tbl 不算 */
        char after = p[ol];
        if (after != '>' && after != ' ' && after != '\t' && after != '\n') { p += ol; continue; }
        const char *gt = strchr(p, '>');
        if (!gt) break;
        const char *end = strstr(gt + 1, close);
        if (!end) break;
        if (stop && end >= stop) break;
        xml_unescape_into(gt + 1, (size_t)(end - gt - 1), out);
        if (newline_each) buf_putb(out, '\n');
        p = end + cl;
    }
}
static void xml_collect_tag(const char *xml, const char *tag, buf_t *out, int newline_each) {
    xml_collect_tag_lim(xml, NULL, tag, out, newline_each);
}

/* ---------------- 各格式 ---------------- */

/* docx: word/document.xml 里的 <w:t>，段落 <w:p> 之间换行 */
static char *doc_read_docx(const char *path) {
    char *xml = doc_read_zip(path, "word/document.xml", NULL);
    if (!xml) return NULL;

    buf_t b;
    if (buf_init(&b, 1 << 16) != 0) { free(xml); return NULL; }

    /* 逐个段落取：找到 <w:p ...> 和它对应的 </w:p>，只在这一段里收集 <w:t>。
     * 不限定范围的话，第一个 <w:p> 就会把全文扫一遍，内容重复好几遍。 */
    const char *p = xml;
    while (*p) {
        const char *ps = strstr(p, "<w:p");
        if (!ps) break;
        /* 跳过 <w:pPr> 这类 —— 要求紧跟 '>' 或空白才算段落开始 */
        char nc = ps[4];
        if (nc != '>' && nc != ' ' && nc != '\t' && nc != '\n') { p = ps + 4; continue; }
        const char *pe = strstr(ps, "</w:p>");
        if (!pe) break;
        xml_collect_tag_lim(ps, pe, "w:t", &b, 0);
        buf_putb(&b, '\n');
        p = pe + 6;
    }
    free(xml);
    buf_putb(&b, 0);
    return buf_detach(&b);
}

/* xlsx: sharedStrings.xml 存字符串池，sheet1.xml 里的 <c t="s"><v>索引</v> */
static char *doc_read_xlsx(const char *path) {
    char *sst = doc_read_zip(path, "xl/sharedStrings.xml", NULL);
    char *sheet = doc_read_zip(path, NULL, "worksheets/sheet1.xml");
    if (!sheet) sheet = doc_read_zip(path, NULL, "worksheets/sheet.xml");
    if (!sheet) { free(sst); return NULL; }

    /* 先把共享字符串按顺序抽成数组 */
    char **strs = NULL;
    int ns = 0, cap = 0;
    if (sst) {
        const char *p = sst;
        while ((p = strstr(p, "<si>")) != NULL) {
            const char *e = strstr(p, "</si>");
            if (!e) break;
            buf_t tb;
            if (buf_init(&tb, 256) == 0) {
                /* 一个 si 里可能有多个 <t>（富文本分片），拼起来 */
                char save = *e;
                *(char *)e = 0;
                xml_collect_tag(p, "t", &tb, 0);
                *(char *)e = save;
                buf_putb(&tb, 0);
                if (ns >= cap) {
                    cap = cap ? cap * 2 : 256;
                    char **nn = realloc(strs, (size_t)cap * sizeof(char *));
                    if (!nn) break;
                    strs = nn;
                }
                strs[ns++] = buf_detach(&tb);
            }
            p = e + 5;
        }
        free(sst);
    }

    buf_t b;
    if (buf_init(&b, 1 << 16) != 0) {
        for (int i = 0; i < ns; i++) free(strs[i]);
        free(strs); free(sheet);
        return NULL;
    }

    const char *p = sheet;
    while ((p = strstr(p, "<c ")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        const char *ve = strstr(gt, "</c>");
        if (!ve) break;
        int is_shared  = (strstr(p, "t=\"s\"") && strstr(p, "t=\"s\"") < gt);
        int is_inline  = (strstr(p, "t=\"inlineStr\"") && strstr(p, "t=\"inlineStr\"") < gt);

        if (is_inline) {
            /* openpyxl 等写的短字符串是内联的：<is><t>文本</t></is>，
             * 不走 sharedStrings（实测 openpyxl 根本不生成那个文件） */
            xml_collect_tag_lim(gt, ve, "t", &b, 0);
        } else {
            const char *v = strstr(gt, "<v>");
            if (v && v < ve) {
                const char *vend = strstr(v, "</v>");
                if (vend) {
                    if (is_shared) {
                        long idx = strtol(v + 3, NULL, 10);
                        if (idx >= 0 && idx < ns && strs[idx]) buf_puts(&b, strs[idx]);
                    } else {
                        buf_putn(&b, v + 3, (size_t)(vend - v - 3));
                    }
                }
            }
        }
        /* 一行（<row>）结束换行 */
        const char *rowe = strstr(gt, "</row>");
        if (rowe && rowe < ve) { buf_putb(&b, '\n'); p = rowe + 6; }
        else { buf_putb(&b, '\t'); p = ve + 4; }
    }

    for (int i = 0; i < ns; i++) free(strs[i]);
    free(strs);
    free(sheet);
    buf_putb(&b, 0);
    return buf_detach(&b);
}

/* pptx: ppt/slides/slideN.xml 里的 <a:t> */
static char *doc_read_pptx(const char *path) {
    buf_t b;
    if (buf_init(&b, 1 << 16) != 0) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) { buf_free(&b); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); buf_free(&b); return NULL; }
    long sz = ftell(f);
    rewind(f);
    unsigned char *raw = malloc((size_t)sz + 1);
    if (!raw) { fclose(f); buf_free(&b); return NULL; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    raw[got] = 0;
    fclose(f);

    /* 从 zip 中央目录里数出 slide 数量，逐个读（slide1..slideN） */
    for (int n = 1; n <= 500; n++) {
        char entry[64];
        snprintf(entry, sizeof(entry), "ppt/slides/slide%d.xml", n);
        char *xml = doc_read_zip(path, entry, NULL);
        if (!xml) break;
        buf_puts(&b, "\n--- 第 ");
        char num[16];
        snprintf(num, sizeof(num), "%d", n);
        buf_puts(&b, num);
        buf_puts(&b, " 页 ---\n");
        xml_collect_tag(xml, "a:t", &b, 1);
        free(xml);
    }
    free(raw);
    buf_putb(&b, 0);
    return buf_detach(&b);
}

/* PDF：暴力扫 stream ... endstream，FlateDecode 的解压后找 Tj/TJ。
 * 只抽字面量和十六进制字符串，不做 CID 映射 —— 中文 PDF 里
 * 有 ToUnicode 的能出字，没有的会出乱码（和 pypdf 的行为一致）。 */
/* 纯文本：整份读进来（限 8MB，超了截断并标注） */
static char *doc_read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    long lim = sz > 8L * 1024 * 1024 ? 8L * 1024 * 1024 : sz;
    rewind(f);
    char *t = malloc((size_t)lim + 64);
    if (!t) { fclose(f); return NULL; }
    size_t got = fread(t, 1, (size_t)lim, f);
    fclose(f);
    t[got] = 0;
    if (got == 0) { free(t); return NULL; }
    return t;
}

/* 总入口：按扩展名分派；不认识的格式返回 NULL（交给纯文本路径） */
static char *doc_extract(const char *path, const char *dot) {
    if (!dot || !dot[0]) return NULL;
    if (!strcasecmp(dot, ".docx")) return doc_read_docx(path);
    if (!strcasecmp(dot, ".xlsx") || !strcasecmp(dot, ".xlsm")) return doc_read_xlsx(path);
    if (!strcasecmp(dot, ".pptx")) return doc_read_pptx(path);
    return NULL;
}
