/* voice.c — 离线中文语音合成（调用 tts-rk3566 的 /usr/bin/tts）
 *
 * op: tts / speak / say
 *   {"op":"tts","text":"要念的内容","speed":1.0,"bitrate":"64k","out":"/tmp/a.mp3"}
 *   {"op":"tts","path":"/tmp/文本.txt"}      从文件读正文
 *
 * 返回：{"ok":1,"path":"/tmp/llama_agent/gen/tts_xxxx.mp3","chars":N,"secs":M,...}
 *   path 就是气泡里的下载/播放地址（CGI 的 ?file= 会给 audio/mpeg 并支持 Range）。
 *
 * 板上的 tts 是 sherpa-onnx + matcha-zh-baker，纯 CPU：
 *   首次调用要加载 26 MB 模型（实测 22 秒），所以按"文本哈希"缓存，
 *   同一句再问一次直接返回已有文件。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#include "plugin.h"

/* 前向声明：run() 里用到，定义在后面（否则隐式声明会变成外部符号，
 * 运行时 dlopen 报 "Error relocating: op_models: symbol not found"） */
static char *op_models(const char *json_in);
static char *op_model_install(const char *json_in);
static char *op_model_status(const char *json_in);
static char *op_model_remove(const char *json_in);
static char *run_asr(const char *json_in);

#define WORK_DIR "/tmp/llama_agent"
#define TTS_BIN  "/usr/bin/tts"
#define MAX_TEXT 4000          /* 一次最多合成 4000 字节正文，超了截断 */

static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p += strlen(pat); continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') { p = q; continue; }
        q++;
        size_t i = 0;
        while (*q && *q != '"' && i + 1 < cap) {
            if (*q == '\\' && q[1]) {
                q++;
                switch (*q) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '"': out[i++] = '"'; break;
                case '\\': out[i++] = '\\'; break;
                default: out[i++] = *q; break;
                }
                q++;
            } else out[i++] = *q++;
        }
        out[i] = 0;
        return 0;
    }
    out[0] = 0;
    return -1;
}

static double jget_num(const char *js, const char *key, double dflt) {
    char p[40];
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *q = strstr(js, pat);
    if (!q) return dflt;
    q += strlen(pat);
    while (*q == ' ' || *q == '\t') q++;
    if (*q != ':') return dflt;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '-' && !isdigit((unsigned char)*q)) return dflt;
    snprintf(p, sizeof(p), "%.3f", atof(q));
    return atof(p);
}

static char *jerr(const char *msg) {
    char *o = malloc(strlen(msg) + 40);
    if (!o) return NULL;
    snprintf(o, strlen(msg) + 40, "{\"error\":\"%s\"}", msg);
    return o;
}

/* JSON 字符串转义 */
static void jesc(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    for (const char *p = src; *p && i + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { dst[i++] = '\\'; dst[i++] = (char)c; }
        else if (c == '\n') { dst[i++] = '\\'; dst[i++] = 'n'; }
        else if (c == '\r') { dst[i++] = '\\'; dst[i++] = 'r'; }
        else if (c == '\t') { dst[i++] = ' '; }
        else if (c < 0x20) dst[i++] = ' ';
        else dst[i++] = (char)c;
    }
    dst[i] = 0;
}


/* matcha-zh-baker 是纯中文音素模型，遇到英文单词会念出怪声。
 * 这里把英文字母逐个换成中文近似读法（A→诶、B→必、C→西…），
 * "CPU" → "西屁优"，至少能听懂；纯英文句子也是"逐字母播报"。
 * 数字不动 —— 模型自带 number.fst 会念成中文数字。 */
static const char *EN_ZH[26] = {
    "诶","必","西","迪","伊","艾弗","吉","艾尺","爱","杰","开","艾勒","艾姆",
    "恩","欧","屁","扣","阿尔","艾斯","提","优","维","达布留","艾克斯","歪","贼德"
};

static void normalize_text(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 40 < cap; ) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            unsigned idx = (c >= 'a') ? (unsigned)(c - 'a') : (unsigned)(c - 'A');
            o += (size_t)snprintf(out + o, cap - o, "%s", EN_ZH[idx]);
            p++;
            /* 字母之间留半拍，免得连读成一句 */
            if (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
                out[o++] = ' ';
            continue;
        }
        out[o++] = (char)c;
        p++;
    }
    out[o] = 0;
}

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    for (const char *p = s; *p; p++) h = h * 33 + (unsigned char)*p;
    return h;
}



static void trim(char *s);        /* 定义在后面，切段函数要用 */
static void tidy_spaces(char *t);
static void expand_symbols(char *t, size_t cap);
static void sh_quote(const char *src, char *dst, size_t cap);   /* 同上 */


