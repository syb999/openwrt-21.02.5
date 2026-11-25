/* image —— 图片修复/放大（Real-ESRGAN on RKNPU，独立包 realesrgan-rk3566）
 *
 * {"op":"fix","path":"/tmp/old.jpg","scale":4}     4 倍放大（最清晰）
 * {"op":"fix","path":"/tmp/old.jpg","scale":2}     2 倍（2x2 平均，噪点更少）
 * {"op":"status"}                                  看模型/程序在不在
 *
 * 底层：/usr/bin/imgup → /opt/realesrgan/sr_img（glibc，走 /opt/glibc loader）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "plugin.h"

/* 本文件的 JSON 小工具（其它插件也都是各自一份 static） */
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


static int jget_int(const char *js, const char *key, int def) {
    char k[128], buf[32];
    snprintf(k, sizeof(k), "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p) return def;
    p = strchr(p + strlen(k), ':');
    if (!p) return def;
    p++;
    while (*p == ' ') p++;
    size_t n = 0;
    while (*p && (isdigit((unsigned char)*p) || *p == '-' || *p == '+') && n + 1 < sizeof(buf))
        buf[n++] = *p++;
    buf[n] = 0;
    return n ? atoi(buf) : def;
}

static char *jerr(const char *msg) {
    char *o = malloc(strlen(msg) + 40);
    if (!o) return NULL;
    snprintf(o, strlen(msg) + 40, "{\"error\":\"%s\"}", msg);
    return o;
}

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


#define WORK_DIR "/tmp/llama_agent"

static const char *EXT_IN[] = {".jpg", ".jpeg", ".png", ".bmp", ".webp", NULL};

static int ext_ok(const char *p) {
    const char *d = strrchr(p, '.');
    if (!d) return 0;
    for (int i = 0; EXT_IN[i]; i++)
        if (!strcasecmp(d, EXT_IN[i])) return 1;
    return 0;
}

static char *run_fix(const char *json_in) {
    char path[600] = {0};
    if (jget(json_in, "path", path, sizeof(path)) != 0 || !path[0])   /* jget 返回 0 才是成功 */
        return jerr("要给 path=图片路径（jpg/png/bmp/webp）");
    int scale = (int)jget_int(json_in, "scale", 4);
    if (scale != 2 && scale != 4) scale = 4;

    if (access("/usr/bin/imgup", X_OK) != 0)
        return jerr("没装 realesrgan-rk3566（/usr/bin/imgup 不存在）");
    if (!ext_ok(path)) return jerr("只支持 jpg / png / bmp / webp");

    char out[900], stem[400];
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    /* 去掉原扩展名，否则 输入.png → 输出 .png.png */
    snprintf(stem, sizeof(stem), "%s", bn);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    snprintf(out, sizeof(out), WORK_DIR "/gen/fix_%ld_%s.png", (long)time(NULL), stem);
    mkdir(WORK_DIR "/gen", 0777);

    char q1[1300], q2[1800];
    /* 简单 shell 引号：路径里不会有单引号就够用 */
    snprintf(q1, sizeof(q1), "'%s'", path);
    snprintf(q2, sizeof(q2), "'%s'", out);
    char cmd[4000];
    snprintf(cmd, sizeof(cmd), "/usr/bin/imgup %s %s %d >/dev/null 2>&1", q1, q2, scale);
    if (system(cmd) != 0) {
        unlink(out);
        return jerr("修复失败（NPU 推理出错，或图片太大内存不够）");
    }
    struct stat st;
    if (stat(out, &st) != 0 || st.st_size < 1000) { unlink(out); return jerr("没生成输出文件"); }

    char esc[1800];
    jesc(path, esc, sizeof(esc));
    char eout[1800];
    jesc(out, eout, sizeof(eout));
    char *o = malloc(6000);
    if (!o) return NULL;
    snprintf(o, 6000,
        /* 只给 file 一个字段：dlall() 会为 path/file/out 各生成一个下载链接，
         * 三个字段同值 → 气泡里出现 4 个一模一样的下载项。 */
        "{\"ok\":1,\"file\":\"%s\",\"src\":\"%s\","
        "\"scale\":%d,\"bytes\":%ld,"
        "\"engine\":\"Real-ESRGAN general-x4v3 (RKNPU int8)\","
        "\"answer\":\"图片已修复 %d 倍（%ld KB），点上面的文件下载\"}",
        eout, esc, scale, (long)st.st_size, scale, (long)(st.st_size / 1024));
    return o;
}


/* ---------- 背景取白 / 去阴影（文档扫描效果）----------
 * 拍照/打印的 A4 纸常有低频阴影或渐变，直接二值化会把细笔画吃掉。
 * 做法：估计背景（大核模糊）→ 光照校正 out = src*255/bg → 百分位对比度拉伸
 *      → 轻微白场提升，让背景变纯白、字仍是黑。纯 CPU，不占 NPU。
 */
static unsigned char *pgm_read(const char *f, int *pw, int *ph) {
    FILE *fp = fopen(f, "rb");
    if (!fp) return NULL;
    char magic[8] = {0};
    int w = 0, h = 0, mx = 0;
    if (fscanf(fp, "%2s", magic) != 1 || strcmp(magic, "P5") != 0 ||
        fscanf(fp, "%d %d %d", &w, &h, &mx) != 3 || w <= 0 || h <= 0 || w > 6000 || h > 6000) {
        fclose(fp); return NULL;
    }
    fgetc(fp);
    size_t n = (size_t)w * h;
    unsigned char *px = malloc(n);
    if (!px || fread(px, 1, n, fp) != n) { free(px); fclose(fp); return NULL; }
    fclose(fp);
    *pw = w; *ph = h;
    return px;
}

static int pgm_write(const char *f, const unsigned char *px, int w, int h) {
    FILE *fp = fopen(f, "wb");
    if (!fp) return -1;
    fprintf(fp, "P5\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, fp);
    fclose(fp);
    return 0;
}

static char *run_whiten(const char *json_in) {
    char path[600] = {0};
    if (jget(json_in, "path", path, sizeof(path)) != 0 || !path[0])
        return jerr("要给 path=图片路径");
    int strength = jget_int(json_in, "strength", 50);      /* 0..100 */
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;

    char pgm[600], opgm[600], out[800], stem[400];
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    snprintf(stem, sizeof(stem), "%s", bn);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    snprintf(pgm, sizeof(pgm), "/tmp/llama_agent/.wh_%ld.pgm", (long)getpid());
    snprintf(opgm, sizeof(opgm), "/tmp/llama_agent/.wh_%ld_o.pgm", (long)getpid());
    snprintf(out, sizeof(out), WORK_DIR "/gen/whiten_%ld_%s.png", (long)time(NULL), stem);

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -y -i '%s' -pix_fmt gray -f image2 '%s' 2>/dev/null", path, pgm);
    if (system(cmd) != 0) { unlink(pgm); return jerr("读图失败（支持 jpg/png/bmp/webp）"); }

    int w = 0, h = 0;
    unsigned char *px = pgm_read(pgm, &w, &h);
    unlink(pgm);
    if (!px) return jerr("解析图片失败");

    size_t n = (size_t)w * h;
    /* 1. 积分图 → 大核 box blur 当背景（半径 ≈ 短边/25，至少 8） */
    int R = (w < h ? w : h) / 25;
    if (R < 8) R = 8;
    double *ii = malloc(sizeof(double) * (size_t)(w + 1) * (h + 1));
    unsigned char *bg = malloc(n);
    if (!ii || !bg) { free(px); free(ii); free(bg); return jerr("内存不足"); }
    memset(ii, 0, sizeof(double) * (size_t)(w + 1) * (h + 1));
    for (int y = 0; y < h; y++) {
        double rowsum = 0;
        for (int x = 0; x < w; x++) {
            rowsum += px[(size_t)y * w + x];
            ii[(size_t)(y + 1) * (w + 1) + (x + 1)] = ii[(size_t)y * (w + 1) + (x + 1)] + rowsum;
        }
    }
    for (int y = 0; y < h; y++) {
        int y0 = y - R < 0 ? 0 : y - R, y1 = y + R >= h ? h - 1 : y + R;
        for (int x = 0; x < w; x++) {
            int x0 = x - R < 0 ? 0 : x - R, x1 = x + R >= w ? w - 1 : x + R;
            double sum = ii[(size_t)(y1 + 1) * (w + 1) + (x1 + 1)]
                       - ii[(size_t)y0 * (w + 1) + (x1 + 1)]
                       - ii[(size_t)(y1 + 1) * (w + 1) + x0]
                       + ii[(size_t)y0 * (w + 1) + x0];
            double area = (double)(y1 - y0 + 1) * (x1 - x0 + 1);
            double v = sum / area;
            bg[(size_t)y * w + x] = (unsigned char)(v < 1 ? 1 : (v > 255 ? 255 : v));
        }
    }
    free(ii);

    /* 2. 光照校正 */
    unsigned char *a = malloc(n);
    if (!a) { free(px); free(bg); return jerr("内存不足"); }
    for (size_t i = 0; i < n; i++) {
        int v = (int)px[i] * 255 / bg[i];
        a[i] = (unsigned char)(v > 255 ? 255 : v);
    }
    free(bg); free(px);

    /* 3. 百分位拉伸（2% ~ 98%） */
    int hist[256] = {0};
    for (size_t i = 0; i < n; i++) hist[a[i]]++;
    long lo = (long)(n * 0.02), hi = (long)(n * 0.98), acc = 0;
    int pl = 0, ph = 255;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= lo) { pl = i; break; } }
    acc = 0;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= hi) { ph = i; break; } }
    if (ph <= pl) { ph = 255; pl = 0; }
    for (size_t i = 0; i < n; i++) {
        int v = (int)a[i] - pl;
        v = v * 255 / (ph - pl);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        a[i] = (unsigned char)v;
    }

    /* 4. 白场提升：把接近白的直接压成纯白，字保持黑。
     * strength 越大越"白底黑字"（默认 50 → 阈值 205 起提） */
    int th = 255 - (int)(strength * 0.60);       /* 50 → 225 */
    if (th < 120) th = 120;
    for (size_t i = 0; i < n; i++) {
        int v = a[i];
        if (v >= th) { a[i] = 255; continue; }
        /* th 以下线性拉伸到 0..255，避免整片灰 */
        v = v * 255 / th;
        a[i] = (unsigned char)(v > 255 ? 255 : v);
    }

    if (pgm_write(opgm, a, w, h) != 0) { free(a); return jerr("写临时文件失败"); }
    free(a);
    snprintf(cmd, sizeof(cmd), "ffmpeg -v error -y -i '%s' '%s' 2>/dev/null", opgm, out);
    int rc = system(cmd);
    unlink(opgm);
    if (rc != 0) { unlink(out); return jerr("编码 PNG 失败"); }

    struct stat st;
    if (stat(out, &st) != 0) return jerr("没生成输出");
    char eout[1800], esrc[1800];
    jesc(out, eout, sizeof(eout));
    jesc(path, esrc, sizeof(esrc));
    char *o = malloc(4000);
    if (!o) return NULL;
    snprintf(o, 4000,
        "{\"ok\":1,\"file\":\"%s\",\"src\":\"%s\",\"w\":%d,\"h\":%d,\"bytes\":%ld,"
        "\"engine\":\"背景取白（积分图背景估计 + 光照校正 + 白场提升）\","
        "\"answer\":\"已去掉阴影、背景取白（%d×%d，%ld KB）\"}",
        eout, esrc, w, h, (long)st.st_size, w, h, (long)(st.st_size / 1024));
    return o;
}


