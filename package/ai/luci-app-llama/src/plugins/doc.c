/* doc.c — 文档解析插件：PDF / Word / Excel / PPT / 文本 → 纯文本
 *
 * 为什么独立成插件：这一路完全依赖外部运行时（python3 + pypdf +
 * python-docx + openpyxl + python-pptx）。环境里缺哪个库，这个插件就干不了
 * 对应的活 —— 放在框架里会让整个 agent 显得"上传坏了"，拆出来就能明确报
 * "缺 pypdf，PDF 转不了"，环境面板也能按库点亮。
 *
 * 入参：{"path":"/tmp/llama_agent/uploads/report.pdf"}
 * 输出：{"text":"...","chars":1234} 或 {"error":"缺少 pypdf，PDF 无法解析"}
 */
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "plugin.h"
#include "py_write.h"       /* 内嵌的文档写入脚本（由 docwrite.py 生成） */
#include "doc_read.inc"     /* 纯 C 读取：zip+xml */

#define OUTMAX 200000

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
        if (*p == '\\' && p[1]) { p++; out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

/* 跑一条命令，收集 stdout */
static char *sh(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char *buf = malloc(OUTMAX);
    if (!buf) { pclose(p); return NULL; }
    size_t n = fread(buf, 1, OUTMAX - 1, p);
    buf[n] = 0;
    pclose(p);
    return buf;
}

/* 该后缀需要哪个 python 库 */
static const char *need_lib(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    if (!strcasecmp(dot, ".pdf")) return "pypdf";
    if (!strcasecmp(dot, ".docx")) return "docx";
    if (!strcasecmp(dot, ".xlsx") || !strcasecmp(dot, ".xls")) return "openpyxl";
    if (!strcasecmp(dot, ".pptx")) return "pptx";
    return NULL;                      /* .txt/.md/.csv 不需要库 */
}

static char *json_escape(const char *in, size_t *outlen) {
    size_t cap = strlen(in) * 6 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = 0;
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { o[i++] = '\\'; o[i++] = (char)c; }
        else if (c == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else if (c == '\r') { o[i++] = '\\'; o[i++] = 'r'; }
        else if (c == '\t') { o[i++] = '\\'; o[i++] = 't'; }
        else if (c < 0x20) { i += (size_t)snprintf(o + i, cap - i, "\\u%04x", c); }
        else o[i++] = (char)c;
    }
    o[i] = 0;
    *outlen = i;
    return o;
}

/* ---------------- 写操作：交给 docwrite.py ----------------
 * 数据走 stdin（内容可能几千字，塞 argv 会被长度限制打断），
 * 结果从 stdout 读，stderr 也合并进去 —— python 报错能原样带回给模型。
 */
#define PY_OUT_MAX (1 << 20)

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
        /* 脚本通过 -c 传、数据通过 stdin 传，两者不冲突。
         * -c 之后第一个参数就是 sys.argv[1]，正好是 op。 */
        char *argv[] = {
            (char *)"python3",
            (char *)"-c",
            (char *)PY_WRITE_SRC,
            (char *)op,
            NULL
        };
        execv("/usr/bin/python3", argv);
        _exit(127);
    }

    close(in[0]);
    close(ou[1]);

    /* 把 JSON 写进子进程 stdin */
    size_t len = strlen(json), off = 0;
    while (off < len) {
        ssize_t w = write(in[1], json + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(in[1]);

    char *buf = malloc(PY_OUT_MAX);
    if (!buf) { close(ou[0]); waitpid(pid, NULL, 0); return NULL; }
    size_t n = 0;
    ssize_t r;
    while ((r = read(ou[0], buf + n, PY_OUT_MAX - 1 - n)) > 0) {
        n += (size_t)r;
        if (n + 1 >= PY_OUT_MAX) break;
    }
    buf[n] = 0;
    close(ou[0]);

    int st = 0;
    waitpid(pid, &st, 0);
    if (st == 127 << 8 || (WIFEXITED(st) && WEXITSTATUS(st) == 127)) {
        free(buf);
        return strdup("{\"error\":\"找不到 python3，无法执行写操作\"}");
    }
    if (!n) {
        free(buf);
        return strdup("{\"error\":\"docwrite.py 没有输出\"}");
    }
    return buf;
}

/* 支持的写操作 */
static int is_write_op(const char *op) {
    static const char *ops[] = {
        "text", "xlsx", "xlsx_edit", "xlsx_read",
        "docx", "docx_append", "pptx", "pdf", NULL
    };
    for (int i = 0; ops[i]; i++)
        if (!strcmp(op, ops[i])) return 1;
    return 0;
}

/* ---------- 图片 OCR ----------
 * 板上 /opt/ppocr 是 RKNPU 版 PP-OCRv4（ppocr_demo 用 aarch64 的可执行文件，
 * 依赖 /opt/glibc 的 loader 和 /opt/rknpu/librknnrt.so）。
 * 用法：ppocr_demo <det.rknn> <rec.rknn> <image>
 * 输出每行形如：regconize result: Hello OCRTest 12345, score=0.953433
 *
 * 这台板子是纯 CPU 跑 3B 文本模型，没有视觉模型 —— 图片只能"认字"，
 * 认完的字交给模型继续问答，这条路是可行的。 */
static char *op_ocr(const char *path) {
    struct stat st;
    if (!path[0] || stat(path, &st) != 0) {
        char m[600];
        snprintf(m, sizeof(m), "{\"error\":\"图片不存在：%s\"}", path);
        return strdup(m);
    }
    const char *loader = "/opt/glibc/ld-linux-aarch64.so.1";
    const char *demo = "/opt/ppocr/ppocr_demo";
    if (access(demo, X_OK) != 0)
        return strdup("{\"error\":\"板上没有 OCR 引擎（/opt/ppocr/ppocr_demo）。"
                      "这是 RK3566 的 RKNPU 版 PP-OCRv4，缺了就没法识别图片文字。\"}");

    char cmd[2048];
    if (access(loader, X_OK) == 0)
        snprintf(cmd, sizeof(cmd),
                 "cd /opt/ppocr && LD_LIBRARY_PATH=/opt/glibc:/opt/rknpu:/usr/lib:/lib:/opt/ppocr "
                 "%s ./ppocr_demo models/ppocrv4_det.rknn models/ppocrv4_rec.rknn '%s' 2>/dev/null",
                 loader, path);
    else
        snprintf(cmd, sizeof(cmd),
                 "cd /opt/ppocr && ./ppocr_demo models/ppocrv4_det.rknn models/ppocrv4_rec.rknn '%s' 2>/dev/null",
                 path);

    FILE *pp = popen(cmd, "r");
    if (!pp) return strdup("{\"error\":\"OCR 引擎启动失败\"}");

    size_t cap = 1 << 16, n = 0;
    char *text = malloc(cap);
    if (!text) { pclose(pp); return NULL; }
    text[0] = 0;
    int lines = 0;
    char line[1024];
    const char *KEY = "regconize result: ";
    while (fgets(line, sizeof(line), pp)) {
        char *k = strstr(line, KEY);
        if (!k) continue;
        k += strlen(KEY);
        char *sc = strstr(k, ", score=");
        if (!sc) continue;
        double score = atof(sc + strlen(", score="));
        *sc = 0;
        /* 去掉首尾空白 */
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); e--) e[-1] = 0;
        if (!k[0] || score < 0.45) continue;
        size_t L = strlen(k);
        if (n + L + 4 >= cap) {
            cap *= 2;
            char *nt = realloc(text, cap);
            if (!nt) break;
            text = nt;
        }
        memcpy(text + n, k, L); n += L;
        text[n++] = '\n';
        lines++;
    }
    pclose(pp);
    text[n] = 0;

    if (!text[0]) {
        free(text);
        return strdup("{\"text\":\"\",\"lines\":0,\"chars\":0,"
                      "\"answer\":\"这张图里没认出文字（可能是纯图片，也可能是字太小/太花）\"}");
    }
    /* JSON 转义 */
    size_t jc = n * 2 + 512;
    char *jt = malloc(jc), *res = malloc(jc + 512);
    if (!jt || !res) { free(jt); free(res); free(text); return NULL; }
    size_t jo = 0;
    for (const char *q = text; *q && jo + 8 < jc; q++) {
        if (*q == '"' || *q == '\\') { jt[jo++] = '\\'; jt[jo++] = *q; }
        else if (*q == '\n') { jt[jo++] = '\\'; jt[jo++] = 'n'; }
        else if ((unsigned char)*q < 0x20) jt[jo++] = ' ';
        else jt[jo++] = *q;
    }
    jt[jo] = 0;
    snprintf(res, jc + 512,
             "{\"text\":\"%s\",\"lines\":%d,\"chars\":%d,\"engine\":\"PP-OCRv4 @ RKNPU\","
             "\"answer\":\"认出 %d 行文字，共 %d 字\"}",
             jt, lines, (int)n, lines, (int)n);
    free(jt);
    free(text);
    return res;
}