/* 空格处理：中文模型把 ASCII 空格当"音节分隔符"，一个字一个空格就会念成
 * 一堆怪音（实测"今 天 天 气 哈 哈 哈"）。规则：
 *   - 两个非 ASCII（汉字/全角标点）之间的空格直接删掉
 *   - 其余连续空白压成一个空格（英文单词之间要保留）
 */
static void tidy_spaces(char *t) {
    char *r = t, *w = t;
    int prev_cjk = 0;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            int run = 0;
            while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') { r++; run++; }
            int next_cjk = ((unsigned char)*r >= 0xE0);
            /* 开头的空格直接丢（w==t 说明还没写过东西） */
            if (run && w != t && !(prev_cjk && next_cjk)) *w++ = ' ';
            continue;
        }
        *w++ = *r;
        /* 关键：汉字是 3 字节，拷贝后紧接着的两个是"续字节"(0x80~0xBF)。
         * 以前用最后拷贝的那个字节判断 prev_cjk，拿到的永远是续字节，
         * 于是"汉字 汉字"之间的空格判不掉 —— 空格还是被当成音节分隔符。 */
        if ((c & 0xC0) == 0x80) { r++; continue; }      /* 续字节：不改 prev_cjk */
        prev_cjk = (c >= 0xE0);
        r++;
    }
    while (w > t && (w[-1] == ' ')) w--;
    *w = 0;
}

/* 长文本切段：双语模型 RTF≈5，一句 3 秒的话要算 15 秒以上，
 * 一次喂几百字会等到天荒地老，还容易中途出错整段白跑。
 * 这里按标点切成 ≤maxlen 字节的小段（不切断多字节字符）。 */
static int split_text(const char *in, char segs[][400], int maxseg, int maxlen) {
    int n = 0;
    size_t len = strlen(in);
    size_t i = 0;
    while (i < len && n < maxseg) {
        size_t start = i, last_break = 0;
        /* 从 start 往后找 maxlen 以内最后一个标点 */
        size_t j = start;
        while (j < len && j - start < (size_t)maxlen) {
            unsigned char c = (unsigned char)in[j];
            if (c == '\n') { last_break = j + 1; }
            /* 中文标点（3 字节）：。！？；，、 */
            else if (j + 2 < len && (unsigned char)in[j] == 0xE3 && (unsigned char)in[j + 1] >= 0x80) {
                if ((unsigned char)in[j + 1] == 0x80 && ((unsigned char)in[j + 2] == 0x82   /* 。 */
                    || (unsigned char)in[j + 2] == 0x81)) last_break = j + 3;              /* ！ */
                if ((unsigned char)in[j + 1] == 0x80 && (unsigned char)in[j + 2] == 0x9B)
                    last_break = j + 3;                                                    /* ？ */
            }
            else if (c == '.' || c == '!' || c == '?' || c == ';' || c == ',') last_break = j + 1;
            j++;
        }
        size_t end = (last_break > start) ? last_break : (j < len ? j : len);
        /* 不切断 UTF-8 字符 */
        while (end < len && ((unsigned char)in[end] & 0xC0) == 0x80) end++;
        size_t l = end - start;
        if (l >= sizeof(segs[0])) l = sizeof(segs[0]) - 1;
        memcpy(segs[n], in + start, l);
        segs[n][l] = 0;
        trim(segs[n]);
        tidy_spaces(segs[n]);
        if (segs[n][0]) {
            /* 只有标点/空格的段不要（喂给 tts_sherpa 会失败，整条链就断了） */
            int has = 0;
            for (const char *q = segs[n]; *q; q++)
                if (((unsigned char)*q >= 0x80) || isalnum((unsigned char)*q)) { has = 1; break; }
            if (has) n++;
        }
        i = end;
    }
    return n;
}



/* ---------- 语音模型管理（模型不进 ipk，按需装） ----------
 * 包里只带 matcha（26MB，中文女声）。可选的大模型：
 *   zh-en   vits-melo-tts-zh_en         中英双语    ~180MB
 *   kokoro  kokoro-int8-multi-lang-v1_1 男声/多音色  ~206MB
 * op=models 看装了没；op=model_install 后台下载解包；op=model_status 看进度。
 */
typedef struct { const char *name, *dir, *url; const char *post; const char *desc; } tts_model_t;

static const tts_model_t TTS_MODELS[] = {
    {"zh-en", "/opt/tts-rk3566/zh-en-model",
     "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-melo-tts-zh_en.tar.bz2",
     /* vits 路径用的是 model.onnx(fp32)，int8 那份用不上 → 装完删掉省 130MB */
     "rm -f model.int8.onnx; rm -rf espeak-ng-data; "
     "ls date.fst number.fst >/dev/null 2>&1 || cp ../matcha/*.fst .",
     "中英双语（英文真发音）"},
    {"kokoro", "/opt/tts-rk3566/kokoro",
     "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-int8-multi-lang-v1_1.tar.bz2",
     /* kokoro 只用 int8：fp32 和多余的 espeak 数据删掉，软链补齐程序要的名字 */
     "ln -sf date-zh.fst date.fst; ln -sf number-zh.fst number.fst; "
     "ln -sf model.int8.onnx model_int8.onnx; rm -f model.onnx; rm -rf espeak-ng-data",
     "男声/上百种音色（RTF≈8 较慢）"},
    {NULL, NULL, NULL, NULL, NULL}
};