/* ---------- 去摩尔纹（拍屏幕 / 翻拍印刷品）----------
 * 摩尔纹是采样混叠：屏幕像素栅格与相机传感器栅格拍频产生规则条纹。
 * 最有效的办法是"先降到一半再升回来"（area 面积平均丢掉混叠成分），
 * 再用中值滤波打掉残留的周期性斑点，最后锐化补回清晰度。
 * 三级强度，全部用板上现成的 ffmpeg 滤镜，纯 CPU。
 */
static char *run_moire(const char *json_in) {
    char path[600] = {0};
    if (jget(json_in, "path", path, sizeof(path)) != 0 || !path[0])
        return jerr("要给 path=图片路径");
    int strength = jget_int(json_in, "strength", 2);      /* 1 轻 / 2 默认 / 3 重 */
    if (strength < 1) strength = 1;
    if (strength > 3) strength = 3;

    char out[800], stem[400], vf[600];
    const char *bn = strrchr(path, '/');
    bn = bn ? bn + 1 : path;
    snprintf(stem, sizeof(stem), "%s", bn);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    snprintf(out, sizeof(out), WORK_DIR "/gen/moire_%ld_%s.png", (long)time(NULL), stem);

    /* 真正的做法是"自适应频谱陷波"：
     * 摩尔纹在频域是几个孤立亮点，找到它们挖掉再反变换 —— 模糊/中值对低频条带无效。
     * 用板上的 python3 + numpy 算 FFT（比在 C 里手写 FFT 稳，也快）。
     * 万一 python/numpy 不可用，退回"降采样+中值"的弱办法并如实说明。 */
    char cmd[1600];
    snprintf(cmd, sizeof(cmd),
             "python3 /opt/realesrgan/demoire.py '%s' '%s' %d 8 2>/dev/null", path, out, strength);
    int rc = system(cmd);
    int used_fft = (rc == 0);
    if (!used_fft || access(out, R_OK) != 0) {
        if (strength == 1)
            snprintf(vf, sizeof(vf),
                "scale=iw/2:ih/2:flags=area,scale=iw*2:ih*2:flags=lanczos,unsharp=5:5:0.5");
        else if (strength == 2)
            snprintf(vf, sizeof(vf),
                "scale=iw/2:ih/2:flags=area,scale=iw*2:ih*2:flags=lanczos,median=3,unsharp=5:5:0.7");
        else
            snprintf(vf, sizeof(vf),
                "scale=iw/2:ih/2:flags=area,scale=iw*2:ih*2:flags=lanczos,"
                "hqdn3d=2:2:4:4,median=5,unsharp=5:5:0.9");
        used_fft = 0;
    }
    if (!used_fft) {
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -y -i '%s' -vf \"%s\" '%s' 2>/dev/null", path, vf, out);
        if (system(cmd) != 0) { unlink(out); return jerr("去摩尔纹失败（ffmpeg 处理出错）"); }
    }

    struct stat st;
    if (stat(out, &st) != 0 || st.st_size < 1000) { unlink(out); return jerr("没生成输出"); }
    char eout[1800], esrc[1800];
    jesc(out, eout, sizeof(eout));
    jesc(path, esrc, sizeof(esrc));
    char *o = malloc(4000);
    if (!o) return NULL;
    snprintf(o, 4000,
        "{\"ok\":1,\"file\":\"%s\",\"src\":\"%s\",\"strength\":%d,\"bytes\":%ld,"
        "\"engine\":\"%s\","
        "\"answer\":\"已去摩尔纹（强度 %d，%ld KB）\"}",
        eout, esrc, strength, (long)st.st_size,
        used_fft ? "去摩尔纹（自适应频谱陷波，FFT）" : "去摩尔纹（降采样+中值，弱模式）",
        strength, (long)(st.st_size / 1024));
    return o;
}