static char *run(const char *json_in) {
    char op[32] = {0};
    char path[512] = {0};
    jget(json_in, "op", op, sizeof(op));
    jget(json_in, "path", path, sizeof(path));
    /* 模型常把路径写成 file / target / output / filename 而不是 path */
    if (!path[0]) {
        static const char *PK[] = {"file", "target", "output", "out_path", "filename", NULL};
        for (int i = 0; PK[i] && !path[0]; i++) jget(json_in, PK[i], path, sizeof(path));
    }
    /* op 为空但明显是在"生成"（带了正文/表格/页）：按内容形状推断写操作，
     * 别落到下面报"缺少 path"。纯 {"path":...} 仍然是读（见下面注释）。 */
    if (!op[0] && !path[0]) {
        if (strstr(json_in, "\"pages\"") || strstr(json_in, "\"slides\""))
            snprintf(op, sizeof(op), "pptx");
        else if (strstr(json_in, "\"sheets\"") || strstr(json_in, "\"csv_files\"")
                 || strstr(json_in, "\"csv\""))
            snprintf(op, sizeof(op), "xlsx");
        else if (strstr(json_in, "\"markdown\"") || strstr(json_in, "\"content\"")
                 || strstr(json_in, "\"text\""))
            snprintf(op, sizeof(op), "docx");
    }

    /* 模型常把"写一个 ppt"发成 op=write / create / new / 生成，或者给了
     * path=xxx.pptx 却不给 op。这些都不是读操作 —— 按扩展名/format 归到
     * 对应的写操作。以前它们全落到下面的读分支，统一报"缺少 path"，
     * 模型看不懂，原样重发两次就被框架判"连续失败"停了。 */
    /* 注意：op 为空表示"读这个文件"（插件文档里的读法就是 {"path":...}），
     * 不能按扩展名猜成写操作 —— 那样上传 xlsx 会被当成"新建 xlsx"，
     * 把刚存好的上传文件覆盖成一张空表（表现：上传成功但 chars=15、
     * 伴生 .txt 没生成）。只有明确写了 write/create/… 才按扩展名归到写。 */
    if (op[0] && (!strcmp(op, "write") || !strcmp(op, "create") || !strcmp(op, "new")
        || !strcmp(op, "gen") || !strcmp(op, "生成") || !strcmp(op, "输出")
        || !strcmp(op, "out"))) {
        char fmt[16] = {0};
        jget(json_in, "format", fmt, sizeof(fmt));
        const char *e = path[0] ? strrchr(path, '.') : NULL;
        if (e && !strcasecmp(e, ".pptx"))      snprintf(op, sizeof(op), "pptx");
        else if (e && !strcasecmp(e, ".docx")) snprintf(op, sizeof(op), "docx");
        else if (e && !strcasecmp(e, ".pdf"))  snprintf(op, sizeof(op), "pdf");
        else if (e && (!strcasecmp(e, ".xlsx") || !strcasecmp(e, ".xlsm")))
            snprintf(op, sizeof(op), "xlsx");
        else if (e && (!strcasecmp(e, ".txt") || !strcasecmp(e, ".md")))
            snprintf(op, sizeof(op), "text");
        else if (fmt[0]) snprintf(op, sizeof(op), "%s", fmt);
    }

    /* 写操作没给 path：路径由框架定，别拿"缺少 path"把任务做废。
     * 放在 /tmp/llama_agent/gen/ 下（页面 ?file= 能取到、也能下载）。 */
    if (op[0] && is_write_op(op) && !path[0]) {
        const char *ext = !strcmp(op, "pptx") ? "pptx"
                        : !strcmp(op, "docx") || !strcmp(op, "docx_append") ? "docx"
                        : !strcmp(op, "pdf") ? "pdf"
                        : !strcmp(op, "text") ? "txt" : "xlsx";
        mkdir("/tmp/llama_agent", 0777);
        mkdir("/tmp/llama_agent/gen", 0777);
        snprintf(path, sizeof(path), "/tmp/llama_agent/gen/%s_%ld.%s",
                 op, (long)time(NULL), ext);
        /* 补出来的 path/op 追加到原 JSON 末尾 —— Python 的 json 遇到重复键
         * 取后一个，所以顺序必须是"原来的一坨, path, op}。 */
        size_t L = strlen(json_in);
        size_t n = L + sizeof(path) + 128;
        char *nj = malloc(n);
        if (nj) {
            if (L > 1 && json_in[0] == '{') {
                size_t strip = (json_in[L - 1] == '}') ? 1 : 0;
                snprintf(nj, n, "%.*s,\"path\":\"%s\",\"op\":\"%s\"}",
                         (int)(L - strip), json_in, path, op);
            } else {
                snprintf(nj, n, "{\"op\":\"%s\",\"path\":\"%s\"}", op, path);
            }
            char *r = run_py(op, nj);
            free(nj);
            return r;
        }
    }

    /* 写操作：fork 出 python3，脚本从 py_write.h 内嵌的源码走 -c 传进去，
     * 数据走 stdin。板上不需要任何 .py 文件，只要 python3 + 相应用库。 */
    if (op[0] && is_write_op(op)) {
        return run_py(op, json_in);
    }

    /* 图片：交给 RKNPU 上的 PP-OCRv4 认字 */
    if (!strcmp(op, "ocr") || !strcmp(op, "image") || !strcmp(op, "img2txt")) {
        if (!path[0]) {
            /* 没给 path 就用最近上传的图片 */
            DIR *d = opendir("/tmp/llama_agent/uploads");
            if (d) {
                struct dirent *e;
                struct stat st2;
                time_t best = 0;
                char cand[900] = {0};
                while ((e = readdir(d)) != NULL) {
                    const char *x = strrchr(e->d_name, '.');
                    if (!x) continue;
                    if (strcasecmp(x, ".jpg") && strcasecmp(x, ".jpeg") && strcasecmp(x, ".png")
                        && strcasecmp(x, ".bmp") && strcasecmp(x, ".webp")) continue;
                    char fp[900];
                    snprintf(fp, sizeof(fp), "/tmp/llama_agent/uploads/%s", e->d_name);
                    if (stat(fp, &st2) == 0 && st2.st_mtime >= best) {
                        best = st2.st_mtime;
                        snprintf(cand, sizeof(cand), "%s", fp);
                    }
                }
                closedir(d);
                if (cand[0]) snprintf(path, sizeof(path), "%s", cand);
            }
        }
        if (!path[0])
            return strdup("{\"error\":\"没有图片可识别：先上传一张图，或给 path=图片路径\"}");
        return op_ocr(path);
    }

    if (!path[0])
        return strdup("{\"error\":\"缺少 path：读文档要给 path；生成文档要用 "
                      "op=xlsx/docx/pptx/pdf+markdown\"}");

    /* docx / xlsx / pptx 用纯 C 解（zip + xml，实测验证过）。
     *
     * PDF 例外：它要解析对象树 → 每页资源 → 字体 /Subtype → 各自的
     * ToUnicode CMap → 文本流里 /F1 Tf 切换编码。子集嵌入字体 + 混合
     * 编码的真实 PDF 上，几百行的 C 实现还原不出文字（pypdf 是几千行）。
     * 这里直接用 pypdf，脚本运行时生成，不再依赖 /opt/llama/agent/extract.py。 */
    const char *dot = strrchr(path, '.');
    char *txt = NULL;

    if (dot && !strcasecmp(dot, ".pdf")) {
        const char *sp = "/tmp/.doc_pdf.py";
        FILE *sf = fopen(sp, "w");
        if (sf) {
            fputs("import sys\n"
                  "try:\n"
                  "    import pypdf\n"
                  "except ImportError:\n"
                  "    sys.stderr.write('need pypdf\\n')\n"
                  "    sys.exit(2)\n"
                  "r = pypdf.PdfReader(sys.argv[1])\n"
                  "out = []\n"
                  "for pg in r.pages:\n"
                  "    t = pg.extract_text()\n"
                  "    if t:\n"
                  "        out.append(t)\n"
                  "sys.stdout.write(chr(10).join(out))\n", sf);
            fclose(sf);
            char cmd[700];
            /* 不要把 stderr 全吞掉：pypdf 真报错（比如 ImportError）时
             * 用户看到的永远是"没装 pypdf"，实际是别的错，没法排查。 */
            snprintf(cmd, sizeof(cmd), "python3 %s '%s'", sp, path);
            txt = sh(cmd);
        }
        if (!txt || !txt[0]) {
            free(txt);
            /* pypdf 已装但提取为空，最常见是 PDF 本身没有文本层（图片版/
             * 扫描件）。以前这里一律说"没装 pypdf"，把人带偏。 */
            return strdup("{\"error\":\"PDF 读不出文字：这份 PDF 大概率没有文本层"
                          "（扫描件或图片版，需要 OCR 才能读）。文字版 PDF 才走 pypdf\"}");
        }
    } else {
        txt = doc_extract(path, dot);
        if (!txt) {
            if (dot && (!strcasecmp(dot, ".docx") || !strcasecmp(dot, ".xlsx")
                        || !strcasecmp(dot, ".xlsm") || !strcasecmp(dot, ".pptx")))
                return strdup("{\"error\":\"这个文档读不出内容（结构损坏或格式变种）\"}");
            txt = doc_read_text(path);        /* 纯文本 / 不认识的格式按文本读 */
        }
    }
    if (!txt) return strdup("{\"error\":\"读不出内容（文件为空或损坏）\"}");

    /* 3. 正常：包成 JSON 返回 */
    size_t n = 0;
    char *esc = json_escape(txt, &n);
    size_t chars = strlen(txt);
    free(txt);
    if (!esc) return NULL;
    char *o = malloc(n + 96);
    if (!o) { free(esc); return NULL; }
    snprintf(o, n + 96, "{\"text\":\"%s\",\"chars\":%zu}", esc, chars);
    free(esc);
    return o;
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "doc",
    "文档读写。读：PDF/Word/Excel/PPT/文本 → 纯文本；"
    "写：生成或修改 docx/xlsx/pptx/pdf/纯文本（依赖 python3 + 对应库，缺库会明确报错）",
    "读：{\"path\":\"/tmp/llama_agent/a.pdf\"}"
    "  ｜写纯文本 {\"op\":\"text\",\"path\":\"/tmp/llama_agent/a.txt\",\"content\":\"内容\"}"
    "  ｜写 Excel {\"op\":\"xlsx\",\"path\":\"/tmp/llama_agent/a.xlsx\","
    "\"csv\":\"产品,销量\\n产品A,100\\n产品B,200\"}     ← 用 csv 字段，别拼嵌套数组"
    "  ｜改单元格 {\"op\":\"xlsx_edit\",\"path\":\"...xlsx\",\"sheet\":\"S1\",\"cell\":\"B2\",\"value\":123}"
    "  ｜写 Word {\"op\":\"docx\",\"path\":\"/tmp/llama_agent/a.docx\","
    "\"markdown\":\"# 标题\\n正文段落\\n- 要点\"}       ← 用 markdown 字段"
    "  ｜追加 Word {\"op\":\"docx_append\",\"path\":\"...docx\",\"markdown\":\"...\"}"
    "  ｜写 PPT {\"op\":\"pptx\",\"path\":\"/tmp/llama_agent/a.pptx\","
    "\"markdown\":\"# 第一页\\n- 要点A\\n- 要点B\\n# 第二页\\n- 要点C\"}"
    "  ｜写 PDF {\"op\":\"pdf\",\"path\":\"/tmp/llama_agent/a.pdf\",\"markdown\":\"# 标题\\n正文\"}",
    run,
    "doc,docx,xlsx,pptx,pdf,read,write,文档,生成,导出"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