static long dir_size_mb(const char *d) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "du -sm %s 2>/dev/null | cut -f1", d);
    FILE *pp = popen(cmd, "r");
    if (!pp) return 0;
    long v = 0;
    if (fscanf(pp, "%ld", &v) != 1) v = 0;
    pclose(pp);
    return v;
}

static char *op_models(const char *json_in) {
    (void)json_in;
    char *o = malloc(2048);
    if (!o) return NULL;
    size_t off = (size_t)snprintf(o, 2048, "{\"ok\":1,\"base\":\"/opt/tts-rk3566\",\"models\":[");
    long m = dir_size_mb("/opt/tts-rk3566/matcha");
    off += (size_t)snprintf(o + off, 2048 - off,
        "{\"name\":\"matcha\",\"desc\":\"中文女声（内置，最快）\",\"installed\":%d,\"mb\":%ld}",
        m > 0 ? 1 : 0, m);
    for (int i = 0; TTS_MODELS[i].name; i++) {
        long sz = dir_size_mb(TTS_MODELS[i].dir);
        off += (size_t)snprintf(o + off, 2048 - off,
            ",{\"name\":\"%s\",\"desc\":\"%s\",\"installed\":%d,\"mb\":%ld}",
            TTS_MODELS[i].name, TTS_MODELS[i].desc, sz > 20 ? 1 : 0, sz);
    }
    snprintf(o + off, 2048 - off, "],\"answer\":\"语音模型已列出\"}");
    return o;
}

static char *op_model_install(const char *json_in) {
    char name[32] = {0}, url[600] = {0};
    jget(json_in, "name", name, sizeof(name));
    jget(json_in, "url", url, sizeof(url));
    const tts_model_t *M = NULL;
    for (int i = 0; TTS_MODELS[i].name; i++)
        if (!strcmp(TTS_MODELS[i].name, name)) { M = &TTS_MODELS[i]; break; }
    if (!M && !url[0])
        return jerr("不认识的模型名（可用：zh-en / kokoro），或给 url=压缩包地址");
    static tts_model_t custom;
    if (!M) {
        custom.name = name; custom.dir = "/opt/tts-rk3566/custom"; custom.url = url;
        custom.post = ""; custom.desc = "自定义";
        M = &custom;
    }
    /* 已经装过就直接跳过：以前点"下载安装"会把 180~210MB 再下一次 */
    long already = dir_size_mb(M->dir);
    if (already > 20) {
        char *o = malloc(400);
        if (!o) return NULL;
        snprintf(o, 400,
            "{\"ok\":1,\"skipped\":1,\"name\":\"%s\",\"mb\":%ld,"
            "\"answer\":\"%s 已经装过了（%ld MB），跳过下载\"}",
            M->name, already, M->name, already);
        return o;
    }
    mkdir(WORK_DIR "/gen", 0777);
    char log[700], q1[900], q2[900], q3[900];
    snprintf(log, sizeof(log), WORK_DIR "/gen/model_%s.log", M->name);
    sh_quote(M->url, q1, sizeof(q1));
    sh_quote(M->dir, q2, sizeof(q2));
    sh_quote(log, q3, sizeof(q3));
    char sh[4000];
    snprintf(sh, sizeof(sh),
        "( echo '[1/4] 下载'; curl -L --retry 3 -o /tmp/mdl.tbz %s || { echo '下载失败'; exit 1; }; "
        "echo '[2/4] 解包'; rm -rf /tmp/mdl_x; mkdir -p /tmp/mdl_x; "
        "bunzip2 -c /tmp/mdl.tbz | tar xf - -C /tmp/mdl_x || { echo '解包失败'; exit 1; }; "
        "echo '[3/4] 安装'; mkdir -p %s; cp -r /tmp/mdl_x/*/* %s/ 2>/dev/null; "
        "cd %s && ( %s ); echo '[4/4] 完成'; du -sm %s; rm -rf /tmp/mdl.tbz /tmp/mdl_x ) > %s 2>&1 &",
        q1, q2, q2, q2, M->post[0] ? M->post : "true", q2, q3);
    int rc = system(sh);
    if (rc != 0) return jerr("启动安装任务失败");
    char *o = malloc(1000);
    if (!o) return NULL;
    snprintf(o, 1000,
        "{\"ok\":1,\"started\":1,\"name\":\"%s\",\"log\":\"%s\",\"dir\":\"%s\","
        "\"answer\":\"已开始安装 %s（后台下载解包，完成后自动生效）\"}",
        M->name, log, M->dir, M->name);
    return o;
}