static char *run_status(const char *json_in) {
    (void)json_in;
    int bin = access("/opt/realesrgan/sr_img", X_OK) == 0;
    int m1 = access("/opt/realesrgan/models/realesr-general-x4v3_640x360.rknn", R_OK) == 0;
    int m2 = access("/opt/realesrgan/models/realesr-general-x4v3_384x384.rknn", R_OK) == 0;
    int m3 = access("/opt/realesrgan/models/realesr-general-x4v3_192x192.rknn", R_OK) == 0;
    char *o = malloc(700);
    if (!o) return NULL;
    snprintf(o, 700,
        "{\"ok\":1,\"binary\":%d,\"models\":{\"640x360\":%d,\"384x384\":%d,\"192x192\":%d},"
        "\"answer\":\"图片修复：%s\"}",
        bin, m1, m2, m3,
        (bin && m1) ? "就绪（Real-ESRGAN general-x4v3 on NPU）" : "未就绪：缺 realesrgan-rk3566 或模型");
    return o;
}

static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    if (!strcmp(op, "fix") || !strcmp(op, "修复") || !strcmp(op, "upscale")
        || !strcmp(op, "zoom") || !strcmp(op, "enhance"))
        return run_fix(json_in);
    if (!strcmp(op, "status") || !strcmp(op, "probe")) return run_status(json_in);
    if (!strcmp(op, "moire") || !strcmp(op, "demoiré") || !strcmp(op, "去摩尔纹")
        || !strcmp(op, "screen")) return run_moire(json_in);
    if (!strcmp(op, "whiten") || !strcmp(op, "scan") || !strcmp(op, "去阴影")
        || !strcmp(op, "背景取白") || !strcmp(op, "deshadow")) return run_whiten(json_in);
    return jerr("支持的 op：fix（修复/放大，path+scale=2|4）、status（查就绪）");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "image",
    "图片修复/放大（Real-ESRGAN general-x4v3 跑 RKNPU int8）：模糊照片、压缩截图、老图放大变清晰。"
    "单张图不受帧率限制，可以直接出 4 倍。需要独立包 realesrgan-rk3566。",
    "{\"op\":\"fix\",\"path\":\"/tmp/old.jpg\",\"scale\":4} 放大；{\"op\":\"whiten\",\"path\":\"/tmp/paper.jpg\",\"strength\":50} 去阴影/背景取白；{\"op\":\"moire\",\"path\":\"/tmp/screen.jpg\",\"strength\":2} 去摩尔纹；op=status 查就绪",
    run,
    NULL
};

plugin_desc_t *plugin_entry(void) { return &desc; }