/* 卸载可选模型，回收空间（matcha 是内置的，不让删） */
static char *op_model_remove(const char *json_in) {
    char name[32] = {0};
    jget(json_in, "name", name, sizeof(name));
    if (!strcmp(name, "matcha"))
        return jerr("matcha 是内置模型（随 tts-rk3566 装的），不能从这里删");
    const tts_model_t *M = NULL;
    for (int i = 0; TTS_MODELS[i].name; i++)
        if (!strcmp(TTS_MODELS[i].name, name)) { M = &TTS_MODELS[i]; break; }
    if (!M) return jerr("不认识的模型名（zh-en / kokoro）");
    long before = dir_size_mb(M->dir);
    if (before <= 0) return jerr("这个模型本来就没装");
    char q[900], cmd[1200];
    sh_quote(M->dir, q, sizeof(q));
    snprintf(cmd, sizeof(cmd), "rm -rf %s", q);
    if (system(cmd) != 0) return jerr("删除失败（权限？）");
    char *o = malloc(600);
    if (!o) return NULL;
    snprintf(o, 600,
        "{\"ok\":1,\"name\":\"%s\",\"freed_mb\":%ld,\"answer\":\"已卸载 %s，回收 %ld MB\"}",
        M->name, before, M->name, before);
    return o;
}

static char *op_model_status(const char *json_in) {
    char name[32] = {0};
    jget(json_in, "name", name, sizeof(name));
    const tts_model_t *M = NULL;
    for (int i = 0; TTS_MODELS[i].name; i++)
        if (!strcmp(TTS_MODELS[i].name, name)) { M = &TTS_MODELS[i]; break; }
    if (!M) return jerr("不认识的模型名（zh-en / kokoro）");
    char log[700];
    snprintf(log, sizeof(log), WORK_DIR "/gen/model_%s.log", M->name);
    long mb = dir_size_mb(M->dir);
    char tail[1200] = {0};
    FILE *f = fopen(log, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, sz > 1000 ? sz - 1000 : 0, SEEK_SET);
        size_t n = fread(tail, 1, sizeof(tail) - 1, f);
        tail[n] = 0;
        fclose(f);
    }
    char esc[2600];
    jesc(tail, esc, sizeof(esc));
    char *o = malloc(strlen(esc) + 800);
    if (!o) return NULL;
    snprintf(o, strlen(esc) + 800,
        "{\"ok\":1,\"name\":\"%s\",\"installed\":%d,\"mb\":%ld,\"running\":%d,\"log\":\"%s\"}",
        M->name, mb > 20 ? 1 : 0, mb,
        (strstr(tail, "[4/4] 完成") || mb > 20) ? 0 : (tail[0] ? 1 : 0), esc);
    return o;
}

/* ---------- 语音转文字（sherpa-onnx sense-voice，中/英/日/韩/粤） ----------
 * 输入任意音频（mp3/wav/m4a/ogg…）：ffmpeg 转 16k 单声道裸流 →
 * /opt/asr/asr_sherpa 模型目录 raw → 一行文本。
 * asr_sherpa 是 aarch64 glibc 程序（板上 musl），走 /opt/glibc 的 loader。 */
static char *run_asr(const char *json_in) {
    char path[600] = {0}, lang[16] = {0};
    if (jget(json_in, "path", path, sizeof(path)) != 0 || !path[0]) {
        /* 没给路径就用 uploads 里最新的音频 */
        DIR *d = opendir(WORK_DIR "/uploads");
        if (d) {
            struct dirent *e;
            struct stat st2;
            time_t best = 0;
            while ((e = readdir(d)) != NULL) {
                const char *x = strrchr(e->d_name, '.');
                if (!x) continue;
                if (strcasecmp(x, ".mp3") && strcasecmp(x, ".wav") && strcasecmp(x, ".m4a")
                    && strcasecmp(x, ".ogg") && strcasecmp(x, ".aac") && strcasecmp(x, ".flac")) continue;
                char fp[900];
                snprintf(fp, sizeof(fp), WORK_DIR "/uploads/%s", e->d_name);
                if (stat(fp, &st2) == 0 && st2.st_mtime >= best) {
                    best = st2.st_mtime;
                    snprintf(path, sizeof(path), "%s", fp);
                }
            }
            closedir(d);
        }
    }
    if (!path[0]) return jerr("没有可识别的音频：给 path=音频文件，或先上传一段录音");
    if (access("/opt/asr/asr_sherpa", X_OK) != 0)
        return jerr("板上没装识别程序（/opt/asr/asr_sherpa）。它是 sherpa-onnx sense-voice 的 aarch64 版本。");
    if (access("/opt/asr/model/model.int8.onnx", R_OK) != 0)
        return jerr("缺识别模型：/opt/asr/model/model.int8.onnx（sense-voice 中英日韩粤）");

    char raw[700];
    snprintf(raw, sizeof(raw), WORK_DIR "/gen/.asr_%ld_%d.raw", (long)time(NULL), (int)getpid());
    char qp[1300], qr[1500];
    sh_quote(path, qp, sizeof(qp));
    sh_quote(raw, qr, sizeof(qr));
    char cmd[3000];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -v error -y -i %s -ar 16000 -ac 1 -f s16le %s >/dev/null 2>&1", qp, qr);
    if (system(cmd) != 0) { unlink(raw); return jerr("音频解码失败（ffmpeg 转 16k 单声道没成功）"); }

    char cmd2[3000];
    snprintf(cmd2, sizeof(cmd2),
             "env LD_LIBRARY_PATH=/opt/glibc:/opt/asr/lib /opt/glibc/ld-linux-aarch64.so.1 "
             "/opt/asr/asr_sherpa /opt/asr/model %s 2>/dev/null", qr);
    FILE *pp = popen(cmd2, "r");
    if (!pp) { unlink(raw); return jerr("识别程序启动失败"); }
    static char text[8000];
    size_t n = 0;
    text[0] = 0;
    while (n + 1 < sizeof(text) && fgets(text + n, (int)(sizeof(text) - n), pp)) {
        n = strlen(text);
        while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) text[--n] = 0;
    }
    pclose(pp);
    unlink(raw);
    if (!text[0]) return jerr("没识别出内容（音频是静音、太短，或语种不支持）");

    char esc[16000];
    jesc(text, esc, sizeof(esc));
    /* 同时落一份 .txt，方便后续当上传文件继续问 */
    char txtpath[700];
    snprintf(txtpath, sizeof(txtpath), "%s.txt", path);
    FILE *of = fopen(txtpath, "w");
    if (of) { fwrite(text, 1, strlen(text), of); fclose(of); }

    size_t cap = strlen(esc) + 1200;
    char *o = malloc(cap);
    if (!o) return NULL;
    snprintf(o, cap,
        "{\"ok\":1,\"text\":\"%s\",\"path\":\"%s\",\"file\":\"%s\",\"chars\":%zu,"
        "\"engine\":\"sherpa-onnx sense-voice (zh/en/ja/ko/yue, CPU)\","
        "\"answer\":\"转写完成（%zu 字）：%s\"}",
        esc, txtpath, txtpath, strlen(text), strlen(text), esc);
    (void)lang;
    return o;
}


/* ---------- 符号 → 中文（三个 TTS 模型都是词典/音素表驱动，
 * 遇到 % ℃ $ 这类符号直接丢掉，所以"30%"会念成"30"）----------
 * 处理顺序有讲究：百分号要插到数字**前面**（30% → 百分之30），
 * 再由模型自带的 number.fst 把 30 念成"三十"。 */
static void expand_symbols(char *t, size_t cap) {
    static char out[MAX_TEXT * 8 + 64];
    size_t o = 0;
    for (size_t i = 0; t[i] && o + 40 < sizeof(out); ) {
        unsigned char c = (unsigned char)t[i];
        /* 百分号：全角 ％ = EF BC 85，半角 % */
        int is_pct = (c == '%') ||
                     (c == 0xEF && (unsigned char)t[i + 1] == 0xBC && (unsigned char)t[i + 2] == 0x85);
        if (is_pct) {
            /* 往回找数字串的起点：把"百分之"插到数字前面 */
            size_t back = o;
            while (back > 0) {
                unsigned char b = (unsigned char)out[back - 1];
                if ((b >= '0' && b <= '9') || b == '.') back--;
                else break;
            }
            if (back < o) {
                size_t numlen = o - back;
                memmove(out + back + 9, out + back, numlen);   /* "百分之" 是 9 字节 */
                memcpy(out + back, "百分之", 9);
                o += 9;
            } else {
                memcpy(out + o, "百分之", 9); o += 9;
            }
            i += (c == '%') ? 1 : 3;
            continue;
        }
        /* 一对一的符号替换 */
        const char *rep = NULL;
        size_t adv = 1;
        if (c == 0xE2 && (unsigned char)t[i+1] == 0x84 && (unsigned char)t[i+2] == 0x83) {   /* ℃ */
            rep = "摄氏度"; adv = 3;
        } else if (c == 0xC2 && (unsigned char)t[i+1] == 0xB0) {                              /* ° */
            rep = "度"; adv = 2;
        } else if (c == 0xEF && (unsigned char)t[i+1] == 0xBF && (unsigned char)t[i+2] == 0xA5) { /* ￥ */
            rep = "元"; adv = 3;
        } else if (c == '$') { rep = "美元"; }
        else if (c == '&') { rep = "和"; }
        else if (c == '+') { rep = "加"; }
        else if (c == '=') { rep = "等于"; }
        else if (c == '<') { rep = "小于"; }
        else if (c == '>') { rep = "大于"; }
        else if (c == '~') { rep = "到"; }
        else if (c == '*') { rep = "乘"; }
        else if (c == '/') { rep = "斜杠"; }
        else if (c == '\\') { rep = "反斜杠"; }
        else if (c == '@') { rep = "艾特"; }
        else if (c == '#') { rep = "井号"; }
        if (rep) {
            size_t L = strlen(rep);
            memcpy(out + o, rep, L); o += L;
            i += adv;
            continue;
        }
        out[o++] = t[i++];
    }
    out[o] = 0;
    snprintf(t, cap, "%s", out);
}

/* ---------- 选模型 ----------
 * /opt/tts-rk3566/matcha        纯中文 matcha-zh-baker（22MB，中文最自然）
 * /opt/tts-rk3566/zh-en-model   中英双语 vits-melo-tts（lexicon-us-en + lexicon-zh），
 *                               中英混读用这个 —— 英文是真英语发音
 * /opt/tts-rk3566/en-model      纯英文 piper（这版 tts_sherpa 走词典，piper 需要
 *                               espeak 音素化，用它只会全 OOV，所以不选）
 */
static int has_dir(const char *d) {
    struct stat st;
    return stat(d, &st) == 0 && S_ISDIR(st.st_mode);
}

/* 按"字符数"算英文占比 —— 不能按字节：一个汉字是 3 字节，
 * 按字节算中文会把分母撑大，英文占比被稀释（实测 16% 被算成 6.8%，
 * 结果该切双语模型时没切）。UTF-8 的续字节(0x80-0xBF)直接跳过。 */
static int ascii_ratio(const char *t) {
    int en = 0, n = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;          /* 续字节 */
        n++;
        if (*p < 0x80 && isalpha(*p)) en++;
    }
    return n ? (en * 100 / n) : 0;
}

/* shell 单引号转义 */
static void sh_quote(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    if (i + 1 < cap) dst[i++] = '\'';
    for (const char *p = src; *p && i + 6 < cap; p++) {
        if (*p == '\'') { memcpy(dst + i, "'\\''", 4); i += 4; }
        else dst[i++] = *p;
    }
    if (i + 1 < cap) dst[i++] = '\'';
    dst[i] = 0;
}

/* 去首尾空白 */
static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (size_t n = strlen(s); n > 0; n--) {
        char c = s[n - 1];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') s[n - 1] = 0;
        else break;
    }
}

static char *run_tts(const char *json_in) {
    char text[MAX_TEXT + 1] = {0}, path[600] = {0}, out[600] = {0}, br[32] = {0};
    if (jget(json_in, "text", text, sizeof(text)) != 0 || !text[0]) {
        if (jget(json_in, "path", path, sizeof(path)) == 0 && path[0]) {
            FILE *f = fopen(path, "rb");
            if (!f) return jerr("要念的文本读不出来：path 指向的文件打不开");
            size_t n = fread(text, 1, MAX_TEXT, f);
            text[n] = 0;
            fclose(f);
        }
    }
    trim(text);
    if (!text[0]) return jerr("缺少 text（要念的中文内容），或用 path 指向一个文本文件");

    /* 选模型：有中英双语模型就用它（英文是真英语）；只有中文模型才退回
     * "英文按字母念"的老办法。lang 参数可以强制：zh / zh-en / en */
    char lang[16] = {0};
    jget(json_in, "lang", lang, sizeof(lang));
    int has_biling = has_dir("/opt/tts-rk3566/zh-en-model");
    int use_biling = 0;
    if (!strcmp(lang, "zh-en") || !strcmp(lang, "biling")) use_biling = 1;
    else if (!strcmp(lang, "zh")) use_biling = 0;
    else if (has_biling && ascii_ratio(text) >= 8) use_biling = 1;

    static char spoken[MAX_TEXT * 8 + 64];
    int had_en = 0;
    if (use_biling) {
        snprintf(spoken, sizeof(spoken), "%s", text);      /* 原样喂，双语模型自己会读 */
        had_en = ascii_ratio(text) > 0;
    } else {
        normalize_text(text, spoken, sizeof(spoken));      /* 逐字母中文读音兜底 */
        had_en = strcmp(spoken, text) != 0;
    }
    /* kokoro：多语种 + 一百多个音色，含男声。voice=male 走它（默认 sid 60，
     * 实测基频 116Hz，是明显的男声；sid 70/90/100 也是男声，可用 sid 指定）。
     * 代价是慢：RTF≈8（1 秒音频要 8 秒算），所以强制按 80 字节分段。 */
    char voice[16] = {0};
    jget(json_in, "voice", voice, sizeof(voice));
    int sid = (int)jget_num(json_in, "sid", -1);
    int use_kokoro = (!strcmp(voice, "male") || !strcmp(voice, "kokoro")
                      || !strcmp(voice, "man") || !strcmp(voice, "男声") || sid >= 0);
    if (use_kokoro && has_dir("/opt/tts-rk3566/kokoro") == 0) use_kokoro = 0;
    if (use_kokoro && sid < 0) sid = 60;
    if (!use_kokoro) sid = 0;

    /* 缓存键和送去合成的文本都用"整理过空格"的版本 —— 否则会命中旧缓存，
     * 听到的还是带空格那版怪音（这就是"修了但还是怪"的原因）。 */
    tidy_spaces(spoken);
    expand_symbols(spoken, sizeof(spoken));   /* 30% → 百分之30（再由 number.fst 念成三十） */

    const char *mdir = use_kokoro ? "/opt/tts-rk3566/kokoro"
                     : (use_biling ? "/opt/tts-rk3566/zh-en-model" : "/opt/tts-rk3566/matcha");
    const char *mkind = use_kokoro ? "kokoro" : (use_biling ? "vits" : "matcha");
    if (access(TTS_BIN, X_OK) != 0)
        return jerr("板上没装 tts-rk3566（缺 /usr/bin/tts）。装好它再试。");

    double speed = jget_num(json_in, "speed", 1.0);
    if (speed < 0.5 || speed > 2.0) speed = 1.0;
    if (jget(json_in, "bitrate", br, sizeof(br)) != 0 || !br[0]) snprintf(br, sizeof(br), "64k");
    int brkbps = atoi(br);
    if (brkbps < 16) brkbps = 64;

    mkdir(WORK_DIR, 0777);
    mkdir(WORK_DIR "/gen", 0777);

    /* 缓存：同一段文字 + 同样语速 → 复用已有 mp3（省掉 22 秒模型加载） */
    unsigned long h = hash_str(spoken) ^ ((unsigned long)(speed * 100) << 16)
                   ^ (use_biling ? 0x9e3779b1UL : 0) ^ ((unsigned long)(sid + 1) << 24);
    char cached[700];
    snprintf(cached, sizeof(cached), WORK_DIR "/gen/tts_%08lx.mp3", h);
    struct stat st;
    int is_cached = (stat(cached, &st) == 0 && st.st_size > 1000);

    if (!is_cached) {
        if (jget(json_in, "out", out, sizeof(out)) != 0 || !out[0])
            snprintf(out, sizeof(out), "%s/.tts_%ld_%d.mp3", WORK_DIR "/gen", (long)time(NULL), (int)getpid());
        char q[8 * MAX_TEXT + 128], qo[1400];
        sh_quote(spoken, q, sizeof(q));
        sh_quote(out, qo, sizeof(qo));
        /* wav 先出到临时文件，再用 ffmpeg 转 mp3（跟 tts.sh 的做法一致） */
        static char segs[24][400];
        int nseg = split_text(spoken, segs, 24, (use_biling || use_kokoro) ? 80 : 200);
        if (nseg <= 0) return jerr("没有可合成的内容");

        char base[700];
        snprintf(base, sizeof(base), WORK_DIR "/gen/.tts_%ld_%d", (long)time(NULL), (int)getpid());
        char listf[760], cmd[12000];
        int off = 0;
        cmd[0] = 0;
        /* 每段都是一次完整调用（env 必须直接接命令，不能接 ( 子壳 —— 这是上次
         * 把 env 提到循环外面导致"整条链都失败"的原因）。 */
        const char *envx = use_kokoro ? " TTS_KOKORO_INT8=1"
                        : (use_biling ? "" : " TTS_QDQ=1 TTS_VOCFP32=1");
        for (int i2 = 0; i2 < nseg && off < (int)sizeof(cmd) - 1200; i2++) {
            char q2[900], w2[800];
            sh_quote(segs[i2], q2, sizeof(q2));
            snprintf(w2, sizeof(w2), "%s_%d.wav", base, i2);
            off += snprintf(cmd + off, sizeof(cmd) - off,
                "( env TTS_KIND=%s%s LD_LIBRARY_PATH=/opt/glibc:/opt/rknpu:/opt/tts-rk3566/lib:%s "
                "/opt/glibc/ld-linux-aarch64.so.1 /opt/tts-rk3566/tts_sherpa %s %s %s %.2f %d "
                ">/dev/null 2>&1 || true ) ; ",
                mkind, envx, mdir, mdir, q2, w2, speed, sid);
        }
        snprintf(listf, sizeof(listf), "%s.list", base);
        /* 只把真生成出来的 wav 写进 concat 列表（某段失败就跳过它） */
        off += snprintf(cmd + off, sizeof(cmd) - off,
            ": > %s ; for f in %s_*.wav ; do [ -s \"$f\" ] && echo \"file $f\" >> %s ; done ; ",
            listf, base, listf);
        off += snprintf(cmd + off, sizeof(cmd) - off,
            "[ -s %s ] || { rm -f %s_*.wav %s ; exit 1 ; } ; "
            "ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i %s "
            "-codec:a libmp3lame -b:a %s %s >/dev/null 2>&1 ; rc=$? ; "
            "rm -f %s %s_*.wav ; exit $rc",
            listf, base, listf, listf, br, qo, listf, base);
        int rc = system(cmd);
        if (rc != 0 || stat(out, &st) != 0 || st.st_size < 1000) {
            unlink(out);
            return jerr("语音合成失败（tts 返回错误或没生成文件）。"
                        "先在手敲一次：tts \"测试\" /tmp/t.mp3 看报什么错");
        }
        rename(out, cached);
        snprintf(out, sizeof(out), "%s", cached);
    }

    long bytes = st.st_size;
    double secs = (double)bytes * 8.0 / ((double)brkbps * 1000.0);
    char tesc[2 * MAX_TEXT + 64];
    jesc(text, tesc, sizeof(tesc));
    char tesc2[2 * MAX_TEXT + 64];
    jesc(spoken, tesc2, sizeof(tesc2));
    /* 长文本只回显前 60 字节，免得气泡被正文刷屏 */
    char brief[200];
    snprintf(brief, sizeof(brief), "%s", tesc);
    if (strlen(brief) > 180) { snprintf(brief + 170, sizeof(brief) - 170, "…"); }
    size_t cap = strlen(tesc) + 900;
    char *o = malloc(cap);
    if (!o) return NULL;
    snprintf(o, cap,
        "{\"ok\":1,\"path\":\"%s\",\"file\":\"%s\",\"mp3\":\"%s\",\"audio\":\"%s\","
        "\"chars\":%zu,\"bytes\":%ld,\"secs\":%.1f,\"speed\":%.2f,\"bitrate\":\"%s\","
        "\"cached\":%d,\"english_spelled\":%d,\"lang\":\"%s\",\"tts_text\":\"%s\","
        "\"engine\":\"tts-rk3566 (%s)\","
        "\"answer\":\"语音已生成：%s（%zu 字，约 %.1f 秒%s）\"}",
        cached, cached, cached, cached,
        strlen(text), bytes, secs, speed, br,
        is_cached ? 1 : 0, had_en,
        use_kokoro ? "kokoro" : (use_biling ? "zh-en" : "zh"), tesc2,
        use_kokoro ? "kokoro-multi-lang int8, 24kHz, 男声"
                   : (use_biling ? "melo-tts zh_en 双语, CPU 16kHz" : "matcha-zh-baker, CPU 22.05kHz"),
        brief, strlen(text), secs,
        is_cached ? "，缓存命中"
                  : (use_biling ? "，中英双语发音" : (had_en ? "，英文按字母读" : "")));
    return o;
}

static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    if (!op[0] || !strcmp(op, "tts") || !strcmp(op, "speak") || !strcmp(op, "say")
        || !strcmp(op, "voice") || !strcmp(op, "audio_gen"))
        return run_tts(json_in);
    if (!strcmp(op, "asr") || !strcmp(op, "stt") || !strcmp(op, "听写")
        || !strcmp(op, "transcribe") || !strcmp(op, "语音转文字"))
        return run_asr(json_in);
    if (!strcmp(op, "models") || !strcmp(op, "list_models"))  return op_models(json_in);
    if (!strcmp(op, "model_install") || !strcmp(op, "install_model")) return op_model_install(json_in);
    if (!strcmp(op, "model_status"))                          return op_model_status(json_in);
    if (!strcmp(op, "model_remove") || !strcmp(op, "uninstall_model")) return op_model_remove(json_in);
    if (!strcmp(op, "probe")) {
        int has = (access(TTS_BIN, X_OK) == 0);
        char *o = malloc(400);
        if (!o) return NULL;
        snprintf(o, 400,
            "{\"ok\":1,\"installed\":%d,\"bin\":\"%s\",\"engine\":\"matcha-zh-baker 22.05kHz\","
            "\"note\":\"%s\"}",
            has, TTS_BIN,
            has ? "tts 就绪（首次调用要 ~22 秒加载模型）"
                : "未安装：opkg install tts-rk3566");
        return o;
    }
    return jerr("不支持的 op：tts / speak / say / probe");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "voice",
    "文字转语音（离线中文 TTS，纯 CPU）：文本 → MP3，气泡里可直接播放/下载。"
    "适合给回答配音、做语音播报、给视频配旁白。首次调用约 22 秒（加载 26MB 模型），同文本会命中缓存。",
    "{\"op\":\"tts\",\"text\":\"要念的中文\",\"speed\":1.0,\"bitrate\":\"64k\"}"
    " 或 {\"op\":\"tts\",\"path\":\"/tmp/正文.txt\"}；op=probe 看是否已安装",
    run,
    NULL
};

plugin_desc_t *plugin_entry(void) { return &desc; }
