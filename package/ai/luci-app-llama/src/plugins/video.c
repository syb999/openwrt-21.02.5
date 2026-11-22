/* video.c — 视频处理插件（ffmpeg 驱动）
 *
 * 名字里的 rkrga 是用户要的，但必须说清楚：这块板子上的 ffmpeg 5.1.3 是
 * OpenWrt stock 构建 —— configure 里没有 --enable-rkrga，`-filters` 里没有
 * rkrga，`-hwaccels` 是空的，`/dev/video*` 一个设备节点都没有，
 * librga.so 虽然存在但没有头文件也没有命令行工具，MPP 库也没装。
 * 所以硬件加速在这个系统上真的用不了（不是插件不写）。
 *
 * 本插件的做法：
 *   ① `hw` op 如实探测并报告每一项硬件能力（rga / mpp / v4l2m2m / 滤镜）
 *   ② 真能用的时候（换 rockchip ffmpeg 分支后）自动加上硬件参数
 *   ③ 竖屏 / 特效 / 裁剪 / 抽帧 / GIF 全部走 CPU，功能完整、速度慢但可用
 *
 * 支持的 op：
 *   hw                    硬件能力探测（先看这个）
 *   info  [path]          视频信息：分辨率/时长/码率/编码
 *   portrait [path]       转竖屏：模糊背景填充(默认) / 居中裁剪 / 黑边填充
 *   effects [path]        特效：淡入淡出、调色、变速、暗角、旋转、镜像、水印文字
 *   trim  [path]          截取片段
 *   thumb [path]          抽一张图（png）
 *   gif   [path]          转 GIF（短片段）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include "plugin.h"

#define OUTMAX   24576
#define WORK_DIR "/tmp/llama_agent"
/* 视频产物必须落【磁盘】：/tmp 是 1.9G 的 tmpfs（吃内存），
 * 一个 20 分钟的竖屏片就 500MB，写进去直接把内存和 CMA 拖死。 */
#define OUTDIR   "/opt/llama/out"
#define VJOBS    "/tmp/llama_agent/vjobs"

/* ---------------- JSON ---------------- */
static int jget(const char *js, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return -1;
    p = found + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) { p++; out[i++] = *p; }
        else out[i++] = *p;
        p++;
    }
    out[i] = 0;
    return 0;
}

/* 数字取值：jget 只认带引号的字符串，`"duration":20` 这种裸数字会被它
 * 判成"没有这个字段"，于是全走默认值（实测截 20 秒变成 10 秒）。
 * 这里单独实现，字符串和裸数字都认。 */
static long jget_int(const char *js, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return dflt;
    p = found + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return dflt;
    return v;
}

static double jget_num(const char *js, const char *key, double dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = js, *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') { found = q; break; }
        p = q;
    }
    if (!found) return dflt;
    p = found + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return dflt;
    return v;
}

/* ---------------- 结果构造 ---------------- */
static char *json_text(const char *text) {
    size_t cap = strlen(text) * 6 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, cap, "{\"value\":\"");
    for (const char *p = text; *p && i + 8 < cap; p++) {
        switch (*p) {
        case '"':  o[i++] = '\\'; o[i++] = '"';  break;
        case '\\': o[i++] = '\\'; o[i++] = '\\'; break;
        case '\n': o[i++] = '\\'; o[i++] = 'n';  break;
        case '\r': o[i++] = '\\'; o[i++] = 'r';  break;
        case '\t': o[i++] = '\\'; o[i++] = 't';  break;
        default:   o[i++] = *p;
        }
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

static char *json_pair(const char *text, const char *answer, const char *extra) {
    /* extra 允许传 NULL（有的 op 没有附加字段），直接 strlen(NULL) 会段错误 */
    if (!extra) extra = "";
    if (!answer) answer = "";
    if (!text) text = "";
    size_t cap = strlen(text) * 6 + strlen(answer) * 6 + strlen(extra) * 2 + 256;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, cap, "{\"value\":\"");
    for (const char *p = text; *p && i + 8 < cap; p++) {
        switch (*p) {
        case '"':  o[i++] = '\\'; o[i++] = '"';  break;
        case '\\': o[i++] = '\\'; o[i++] = '\\'; break;
        case '\n': o[i++] = '\\'; o[i++] = 'n';  break;
        case '\r': o[i++] = '\\'; o[i++] = 'r';  break;
        case '\t': o[i++] = '\\'; o[i++] = 't';  break;
        default:   o[i++] = *p;
        }
    }
    i += (size_t)snprintf(o + i, cap - i, "\",\"answer\":\"");
    for (const char *p = answer; *p && i + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    /* 无论有没有 extra，answer 的引号都必须闭合 —— 少了这个 " 出来的就是
     * 半截 JSON，前端 JSON.parse 直接失败（实测 detect 的结果就是这样）。 */
    i += (size_t)snprintf(o + i, cap - i, "\"");
    if (extra && extra[0])
        i += (size_t)snprintf(o + i, cap - i, ",%s", extra);
    snprintf(o + i, cap - i, "}");
    return o;
}

static char *errf(const char *fmt, const char *a, const char *b) {
    char m[700];
    snprintf(m, sizeof(m), fmt, a ? a : "", b ? b : "");
    char *o = malloc(strlen(m) * 6 + 64);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, strlen(m) * 6 + 64, "{\"ok\":0,\"error\":\"");
    for (const char *p = m; *p; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    snprintf(o + i, strlen(m) * 6 + 64 - i, "\"}");
    return o;
}

static void resolve_path(const char *in, char *out, size_t cap) {
    if (!in || !in[0]) { snprintf(out, cap, "%s", WORK_DIR); return; }
    if (in[0] == '/') { snprintf(out, cap, "%s", in); return; }
    snprintf(out, cap, "%s/%s", WORK_DIR, in);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ---------------- 跑外部命令（不走 shell）---------------- */
typedef struct { char **v; int n, cap; } argv_t;
static void av_init(argv_t *a, int cap) {
    a->v = calloc((size_t)cap, sizeof(char *));
    a->n = 0;
    a->cap = cap;
}
static void av_add(argv_t *a, const char *s) {
    if (a->n + 1 < a->cap) a->v[a->n++] = strdup(s);
}
static void av_free(argv_t *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    a->v = NULL;
    a->n = 0;
}

/* 跑命令，返回 exit code；stderr 收到 errbuf 里（方便把 ffmpeg 的报错给用户） */
static int run_argv(argv_t *a, char *errbuf, size_t errcap) {
    char tmpl[64];
    snprintf(tmpl, sizeof(tmpl), "/tmp/vid_%ld.err", (long)time(NULL));
    pid_t pid = fork();
    if (pid < 0) { snprintf(errbuf, errcap, "fork 失败"); return -1; }
    if (pid == 0) {
        int fd = open(tmpl, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execv(a->v[0], a->v);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    errbuf[0] = 0;
    FILE *f = fopen(tmpl, "r");
    if (f) {
        size_t n = fread(errbuf, 1, errcap - 1, f);
        errbuf[n] = 0;
        fclose(f);
        unlink(tmpl);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* 前向声明：这些实现在后面的"增强部分"，但 op_portrait 要用到 */
static double fps_of(const char *path);   /* 前向声明 */
static double duration_of(const char *path);
/* 跑 shell 管道（解码 | 重建 | 编码），返回退出码 */
static int run_pipe_sync(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}
static char *maybe_async(argv_t *a, const char *path, const char *out,
                         const char *kind, const char *json, const char *label);
static int run_async(argv_t *a, const char *progfile);
static void job_record(const char *id, int pid, const char *out, double total, const char *kind);

/* 硬件转码用的是【另一个二进制】/usr/bin/ffmpeg-rkrga（树里的
 * package/multimedia/ffmpeg-rkrga），不是 /usr/bin/ffmpeg。
 * 之前只探了 stock ffmpeg 就下结论"硬件不可用"，是错的。 */
static const char *ff_hw_path(void) {
    static const char *cands[] = {"/usr/bin/ffmpeg-rkrga", "/usr/local/bin/ffmpeg-rkrga", NULL};
    for (int i = 0; cands[i]; i++)
        if (access(cands[i], X_OK) == 0) return cands[i];
    return NULL;
}

/* 硬件编码是否可用：有 ffmpeg-rkrga 且它带 h264_rkmpp 编码器 */
static int hw_ok(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    const char *hw = ff_hw_path();
    if (!hw) return 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s -hide_banner -encoders 2>/dev/null | grep -q h264_rkmpp", hw);
    cached = (system(cmd) == 0);
    return cached;
}

/* ffmpeg-rkrga 是精简构建：只有它编进去的滤镜才能用（实测没有 gblur）。
 * 用某个 op 前先问它"这个滤镜有没有"，没有就退回 stock ffmpeg 软编。 */
static int ff_has_filter(const char *bin, const char *name) {
    if (!bin || !name) return 0;
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
        "%s -hide_banner -filters 2>/dev/null | grep -qE \"^ *[A-Z.]+ +%s \"", bin, name);
    return system(cmd) == 0;
}

static int g_force_sw;
static const char *g_cur_json;
static double g_job_fps = 25.0;   /* 给进度换算用（desub-rk3566 按帧报进度） */
static char g_src_path[512];      /* 当前处理的源文件，用来在结果里带 "src"
                                   * 前端据此同时显示"原片/处理后"两个播放器 */
static const char *g_cur_kind = "video";      /* 任务种类 id */
static const char *g_cur_label = "视频处理";    /* 给人看的名字 */   /* 当前 op 的入参 JSON，异步分支要用 */          /* 1 = 这个 op 只能用软件 ffmpeg */

static const char *ff_path(void) {
    static const char *cands[] = {"/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg", NULL};
    for (int i = 0; cands[i]; i++) if (access(cands[i], X_OK) == 0) return cands[i];
    return "ffmpeg";
}
static const char *fp_path(void) {
    static const char *cands[] = {"/usr/bin/ffprobe", "/usr/local/bin/ffprobe", NULL};
    for (int i = 0; cands[i]; i++) if (access(cands[i], X_OK) == 0) return cands[i];
    return NULL;
}

/* ---------------- 硬件能力探测 ---------------- */
static int has_rga(void) {
    return access("/dev/rga", R_OK | W_OK) == 0 && access("/usr/lib/librga.so", R_OK) == 0;
}
static int has_mpp(void) {
    return access("/dev/mpp_service", R_OK | W_OK) == 0;
}
static int has_v4l2node(void) {
    for (int i = 0; i < 32; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/video%d", i);
        if (access(p, R_OK) == 0) return 1;
    }
    return 0;
}

/* ffmpeg 里有没有某个滤镜（--enable-rkrga 才会出现） */
static int ffmpeg_has(const char *what) {
    argv_t a;
    av_init(&a, 8);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner");
    av_add(&a, "-filters");
    av_free(&a);
    /* 用 popen 读输出（只读命令、参数固定，无注入风险） */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s -hide_banner -filters 2>/dev/null", ff_path());
    FILE *pp = popen(cmd, "r");
    if (!pp) return 0;
    char line[512];
    int hit = 0;
    while (fgets(line, sizeof(line), pp))
        if (strstr(line, what)) { hit = 1; break; }
    pclose(pp);
    return hit;
}

/* 硬件解码是否真的可用：ffmpeg 里有 v4l2m2m 解码器 且 存在 /dev/video* */
static int hwdecode_ok(void) { return has_v4l2node() && ffmpeg_has("h264_v4l2m2m"); }

static char *op_hw(void) {
    const char *hw = ff_hw_path();
    int ok = hw_ok();
    int rgaf = 0, mppf = 0;
    if (hw) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s -hide_banner -filters 2>/dev/null | grep -q scale_rkrga", hw);
        rgaf = (system(cmd) == 0);
        snprintf(cmd, sizeof(cmd), "%s -hide_banner -encoders 2>/dev/null | grep -q hevc_rkmpp", hw);
        mppf = (system(cmd) == 0);
    }

    char text[2400];
    size_t i = (size_t)snprintf(text, sizeof(text),
        "**硬件加速能力（实测）**\n\n| 项 | 状态 |\n|---|---|\n"
        "| `/usr/bin/ffmpeg-rkrga` | %s |\n"
        "| h264_rkmpp 硬件编码 | %s |\n"
        "| hevc_rkmpp 硬件编码 | %s |\n"
        "| scale_rkrga 硬件缩放 | %s |\n"
        "| /dev/mpp_service | %s |\n"
        "| /dev/rga | %s |\n"
        "| librockchip_mpp | %s |\n\n",
        hw ? "有" : "**无**",
        ok ? "**可用**" : "不可用",
        mppf ? "**可用**" : "不可用",
        rgaf ? "**可用**" : "不可用",
        has_mpp() ? "有" : "无",
        has_rga() ? "有" : "无",
        access("/usr/lib/librockchip_mpp.so.0", R_OK) == 0 ? "有" : "无");

    if (ok)
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "**结论**：可以用硬件转码。正确姿势是**零拷贝**：\n\n"
            "```\n-hwaccel rkmpp -hwaccel_output_format drm_prime -c:v h264_rkmpp -b:v 3M\n```\n\n"
            "缩放走 `scale_rkrga`；**千万不要加 `-pix_fmt yuv420p`** —— 那会插进逐帧\n"
            "swscale（NV12→YUV420P），实测从 4.39x 掉到 1.06x。\n\n"
            "限制：VPU 只硬解 4:2:0 的 H.264/HEVC；yuv444p 源会退到软解。\n"
            "软件滤镜（模糊背景 gblur、调色 eq、暗角、drawtext）不在硬件通路里，\n"
            "用到它们的 op 仍然走 CPU。\n");
    else
        i += (size_t)snprintf(text + i, sizeof(text) - i,
            "没找到可用的 ffmpeg-rkrga —— 装 `ffmpeg-rkrga` 包后硬件转码才能用。\n");
    return json_text(text);
}

/* ---------- 通用异步执行 ----------
 * 长任务（给整片转码）同步跑的时候，前端拿到的是"一个卡住的请求"，
 * 什么进度都看不到。这里统一改法：素材超过 30 秒就自动后台跑，
 * 立刻返回 job id；前端轮询 job 拿百分比，画进度条。
 * 显式传 async:0 可以强制同步（短素材用）。 */
static char *maybe_async(argv_t *a, const char *path, const char *out,
                         const char *kind, const char *json, const char *label) {
    int async = (int)jget_int(json, "async", -1);
    double total = duration_of(path);
    if (async < 0) async = (total > 30);       /* 默认：长素材后台跑 */
    if (!async) return NULL;                    /* NULL = 调用方照旧同步执行 */

    char id[64], prog[512];
    snprintf(id, sizeof(id), "%s_%ld", kind, (long)time(NULL));
    mkdir(VJOBS, 0777);
    snprintf(prog, sizeof(prog), "%s/%s.prog", VJOBS, id);

    /* 把 -progress 插到 argv 前面（ffmpeg 允许放在输出之前的任意位置） */
    int n = a->n;
    if (n + 5 >= a->cap) return NULL;
    /* 把 [1 .. n-1] 整体后移 3 格，空出 1..3 插 -nostats -progress <file>。
     * 必须从后往前搬，并且下标是 i+3 = i（不是 i-1）—— 之前写错成 i-1，
     * 既漏搬最后一个参数又可能越界，实测直接段错误。 */
    for (int i = n - 1; i >= 1; i--) a->v[i + 3] = a->v[i];
    a->v[1] = strdup("-nostats");
    a->v[2] = strdup("-progress");
    a->v[3] = strdup(prog);
    a->n = n + 3;
    a->v[a->n] = NULL;

    int pid = run_async(a, prog);
    if (pid <= 0) return NULL;
    job_record(id, pid, out, total, kind);

    char text[900], ans[220], extra[420];
    snprintf(text, sizeof(text),
        "%s已在后台开始，完成后自动显示\n\n"
        "| 项 | 值 |\n|---|---|\n| 任务 ID | `%s` |\n| 模式 | %s |\n"
        "| 素材 | %.0f 秒 |\n| 输出 | `%s` |\n",
        label, id, label, total, out);
    snprintf(ans, sizeof(ans), "%s已开始（后台），完成后自动显示", label);
    snprintf(extra, sizeof(extra),
             "\"id\":\"%s\",\"pid\":%d,\"out\":\"%s\",\"async\":1,\"kind\":\"%s\","
             "\"total_sec\":%.0f,\"format\":\"mp4\",\"src\":\"%s\"",
             id, pid, out, kind, total, g_src_path);
    return json_pair(text, ans, extra);
}

/* ---------------- 探测媒体信息 ---------------- */
static char *op_info(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) jget(json, "file", p, sizeof(p));
    if (!p[0]) return errf("缺少 path（要处理的视频）%s%s", "", "");
    g_cur_kind = "portrait"; g_cur_label = "竖屏转换";
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s（/opt/mp4/ 下有示例）%s", path, "");

    const char *fp = fp_path();
    if (!fp) return errf("没有 ffprobe，装不上就先用 ffmpeg -i 看%s%s", "", "");

    char cmd[1200], line[512];
    snprintf(cmd, sizeof(cmd),
        "%s -v error -select_streams v:0 -show_entries "
        "stream=codec_name,width,height,r_frame_rate,bit_rate "
        "-show_entries format=duration,size,bit_rate -of default=nw=1 \"%s\" 2>&1",
        fp, path);
    FILE *pp = popen(cmd, "r");
    if (!pp) return errf("ffprobe 跑不起来%s%s", "", "");
    char codec[64] = "-", res[64] = "-", fps[32] = "-", dur[32] = "-", size[32] = "-", br[32] = "-";
    while (fgets(line, sizeof(line), pp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        char *nl = strchr(v, '\n');
        if (nl) *nl = 0;
        if (!strcmp(k, "codec_name")) snprintf(codec, sizeof(codec), "%s", v);
        else if (!strcmp(k, "width")) snprintf(res, sizeof(res), "%s", v);
        else if (!strcmp(k, "height")) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "x%s", v);
            strncat(res, tmp, sizeof(res) - strlen(res) - 1);
        }
        else if (!strcmp(k, "r_frame_rate")) snprintf(fps, sizeof(fps), "%s", v);
        else if (!strcmp(k, "duration")) snprintf(dur, sizeof(dur), "%.1f 秒", atof(v));
        else if (!strcmp(k, "size")) snprintf(size, sizeof(size), "%.1f MB", atof(v) / 1048576.0);
        else if (!strcmp(k, "bit_rate")) snprintf(br, sizeof(br), "%.0f kbps", atof(v) / 1000.0);
    }
    pclose(pp);

    char text[1200], ans[200];
    snprintf(text, sizeof(text),
        "**%s**\n\n| 项 | 值 |\n|---|---|\n| 编码 | %s |\n| 分辨率 | %s |\n"
        "| 帧率 | %s |\n| 时长 | %s |\n| 大小 | %s |\n| 码率 | %s |\n",
        strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
        codec, res, fps, dur, size, br);
    snprintf(ans, sizeof(ans), "%s · %s · %s · %s", codec, res, dur, size);
    char extra[400];
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\"", path, path);
    return json_pair(text, ans, extra);
}

/* ---------------- 转码公共部分 ---------------- */
static int out_name(const char *in_path, const char *suffix, const char *ext,
                    char *out, size_t cap) {
    const char *base = strrchr(in_path, '/');
    base = base ? base + 1 : in_path;
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    mkdir(OUTDIR, 0777);
    snprintf(out, cap, "%s/%s_%s_%ld.%s", OUTDIR, stem, suffix, (long)time(NULL), ext);
    return 0;
}

/* 编码器参数：硬件有就用 rkmpp（注意不加 -pix_fmt），否则 libx264。
 * 1080x1920 的瓶颈在编码（软编 ~8 fps），换成 VPU 后能到 30+ fps。 */
static void add_enc(argv_t *a, int hw, const char *bitrate) {
    if (hw) {
        av_add(a, "-c:v"); av_add(a, "h264_rkmpp");
        av_add(a, "-b:v"); av_add(a, bitrate ? bitrate : "3M");
        /* 不加 -pix_fmt：rkmpp 原生 NV12，强制 yuv420p 会插逐帧 swscale */
    } else {
        av_add(a, "-c:v"); av_add(a, "libx264");
        av_add(a, "-preset"); av_add(a, "ultrafast");
        av_add(a, "-crf"); av_add(a, "23");
        av_add(a, "-pix_fmt"); av_add(a, "yuv420p");
    }
}

static char *encode_with(const char *in, const char *out, const char *vf_complex,
                         const char *vf_simple, const char *extra_in,
                         const char *extra_out, int copy_audio) {
    int hw = hw_ok() && !g_force_sw;
    argv_t a;
    av_init(&a, 60);
    av_add(&a, hw ? ff_hw_path() : ff_path());
    av_add(&a, "-hide_banner");
    av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y");
    if (extra_in && extra_in[0]) av_add(&a, extra_in);
    av_add(&a, "-i"); av_add(&a, in);
    if (vf_complex && vf_complex[0]) {
        av_add(&a, "-filter_complex"); av_add(&a, vf_complex);
    } else if (vf_simple && vf_simple[0]) {
        av_add(&a, "-vf"); av_add(&a, vf_simple);
    }
    add_enc(&a, hw, NULL);
    if (copy_audio) { av_add(&a, "-c:a"); av_add(&a, "copy"); }
    else { av_add(&a, "-an"); }
    av_add(&a, "-movflags"); av_add(&a, "+faststart");
    if (extra_out && extra_out[0]) av_add(&a, extra_out);
    av_add(&a, out);

    {
        char *job = maybe_async(&a, in, out, g_cur_kind, g_cur_json, g_cur_label);
        if (job) { av_free(&a); return job; }
    }
    char err[900];
    int rc = run_argv(&a, err, sizeof(err));
    char kept[64];
    snprintf(kept, sizeof(kept), "%s", a.v[0]);
    av_free(&a);
    (void)kept;
    if (rc != 0) {
        char m[900];
        snprintf(m, sizeof(m), "ffmpeg 失败（退出码 %d）", rc);
        return errf("%s\n%.600s", m, err);
    }
    return NULL;   /* 成功返回 NULL，由调用方拼结果 */
}

/* ---------------- 竖屏 ---------------- */
static char *op_portrait(const char *json) {
    char p[768] = {0}, mode[32] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) jget(json, "file", p, sizeof(p));
    if (!p[0]) snprintf(p, sizeof(p), "/opt/mp4/01.mp4");
    jget(json, "mode", mode, sizeof(mode));
    if (!mode[0]) snprintf(mode, sizeof(mode), "blur");
    int w = (int)jget_int(json, "width", 1080);
    int h = (int)jget_int(json, "height", 1920);
    if (w < 240 || w > 2160) w = 1080;
    if (h < 240 || h > 3840) h = 1920;

    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    /* ffmpeg-rkrga 没有 gblur → 模糊背景只能用软件 ffmpeg */
    g_force_sw = 0;
    if (!strcmp(mode, "blur")) {
        if (!hw_ok() || !ff_has_filter(ff_hw_path(), "gblur")) g_force_sw = 1;
    }
    char vf[1200];
    if (!strcmp(mode, "crop")) {
        /* 铺满：放大到覆盖画布再裁中间 —— 内容损失最大，但没有黑边 */
        snprintf(vf, sizeof(vf),
            "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d", w, h, w, h);
    } else if (!strcmp(mode, "pad")) {
        /* 完整保留画面 + 黑边 */
        snprintf(vf, sizeof(vf),
            "scale=%d:%d:force_original_aspect_ratio=decrease,"
            "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black", w, h, w, h);
    } else {
        /* 默认：背景放大模糊，前景等比放中间（短视频最常用） */
        /* 在小画布上做模糊再放大：gblur 的开销跟像素数成正比，
         * 1080x1920 直接模糊实测慢到不可用（60 秒素材跑 10 分钟以上）。
         * 缩到 1/4 线性尺寸（1/16 像素）模糊再拉回，观感基本一致。 */
        int bw = w / 4, bh = h / 4;
        if (bw < 90) bw = 90;
        if (bh < 160) bh = 160;
        snprintf(vf, sizeof(vf),
            "[0:v]scale=%d:%d:force_original_aspect_ratio=increase,"
            "crop=%d:%d,gblur=sigma=6,scale=%d:%d:flags=bilinear[bg];"
            "[0:v]scale=%d:-2:flags=fast_bilinear[fg];"
            "[bg][fg]overlay=(W-w)/2:(H-h)/2",
            bw, bh, bw, bh, w, h, w);
    }

    char out[900];
    out_name(path, "portrait", "mp4", out, sizeof(out));

    /* 长任务（22 分钟的视频要一小时）必须能后台跑，否则一个 HTTP 请求挂死 */
    int async_portrait = (int)jget_int(json, "async", 0);
    if (async_portrait) {
        double total = duration_of(path);
        char id[64], prog[512];
        snprintf(id, sizeof(id), "portrait_%ld", (long)time(NULL));
        mkdir(VJOBS, 0777);
        snprintf(prog, sizeof(prog), "%s/%s.prog", VJOBS, id);

        int hw = hw_ok() && !g_force_sw;
        argv_t a;
        av_init(&a, 60);
        av_add(&a, hw ? ff_hw_path() : ff_path());
        av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
        av_add(&a, "-nostats");
        av_add(&a, "-progress"); av_add(&a, prog);
        av_add(&a, "-y");
        av_add(&a, "-i"); av_add(&a, path);
        if (!strcmp(mode, "blur")) {
            av_add(&a, "-filter_complex"); av_add(&a, vf);
        } else {
            av_add(&a, "-vf"); av_add(&a, vf);
        }
        add_enc(&a, hw, NULL);
        av_add(&a, "-c:a"); av_add(&a, "copy");
        av_add(&a, "-movflags"); av_add(&a, "+faststart");
        av_add(&a, out);
        int pid = run_async(&a, prog);
        av_free(&a);
        if (pid <= 0) return errf("后台启动失败%s%s", "", "");
        job_record(id, pid, out, total, "portrait");

        char text[900], ans[220], extra[420];
        snprintf(text, sizeof(text),
            "**竖屏转换已开始（后台）**\n\n| 项 | 值 |\n|---|---|\n"
            "| 任务 ID | `%s` |\n| 进程 | %d |\n| 模式 | %s |\n"
            "| 输出 | `%s` |\n| 素材时长 | %.0f 秒 |\n\n"
            "查进度：`{\"op\":\"job\",\"id\":\"%s\"}`\n",
            id, pid, mode, out, total, id);
        snprintf(ans, sizeof(ans), "竖屏转换已在后台开始（%s），用 job 查进度", id);
        snprintf(extra, sizeof(extra), "\"id\":\"%s\",\"pid\":%d,\"out\":\"%s\",\"async\":1",
                 id, pid, out);
        return json_pair(text, ans, extra);
    }

    char *bad = encode_with(path, out, strcmp(mode, "blur") ? NULL : vf,
                            strcmp(mode, "blur") ? vf : NULL, NULL, NULL, 1);
    if (bad) return bad;

    struct stat st;
    stat(out, &st);
    char text[900], ans[220], extra[400];
    snprintf(text, sizeof(text),
        "**竖屏转换完成**（%s 模式，%dx%d）\n\n| 项 | 值 |\n|---|---|\n"
        "| 文件 | `%s` |\n| 大小 | %.1f MB |\n| 编码 | %s |\n",
        mode, w, h, out, st.st_size / 1048576.0,
        (hw_ok() && !g_force_sw) ? "H.264 (rkmpp 硬件编码)" : "H.264 (libx264 软编)");
    snprintf(ans, sizeof(ans), "已转成 %dx%d 竖屏（%s 模式），%.1f MB",
             w, h, mode, st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* drawtext 必须给 fontfile，否则报 "No font filename provided"。
 * 板上默认没有 CJK 字体：中文水印要自己装一个（Noto Sans CJK / 文泉驿），
 * 找不到就退回 DejaVu（只有拉丁字形，中文会显示成方框）。 */
static const char *font_file(void) {
    static const char *cands[] = {
        "/opt/llama/simhei.ttf",                      /* 用户自己放的黑体（有中文字形） */
        "/opt/llama/font.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/arphic/uming.ttc",
        "/opt/chrome-root/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/opt/chrome-root/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    for (int i = 0; cands[i]; i++)
        if (access(cands[i], R_OK) == 0) return cands[i];
    return NULL;
}

/* ---------------- 特效 ---------------- */
static char *op_effects(const char *json) {
    g_cur_kind = "effects"; g_cur_label = "特效处理";
    char p[768] = {0}, text_wm[128] = {0}, wmpos[32] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) jget(json, "file", p, sizeof(p));
    jget(json, "text", text_wm, sizeof(text_wm));
    jget(json, "position", wmpos, sizeof(wmpos));
    /* 模型常常只把 op 传过来，参数一个不给（"转成黑白视频" 实测就只发了
     * {op:effects,path:...}，于是插件报"没给任何特效参数"）。
     * 框架会把原始任务放在 _task 里，这里兜底解析常见中文说法 ——
     * 有显式参数时一律以显式参数为准。 */
    int vw = 0;
    char vf[1600];
    size_t k = 0;
    vf[0] = 0;

#define ADDVF(fmt, ...) do { \
        if (k) k += (size_t)snprintf(vf + k, sizeof(vf) - k, ", "); \
        k += (size_t)snprintf(vf + k, sizeof(vf) - k, fmt, ##__VA_ARGS__); \
    } while (0)

    int fade_in = (int)jget_int(json, "fade_in", 0);
    int fade_out = (int)jget_int(json, "fade_out", 0);
    double dur = jget_num(json, "duration", 0);
    double speed = jget_num(json, "speed", 1.0);
    double bright = jget_num(json, "brightness", 0);
    double contrast = jget_num(json, "contrast", 1.0);
    double sat = jget_num(json, "saturation", 1.0);
    int vignette = (int)jget_int(json, "vignette", 0);
    int rotate = (int)jget_int(json, "rotate", 0);
    int hflip = (int)jget_int(json, "hflip", 0);
    int gray = (int)jget_int(json, "grayscale", 0);
    int from_task = 0;
    {
        char tk[1024] = {0};
        jget(json, "_task", tk, sizeof(tk));
        int any = (int)jget_int(json, "fade_in", 0) | (int)jget_int(json, "fade_out", 0)
                | (int)jget_int(json, "vignette", 0) | (int)jget_int(json, "grayscale", 0)
                | (int)jget_int(json, "rotate", 0) | (int)jget_int(json, "hflip", 0);
        double num = jget_num(json, "speed", 1.0) + jget_num(json, "brightness", 0)
                   + jget_num(json, "contrast", 1.0) + jget_num(json, "saturation", 1.0);
        int explicit_params = (any != 0) || (num > 0.01 && num != 3.0);
        if (tk[0] && !explicit_params) {
            char *k = NULL;
            if ((k = strstr(tk, "黑白")) || strstr(tk, "灰度") || strstr(tk, "单色"))
                { gray = 1; from_task = 1; }
            if (strstr(tk, "淡入")) { fade_in = fade_in ? fade_in : 2; from_task = 1; }
            if (strstr(tk, "淡出")) { fade_out = fade_out ? fade_out : 2; from_task = 1; }
            if (strstr(tk, "慢放") || strstr(tk, "变慢")) { speed = 0.5; from_task = 1; }
            if (strstr(tk, "快放") || strstr(tk, "加速")) { speed = 2.0; from_task = 1; }
            if (strstr(tk, "变亮") || strstr(tk, "提亮")) { bright = 0.08; from_task = 1; }
            if (strstr(tk, "变暗") || strstr(tk, "压暗")) { bright = -0.08; from_task = 1; }
            if (strstr(tk, "对比")) { contrast = 1.3; from_task = 1; }
            if (strstr(tk, "饱和")) { sat = 1.4; from_task = 1; }
            if (strstr(tk, "暗角")) { vignette = 1; from_task = 1; }
            if (strstr(tk, "镜像")) { hflip = 1; from_task = 1; }
            if (strstr(tk, "旋转90")) { rotate = 90; from_task = 1; }
            if (strstr(tk, "旋转180")) { rotate = 180; from_task = 1; }
            (void)k;
        }
    }


    if (speed > 1.01 || speed < 0.99) ADDVF("setpts=%f*PTS", 1.0 / speed);
    if (rotate == 90) ADDVF("transpose=1");
    else if (rotate == 180) ADDVF("transpose=1,transpose=1");
    else if (rotate == 270) ADDVF("transpose=2");
    if (hflip) ADDVF("hflip");
    if (gray) ADDVF("format=gray");
    if (bright != 0 || contrast != 1.0 || sat != 1.0)
        ADDVF("eq=brightness=%.2f:contrast=%.2f:saturation=%.2f", bright, contrast, sat);
    if (vignette) ADDVF("vignette=PI/4");
    if (fade_in > 0) ADDVF("fade=t=in:st=0:d=%d", fade_in);
    if (fade_out > 0 && dur > 0) ADDVF("fade=t=out:st=%.2f:d=%d", dur - fade_out, fade_out);
    if (text_wm[0]) {
        /* drawtext 的负坐标不是"从右边算"——直接 x=-20:y=-20 会跑到左上角
         * 还被切掉。必须写成表达式：x=w-tw-20 才是右对齐。 */
        /* 必须带上 y= 前缀：只写 "w-tw-20:h-th-20" 时 ffmpeg 会把第二段
         * 当成另一个选项名，最后报 Could not load font "h-th-20"。 */
        const char *xy;
        if (!strcmp(wmpos, "tl")) xy = "x=20:y=20";
        else if (!strcmp(wmpos, "tr")) xy = "x=w-tw-20:y=20";
        else if (!strcmp(wmpos, "bl")) xy = "x=20:y=h-th-20";
        else if (!strcmp(wmpos, "c")) xy = "x=(w-tw)/2:y=(h-th)/2";
        else xy = "x=w-tw-20:y=h-th-20";       /* 默认右下 */
        int fsz = (int)jget_int(json, "font_size", 28);
        if (fsz < 12) fsz = 12;
        if (fsz > 120) fsz = 120;
        const char *ff_font = font_file();
        if (!ff_font) {
            ADDVF("drawtext=text='%s':fontsize=%d:fontcolor=white@0.85:"
                  "box=1:boxcolor=black@0.35:boxborderw=8:%s", text_wm, fsz, xy);
        } else {
            ADDVF("drawtext=fontfile='%s':text='%s':fontsize=%d:fontcolor=white@0.85:"
                  "box=1:boxcolor=black@0.35:boxborderw=8:%s",
                  ff_font, text_wm, fsz, xy);
        }
    }
#undef ADDVF

    /* 特效链用的这些滤镜，硬件构建里通常没有；逐个试探，缺一个就走软件。
     * 灰度（format=gray）出来的帧 rkmpp 编码器不收（实测输出 0 字节），
     * 所以只要带灰度就必须走软编。 */
    g_force_sw = 1;
    if (gray) g_force_sw = 1;
    if (hw_ok()) {
        const char *hb = ff_hw_path();
        int okall = 1;
        if (bright != 0 || contrast != 1.0 || sat != 1.0) okall &= ff_has_filter(hb, "eq");
        if (vignette) okall &= ff_has_filter(hb, "vignette");
        if (text_wm[0]) okall &= ff_has_filter(hb, "drawtext");
        if (fade_in > 0 || fade_out > 0) okall &= ff_has_filter(hb, "fade");
        if (speed > 1.01 || speed < 0.99) okall &= ff_has_filter(hb, "setpts");
        if (okall && !gray) g_force_sw = 0;
    }

    if (!p[0]) return errf("缺少 path（要加特效的视频）%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");
    if (!k) return errf("没给任何特效参数。可用：fade_in/fade_out/speed/brightness/"
                        "contrast/saturation/vignette/rotate/hflip/grayscale/text %s", "", "");

    char extra_in[128] = {0};
    if (speed > 1.01 || speed < 0.99)
        snprintf(extra_in, sizeof(extra_in), "-filter_threads 2");

    char out[900];
    out_name(path, "fx", "mp4", out, sizeof(out));
    char *bad = encode_with(path, out, NULL, vf,
                            extra_in[0] ? extra_in : NULL, NULL,
                            !(speed > 1.01 || speed < 0.99));
    if (bad) return bad;

    struct stat st;
    stat(out, &st);
    char text[1000], ans[200], extra[400];
    snprintf(text, sizeof(text),
        "**特效已应用**\n\n| 项 | 值 |\n|---|---|\n| 滤镜链 | `%s` |\n"
        "| 文件 | `%s` |\n| 大小 | %.1f MB |\n", vf, out, st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "已应用特效（%s），%.1f MB",
             strrchr(out, '/') ? strrchr(out, '/') + 1 : out, st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 截取 / 抽帧 / GIF ---------------- */
static char *op_trim(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    double start = jget_num(json, "start", 0);
    double dur = jget_num(json, "duration", 10);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    char out[900], ss[32], tt[32];
    out_name(path, "clip", "mp4", out, sizeof(out));
    snprintf(ss, sizeof(ss), "%.2f", start);
    snprintf(tt, sizeof(tt), "%.2f", dur);

    argv_t a;
    av_init(&a, 40);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y");
    av_add(&a, "-ss"); av_add(&a, ss);
    av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-t"); av_add(&a, tt);
    av_add(&a, "-c:v"); av_add(&a, "libx264");
    av_add(&a, "-preset"); av_add(&a, "ultrafast");
    av_add(&a, "-crf"); av_add(&a, "23");
    av_add(&a, "-pix_fmt"); av_add(&a, "yuv420p");
    av_add(&a, "-c:a"); av_add(&a, "aac");
    av_add(&a, "-movflags"); av_add(&a, "+faststart");
    av_add(&a, out);
    char err[900];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("截取失败（%d）：%.500s", "", err);

    struct stat st;
    stat(out, &st);
    char text[600], ans[200], extra[400];
    snprintf(text, sizeof(text), "**已截取** %.2f~%.2f 秒\n\n文件：`%s`（%.1f MB）\n",
             start, start + dur, out, st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "已截取 %.1f 秒片段，%.1f MB", dur, st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

static char *op_thumb(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    double at = jget_num(json, "at", 5);
    int w = (int)jget_int(json, "width", 0);
    if (!p[0]) snprintf(p, sizeof(p), "/opt/mp4/01.mp4");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    char out[900], ss[32], scale[64] = {0};
    out_name(path, "thumb", "png", out, sizeof(out));
    snprintf(ss, sizeof(ss), "%.2f", at);
    if (w > 0) snprintf(scale, sizeof(scale), "scale=%d:-2", w);

    argv_t a;
    av_init(&a, 30);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y");
    av_add(&a, "-ss"); av_add(&a, ss);
    av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-frames:v"); av_add(&a, "1");
    if (scale[0]) { av_add(&a, "-vf"); av_add(&a, scale); }
    av_add(&a, out);
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("抽帧失败：%.500s", "", err);

    struct stat st;
    stat(out, &st);
    char text[600], ans[200], extra[400];
    snprintf(text, sizeof(text), "**已抽帧**（%.1f 秒处）\n\n文件：`%s`（%.1f KB）\n",
             at, out, st.st_size / 1024.0);
    snprintf(ans, sizeof(ans), "已抽帧，%.1f KB", st.st_size / 1024.0);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"png\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

static char *op_gif(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    double start = jget_num(json, "start", 0);
    double dur = jget_num(json, "duration", 3);
    int w = (int)jget_int(json, "width", 480);
    int fps = (int)jget_int(json, "fps", 10);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");
    if (w < 80 || w > 1080) w = 480;
    if (fps < 5 || fps > 25) fps = 10;

    char out[900], ss[32], tt[32], vf[256];
    out_name(path, "gif", "gif", out, sizeof(out));
    snprintf(ss, sizeof(ss), "%.2f", start);
    snprintf(tt, sizeof(tt), "%.2f", dur);
    snprintf(vf, sizeof(vf), "fps=%d,scale=%d:-1:flags=lanczos,split[a][b];"
                             "[a]palettegen[p];[b][p]paletteuse", fps, w);

    argv_t a;
    av_init(&a, 30);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y");
    av_add(&a, "-ss"); av_add(&a, ss);
    av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-t"); av_add(&a, tt);
    av_add(&a, "-vf"); av_add(&a, vf);
    av_add(&a, "-loop"); av_add(&a, "0");
    av_add(&a, out);
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("GIF 生成失败：%.500s", "", err);

    struct stat st;
    stat(out, &st);
    char text[600], ans[200], extra[400];
    snprintf(text, sizeof(text), "**GIF 已生成**\n\n文件：`%s`（%.1f MB，%dx%d，%dfps）\n",
             out, st.st_size / 1048576.0, w, w, fps);
    snprintf(ans, sizeof(ans), "GIF 已生成，%.1f MB", st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"gif\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ======================================================================
 * 以下为增强部分：压缩 / 音轨 / 水印 / 裁剪缩放 / 合并 / 切片 /
 * 缩略图阵列 / 转封装 / 黑帧与场景检测 / 长任务后台跑 + 进度查询
 * ====================================================================== */


/* 取时长（秒），失败返回 0 */
static double duration_of(const char *path) {
    const char *fp = fp_path();
    if (!fp) return 0;
    char cmd[1200], line[256];
    snprintf(cmd, sizeof(cmd),
        "%s -v error -show_entries format=duration -of default=nw=1:nk=1 \"%s\" 2>/dev/null",
        fp, path);
    FILE *pp = popen(cmd, "r");
    if (!pp) return 0;
    double d = 0;
    if (fgets(line, sizeof(line), pp)) d = atof(line);
    pclose(pp);
    return d;
}

/* 取帧率（r_frame_rate 形如 25/1） */
static double fps_of(const char *path) {
    char cmd[600], buf[128];
    snprintf(cmd, sizeof(cmd),
             "%s -v error -select_streams v:0 -show_entries stream=r_frame_rate "
             "-of default=nw=1:nk=1 \"%s\" 2>/dev/null", fp_path(), path);
    FILE *p = popen(cmd, "r");
    if (!p) return 25.0;
    double v = 25.0;
    if (fgets(buf, sizeof(buf), p)) {
        double num = 0, den = 1;
        if (sscanf(buf, "%lf/%lf", &num, &den) == 2 && den > 0) v = num / den;
        else if (sscanf(buf, "%lf", &num) == 1 && num > 0) v = num;
    }
    pclose(p);
    return v > 0.1 ? v : 25.0;
}

/* 后台起 ffmpeg：不等待，stdout/stderr 丢 /dev/null，进度写 progfile */
static int run_async(argv_t *a, const char *progfile) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        execv(a->v[0], a->v);
        _exit(127);
    }
    (void)progfile;
    return (int)pid;
}

/* 写任务记录，返回 job id */
static void job_record(const char *id, int pid, const char *out, double total, const char *kind) {
    mkdir(VJOBS, 0777);
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.json", VJOBS, id);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "{\"id\":\"%s\",\"pid\":%d,\"out\":\"%s\",\"total\":%.2f,\"kind\":\"%s\","
               "\"start\":%ld,\"fps\":%.4f,\"src\":\"%s\"}\n",
            id, pid, out, total, kind, (long)time(NULL), g_job_fps, g_src_path);
    fclose(f);
}

/* 起一个长任务，返回 json（ok + id + 预计耗时） */
static char *job_start(const char *kind, const char *in, const char *out, const char *prog,
                       const char *label, double total) {
    char id[64];
    snprintf(id, sizeof(id), "%s_%ld", kind, (long)time(NULL));
    char progfile[512];
    snprintf(progfile, sizeof(progfile), "%s/%s.prog", VJOBS, id);
    mkdir(VJOBS, 0777);

    argv_t a;
    av_init(&a, 80);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner");
    av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-nostats");
    av_add(&a, "-progress"); av_add(&a, progfile);
    av_add(&a, "-y");
    for (const char *p = prog; p && *p; ) {
        const char *e = strchr(p, '\x1f');         /* \x1f 分隔参数 */
        size_t L = e ? (size_t)(e - p) : strlen(p);
        char tmp[1024];
        if (L >= sizeof(tmp)) L = sizeof(tmp) - 1;
        memcpy(tmp, p, L);
        tmp[L] = 0;
        av_add(&a, tmp);
        p = e ? e + 1 : NULL;
    }
    int pid = run_async(&a, progfile);
    av_free(&a);
    if (pid <= 0) return errf("起不来后台进程（fork 失败）%s%s", "", "");

    job_record(id, pid, out, total, kind);

    char text[900], ans[220], extra[400];
    double fps_est = 8.0;                          /* 1080x1920 软编实测 */
    long eta = total > 0 ? (long)(total * fps_est / fps_est) : 0;   /* 按 1x 实时估 */
    snprintf(text, sizeof(text),
        "**%s已开始（后台）**\n\n| 项 | 值 |\n|---|---|\n| 任务 ID | `%s` |\n"
        "| 进程 | %d |\n| 输出 | `%s` |\n| 素材时长 | %.0f 秒 |\n\n"
        "查进度：`{\"op\":\"job\",\"id\":\"%s\"}`\n", label, id, pid, out, total, id);
    snprintf(ans, sizeof(ans), "%s已在后台开始（任务 %s），用 job 查进度", label, id);
    snprintf(extra, sizeof(extra), "\"id\":\"%s\",\"pid\":%d,\"out\":\"%s\",\"async\":1",
             id, pid, out);
    (void)eta;
    return json_pair(text, ans, extra);
}

/* 查进度 */
static char *op_job(const char *json) {
    char id[64] = {0};
    jget(json, "id", id, sizeof(id));
    if (!id[0]) return errf("缺少 id（job 的返回值里有）%s%s", "", "");

    char jp[512], pp[512];
    snprintf(jp, sizeof(jp), "%s/%s.json", VJOBS, id);
    snprintf(pp, sizeof(pp), "%s/%s.prog", VJOBS, id);
    FILE *f = fopen(jp, "r");
    if (!f) return errf("没有这个任务：%s%s", id, "");
    char buf[600];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    int pid = 0;
    double total = 0;
    char out[900] = {0};
    {
        const char *q;
        if ((q = strstr(buf, "\"pid\":"))) pid = atoi(q + 6);
        if ((q = strstr(buf, "\"total\":"))) total = atof(q + 8);
        if ((q = strstr(buf, "\"out\":\""))) {
            q += 7;
            size_t i = 0;
            while (*q && *q != '"' && i + 1 < sizeof(out)) out[i++] = *q++;
            out[i] = 0;
        }
    }

    /* ffmpeg -progress 写出 out_time_us=... / progress=continue|end */
    double done = 0;
    int ended = 0;
    f = fopen(pp, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "out_time_us=", 12)) done = atof(line + 12) / 1000000.0;
            else if (!strncmp(line, "out_time_ms=", 12)) done = atof(line + 12) / 1000000.0;
            if (strstr(line, "progress=end")) ended = 1;
        }
        fclose(f);
    }
    /* 进程还在不在 */
    int alive = 0;
    if (pid > 0) {
        char kp[64];
        snprintf(kp, sizeof(kp), "/proc/%d", pid);
        alive = (access(kp, F_OK) == 0);
    }
    struct stat st;
    long long outsz = (stat(out, &st) == 0) ? (long long)st.st_size : 0;
    int pct = (total > 0) ? (int)(done * 100.0 / total) : (ended ? 100 : 0);
    if (pct > 100) pct = 100;

    const char *state = ended ? "完成" : (alive ? "进行中" : "已停止");
    char text[900], ans[220], extra[400];
    snprintf(text, sizeof(text),
        "**任务 %s**\n\n| 项 | 值 |\n|---|---|\n| 状态 | %s |\n| 进度 | %d%%（%.0f/%.0f 秒）|\n"
        "| 输出大小 | %.1f MB |\n| 文件 | `%s` |\n",
        id, state, pct, done, total, outsz / 1048576.0, out);
    snprintf(ans, sizeof(ans), "%s：%d%%（%.1f MB）", state, pct, outsz / 1048576.0);
    snprintf(extra, sizeof(extra),
             "\"state\":\"%s\",\"pct\":%d,\"done_sec\":%.1f,\"total_sec\":%.1f,\"size\":%lld,\"path\":\"%s\",\"file\":\"%s\"",
             state, pct, done, total, outsz, out, out);
    return json_pair(text, ans, extra);
}

/* ---------------- 压缩 ---------------- */
static char *op_compress(const char *json) {
    g_cur_kind = "compress"; g_cur_label = "压缩";
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    int crf = (int)jget_int(json, "crf", 28);
    if (crf < 16) crf = 16;
    if (crf > 40) crf = 40;
    int w = (int)jget_int(json, "width", 0);        /* 0 = 保持原分辨率 */
    int scale_w = w;
    if (scale_w < 0) scale_w = 0;
    if (scale_w > 3840) scale_w = 3840;
    char crfs[16], vf[64] = {0};
    snprintf(crfs, sizeof(crfs), "%d", crf);
    if (scale_w > 0) snprintf(vf, sizeof(vf), "scale=%d:-2", scale_w);

    struct stat st0;
    stat(path, &st0);

    char out[900];
    out_name(path, "small", "mp4", out, sizeof(out));

    int hw = hw_ok();
    argv_t a;
    av_init(&a, 50);
    av_add(&a, hw ? ff_hw_path() : ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
    if (vf[0]) { av_add(&a, "-vf"); av_add(&a, vf); }
    add_enc(&a, hw_ok(), "2M");
    av_add(&a, "-c:a"); av_add(&a, "aac");
    av_add(&a, "-b:a"); av_add(&a, "96k");
    av_add(&a, "-movflags"); av_add(&a, "+faststart");
    av_add(&a, out);
    {
        char *job = maybe_async(&a, path, out, "compress", json, "压缩");
        if (job) { av_free(&a); return job; }
    }
    char err[800];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("压缩失败（%d）：%.400s", "", err);

    struct stat st;
    stat(out, &st);
    double ratio = st0.st_size > 0 ? (1.0 - (double)st.st_size / st0.st_size) * 100.0 : 0;
    char text[900], ans[220], extra[400];
    snprintf(text, sizeof(text),
        "**压缩完成**\n\n| 项 | 值 |\n|---|---|\n| 原大小 | %.1f MB |\n| 新大小 | %.1f MB |\n"
        "| 减小 | %.0f%% |\n| CRF | %d |\n| 宽度 | %s |\n| 文件 | `%s` |\n",
        st0.st_size / 1048576.0, st.st_size / 1048576.0, ratio, crf,
        scale_w > 0 ? "已缩放到设定宽度" : "保持原分辨率", out);
    snprintf(ans, sizeof(ans), "压缩完成：%.1f MB → %.1f MB（减小 %.0f%%）",
             st0.st_size / 1048576.0, st.st_size / 1048576.0, ratio);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 音轨：提取 / 静音 / 调音量 ---------------- */
static char *op_audio(const char *json) {
    char p[768] = {0}, fmt[16] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "format", fmt, sizeof(fmt));
    if (!fmt[0]) snprintf(fmt, sizeof(fmt), "mp3");
    int mute = (int)jget_int(json, "mute", 0);
    double vol = jget_num(json, "volume", 1.0);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    char out[900];
    char volbuf[32];
    snprintf(volbuf, sizeof(volbuf), "%.2f", vol);

    if (mute || (vol != 1.0 && !strcmp(fmt, "keep"))) {
        /* 改音轨但保留视频 */
        out_name(path, mute ? "mute" : "vol", "mp4", out, sizeof(out));
        argv_t a;
        av_init(&a, 40);
        av_add(&a, ff_path());
        av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
        av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
        av_add(&a, "-c:v"); av_add(&a, "copy");
        if (mute) av_add(&a, "-an");
        else { av_add(&a, "-af"); av_add(&a, volbuf); }
        av_add(&a, out);
        char err[700];
        int rc = run_argv(&a, err, sizeof(err));
        av_free(&a);
        if (rc != 0) return errf("处理音轨失败：%.400s", "", err);
    } else {
        /* 提取音轨 */
        const char *ext = "mp3", *codec = "libmp3lame", *q = "4";
        if (!strcmp(fmt, "wav")) { ext = "wav"; codec = "pcm_s16le"; q = NULL; }
        else if (!strcmp(fmt, "aac") || !strcmp(fmt, "m4a")) { ext = "m4a"; codec = "aac"; q = "128k"; }
        else if (!strcmp(fmt, "opus")) { ext = "ogg"; codec = "libopus"; q = "96k"; }
        out_name(path, "audio", ext, out, sizeof(out));
        argv_t a;
        av_init(&a, 40);
        av_add(&a, ff_path());
        av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
        av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
        av_add(&a, "-vn"); av_add(&a, "-c:a"); av_add(&a, codec);
        if (q) {
            if (!strcmp(fmt, "mp3")) { av_add(&a, "-q:a"); av_add(&a, q); }
            else { av_add(&a, "-b:a"); av_add(&a, q); }
        }
        av_add(&a, out);
        char err[700];
        int rc = run_argv(&a, err, sizeof(err));
        av_free(&a);
        if (rc != 0) return errf("提取音轨失败（源可能没有音轨）：%.400s", "", err);
    }

    struct stat st;
    stat(out, &st);
    char text[700], ans[200], extra[400];
    snprintf(text, sizeof(text),
        "**音轨处理完成**（%s）\n\n文件：`%s`（%.1f MB）\n",
        mute ? "静音" : (vol != 1.0 ? "调音量" : "提取音轨"), out, st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "%s完成，%.1f MB", mute ? "静音" : "音轨处理",
             st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 图片水印 ---------------- */
static char *op_watermark(const char *json) {
    char p[768] = {0}, logo[768] = {0}, pos[16] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "logo", logo, sizeof(logo));
    jget(json, "position", pos, sizeof(pos));
    double opacity = jget_num(json, "opacity", 0.7);
    int lw = (int)jget_int(json, "logo_width", 0);     /* 0 = 视频宽度的 15% */
    int margin = (int)jget_int(json, "margin", 20);
    if (!p[0] || !logo[0]) return errf("需要 path（视频）和 logo（图片）%s%s", "", "");
    char path[900], lpath[900];
    resolve_path(p, path, sizeof(path));
    resolve_path(logo, lpath, sizeof(lpath));
    if (!exists(path)) return errf("视频不存在：%s%s", path, "");
    if (!exists(lpath)) return errf("水印图不存在：%s%s", lpath, "");
    if (opacity < 0.05) opacity = 0.05;
    if (opacity > 1.0) opacity = 1.0;

    char fc[900];
    const char *xy = "W-w-20:H-h-20";
    if (!strcmp(pos, "tl")) xy = "20:20";
    else if (!strcmp(pos, "tr")) xy = "W-w-20:20";
    else if (!strcmp(pos, "bl")) xy = "20:H-h-20";
    else if (!strcmp(pos, "c")) xy = "(W-w)/2:(H-h)/2";

    char marginbuf[16];
    snprintf(marginbuf, sizeof(marginbuf), "%d", margin);
    if (!strcmp(pos, "tl")) snprintf(fc, sizeof(fc),
        "[1:v]scale=%s:-1,format=rgba,colorchannelmixer=aa=%.2f[wm];[0:v][wm]overlay=%d:%d",
        lw > 0 ? "LOGO_W" : "iw*0.15", opacity, margin, margin);
    else
        snprintf(fc, sizeof(fc),
        "[1:v]scale=%s:-1,format=rgba,colorchannelmixer=aa=%.2f[wm];[0:v][wm]overlay=%s",
        lw > 0 ? "LOGO_W" : "iw*0.15", opacity, xy);
    /* 用具体宽度替换占位（lw 给了就用具体值） */
    if (lw > 0) {
        char tmp[900], lwb[16];
        snprintf(lwb, sizeof(lwb), "%d", lw);
        char *hit = strstr(fc, "LOGO_W");
        if (hit) {
            snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)(hit - fc), fc, lwb, hit + 6);
            snprintf(fc, sizeof(fc), "%s", tmp);
        }
    } else {
        char *hit = strstr(fc, "LOGO_W");
        if (hit) {
            char tmp[900];
            snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(hit - fc), fc, hit + 6);
            /* 上面删掉占位后 scale= 的宽度会变成空，改用 iw*0.15 */
            char fixed[900];
            snprintf(fixed, sizeof(fixed), "%s", tmp);
            char *e = strstr(fixed, "scale=:-1");
            if (e) {
                char tmp2[900];
                snprintf(tmp2, sizeof(tmp2), "%.*sscale=iw*0.15:-1%s",
                         (int)(e - fixed), fixed, e + 11);
                snprintf(fixed, sizeof(fixed), "%s", tmp2);
            }
            snprintf(fc, sizeof(fc), "%s", fixed);
        }
    }
    (void)marginbuf;

    char out[900];
    out_name(path, "wm", "mp4", out, sizeof(out));

    argv_t a;
    av_init(&a, 50);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y");
    av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-i"); av_add(&a, lpath);
    av_add(&a, "-filter_complex"); av_add(&a, fc);
    av_add(&a, "-c:v"); av_add(&a, "libx264");
    av_add(&a, "-preset"); av_add(&a, "ultrafast");
    av_add(&a, "-crf"); av_add(&a, "24");
    av_add(&a, "-pix_fmt"); av_add(&a, "yuv420p");
    av_add(&a, "-c:a"); av_add(&a, "copy");
    av_add(&a, "-movflags"); av_add(&a, "+faststart");
    av_add(&a, out);
    char err[800];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("加水印失败：%.500s", "", err);

    struct stat st;
    stat(out, &st);
    char text[700], ans[200], extra[400];
    snprintf(text, sizeof(text), "**水印已加**（%s，不透明度 %.0f%%）\n\n文件：`%s`（%.1f MB）\n",
             pos[0] ? pos : "右下", opacity * 100, out, st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "水印已加，%.1f MB", st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 裁剪 / 缩放 ---------------- */
static char *op_crop_scale(const char *json) {
    char p[768] = {0}, op2[16] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "op", op2, sizeof(op2));
    int x = (int)jget_int(json, "x", 0), y = (int)jget_int(json, "y", 0);
    int w = (int)jget_int(json, "width", 0), h = (int)jget_int(json, "height", 0);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    char vf[256] = {0};
    if (!strcmp(op2, "crop")) {
        if (w <= 0) return errf("crop 需要 width（可加 height/x/y）%s%s", "", "");
        if (h > 0) snprintf(vf, sizeof(vf), "crop=%d:%d:%d:%d", w, h, x, y);
        else snprintf(vf, sizeof(vf), "crop=%d:ih:%d:%d", w, x, y);
    } else {
        if (w <= 0 && h <= 0) return errf("scale 需要 width 或 height%s%s", "", "");
        if (w > 0 && h > 0) snprintf(vf, sizeof(vf), "scale=%d:%d", w, h);
        else if (w > 0) snprintf(vf, sizeof(vf), "scale=%d:-2", w);
        else snprintf(vf, sizeof(vf), "scale=-2:%d", h);
    }

    char out[900];
    out_name(path, !strcmp(op2, "crop") ? "crop" : "scale", "mp4", out, sizeof(out));

    argv_t a;
    av_init(&a, 40);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-vf"); av_add(&a, vf);
    av_add(&a, "-c:v"); av_add(&a, "libx264");
    av_add(&a, "-preset"); av_add(&a, "ultrafast");
    av_add(&a, "-crf"); av_add(&a, "23");
    av_add(&a, "-pix_fmt"); av_add(&a, "yuv420p");
    av_add(&a, "-c:a"); av_add(&a, "copy");
    av_add(&a, "-movflags"); av_add(&a, "+faststart");
    av_add(&a, out);
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("处理失败（参数超出画面范围？）：%.400s", "", err);

    struct stat st;
    stat(out, &st);
    char text[700], ans[200], extra[400];
    snprintf(text, sizeof(text), "**%s 完成**\n\n| 项 | 值 |\n|---|---|\n| 滤镜 | `%s` |\n"
             "| 文件 | `%s` |\n| 大小 | %.1f MB |\n",
             !strcmp(op2, "crop") ? "裁剪" : "缩放", vf, out, st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "%s完成（%s），%.1f MB",
             !strcmp(op2, "crop") ? "裁剪" : "缩放", vf, st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 合并 ---------------- */
static char *op_concat(const char *json) {
    char list[4096] = {0};
    jget(json, "files", list, sizeof(list));       /* 逗号分隔的绝对路径 */
    if (!list[0]) return errf("需要 files：逗号分隔的视频路径列表%s%s", "", "");

    char lst[64];
    snprintf(lst, sizeof(lst), "/tmp/vcat_%ld.txt", (long)time(NULL));
    FILE *f = fopen(lst, "w");
    if (!f) return errf("写不了列表文件%s%s", "", "");
    int cnt = 0;
    char first[900] = {0};
    char *p = list;
    while (p && *p) {
        char *comma = strchr(p, ',');
        size_t L = comma ? (size_t)(comma - p) : strlen(p);
        char one[900];
        if (L >= sizeof(one)) L = sizeof(one) - 1;
        memcpy(one, p, L);
        one[L] = 0;
        while (*one == ' ') memmove(one, one + 1, strlen(one));
        char full[900];
        resolve_path(one, full, sizeof(full));
        if (exists(full)) {
            fprintf(f, "file '%s'\n", full);
            if (!first[0]) snprintf(first, sizeof(first), "%s", full);
            cnt++;
        }
        p = comma ? comma + 1 : NULL;
    }
    fclose(f);
    if (cnt < 2) { unlink(lst); return errf("至少要两个存在的文件（实际找到 %d 个）%s%s",
                                            NULL, ""); }

    char out[900];
    out_name(first, "merged", "mp4", out, sizeof(out));

    /* 先试 -c copy（最快，要求编码参数一致）；不行再重编 */
    argv_t a;
    av_init(&a, 40);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y");
    av_add(&a, "-f"); av_add(&a, "concat");
    av_add(&a, "-safe"); av_add(&a, "0");
    av_add(&a, "-i"); av_add(&a, lst);
    av_add(&a, "-c"); av_add(&a, "copy");
    av_add(&a, "-movflags"); av_add(&a, "+faststart");
    av_add(&a, out);
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);

    int reencoded = 0;
    if (rc != 0) {                                  /* 参数不一致 → 重编 */
        av_init(&a, 50);
        av_add(&a, ff_path());
        av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
        av_add(&a, "-y");
        av_add(&a, "-f"); av_add(&a, "concat");
        av_add(&a, "-safe"); av_add(&a, "0");
        av_add(&a, "-i"); av_add(&a, lst);
        av_add(&a, "-c:v"); av_add(&a, "libx264");
        av_add(&a, "-preset"); av_add(&a, "ultrafast");
        av_add(&a, "-crf"); av_add(&a, "23");
        av_add(&a, "-pix_fmt"); av_add(&a, "yuv420p");
        av_add(&a, "-c:a"); av_add(&a, "aac");
        av_add(&a, "-movflags"); av_add(&a, "+faststart");
        av_add(&a, out);
        rc = run_argv(&a, err, sizeof(err));
        av_free(&a);
        reencoded = 1;
    }
    unlink(lst);
    if (rc != 0) return errf("合并失败：%.500s", "", err);

    struct stat st;
    stat(out, &st);
    char text[800], ans[220], extra[400];
    snprintf(text, sizeof(text),
        "**合并完成**（%d 段，%s）\n\n文件：`%s`（%.1f MB，%.0f 秒）\n",
        cnt, reencoded ? "参数不一致→重编" : "直接拼接", out, st.st_size / 1048576.0,
        duration_of(out));
    snprintf(ans, sizeof(ans), "已合并 %d 段，%.1f MB", cnt, st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 按时长切片 ---------------- */
static char *op_split(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    int seg = (int)jget_int(json, "seconds", 60);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    if (seg < 5) seg = 5;
    if (seg > 3600) seg = 3600;
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    mkdir(OUTDIR, 0777);
    char out[900], segbuf[16];
    snprintf(out, sizeof(out), "%s/%s_seg_%%03d.mp4", OUTDIR, stem);
    snprintf(segbuf, sizeof(segbuf), "%d", seg);

    argv_t a;
    av_init(&a, 40);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-c"); av_add(&a, "copy");
    av_add(&a, "-map"); av_add(&a, "0");
    av_add(&a, "-f"); av_add(&a, "segment");
    av_add(&a, "-segment_time"); av_add(&a, segbuf);
    av_add(&a, "-reset_timestamps"); av_add(&a, "1");
    av_add(&a, out);
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("切片失败：%.400s", "", err);

    /* 数一下生成了几个 */
    int n = 0;
    char first[900] = {0};
    for (int i = 0; i < 999; i++) {
        char c[900];
        snprintf(c, sizeof(c), "%s/%s_seg_%03d.mp4", OUTDIR, stem, i);
        if (exists(c)) { n++; if (!first[0]) snprintf(first, sizeof(first), "%s", c); }
    }
    char text[800], ans[220], extra[400];
    snprintf(text, sizeof(text),
        "**已切片**（每段 %d 秒，共 %d 段）\n\n目录：`%s/`\n第一段：`%s`\n",
        seg, n, OUTDIR, first);
    snprintf(ans, sizeof(ans), "切成 %d 段（每段 %d 秒）", n, seg);
    snprintf(extra, sizeof(extra), "\"parts\":%d,\"path\":\"%s\",\"file\":\"%s\"", n, first, first);
    return json_pair(text, ans, extra);
}

/* ---------------- 缩略图阵列 ---------------- */
static char *op_sheet(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    int cols = (int)jget_int(json, "cols", 4);
    int rows = (int)jget_int(json, "rows", 3);
    int tw = (int)jget_int(json, "tile_width", 320);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    if (cols < 1 || cols > 8) cols = 4;
    if (rows < 1 || rows > 8) rows = 3;
    if (tw < 80 || tw > 800) tw = 320;
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    double dur = duration_of(path);
    int total = cols * rows;
    char vf[256];
    if (dur > 0)
        snprintf(vf, sizeof(vf), "fps=%d/%.0f,scale=%d:-1,tile=%dx%d",
                 total, dur, tw, cols, rows);
    else
        snprintf(vf, sizeof(vf), "scale=%d:-1,tile=%dx%d", tw, cols, rows);

    char out[900];
    out_name(path, "sheet", "jpg", out, sizeof(out));
    argv_t a;
    av_init(&a, 40);
    av_add(&a, ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
    av_add(&a, "-vf"); av_add(&a, vf);
    av_add(&a, "-frames:v"); av_add(&a, "1");
    av_add(&a, out);
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("生成缩略图阵列失败：%.400s", "", err);

    struct stat st;
    stat(out, &st);
    char text[700], ans[200], extra[400];
    snprintf(text, sizeof(text), "**缩略图阵列**（%dx%d 张）\n\n文件：`%s`（%.1f KB）\n",
             cols * rows, 1, out, st.st_size / 1024.0);
    snprintf(ans, sizeof(ans), "已生成 %d 张缩略图阵列，%.1f KB", cols * rows,
             st.st_size / 1024.0);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"jpg\",\"size\":%ld",
             out, out, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 转封装 ---------------- */
static char *op_convert(const char *json) {
    g_cur_kind = "convert"; g_cur_label = "转封装";
    char p[768] = {0}, to[16] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "to", to, sizeof(to));
    if (!to[0]) snprintf(to, sizeof(to), "mp4");
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    const char *ext = to;
    if (!strcmp(to, "mkv")) ext = "mkv";
    else if (!strcmp(to, "mov")) ext = "mov";
    else if (!strcmp(to, "webm")) ext = "webm";
    else ext = "mp4";

    char out[900];
    out_name(path, "conv", ext, out, sizeof(out));

    int hw = hw_ok();
    argv_t a;
    av_init(&a, 40);
    av_add(&a, hw ? ff_hw_path() : ff_path());
    av_add(&a, "-hide_banner"); av_add(&a, "-loglevel"); av_add(&a, "error");
    av_add(&a, "-y"); av_add(&a, "-i"); av_add(&a, path);
    if (!strcmp(ext, "webm")) {
        /* 这台 ffmpeg 没编 libvpx → webm 只能出音频，明确告诉用户 */
        return errf("webm 需要 libvpx，这个 ffmpeg 构建没有。"
                    "改用 mkv（同样是通用封装）：{\"op\":\"convert\",\"to\":\"mkv\"} %s%s", "", "");
    }
    add_enc(&a, hw_ok(), "3M");
    av_add(&a, "-c:a"); av_add(&a, "aac");
    if (!strcmp(ext, "mp4") || !strcmp(ext, "mov")) {
        av_add(&a, "-movflags"); av_add(&a, "+faststart");
    }
    av_add(&a, out);
    {
        char *job = maybe_async(&a, path, out, "convert", json, "转封装");
        if (job) { av_free(&a); return job; }
    }
    char err[700];
    int rc = run_argv(&a, err, sizeof(err));
    av_free(&a);
    if (rc != 0) return errf("转封装失败：%.400s", "", err);

    struct stat st;
    stat(out, &st);
    char text[700], ans[200], extra[400];
    snprintf(text, sizeof(text), "**已转为 %s**\n\n文件：`%s`（%.1f MB）\n", ext, out,
             st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "已转为 %s，%.1f MB", ext, st.st_size / 1048576.0);
    snprintf(extra, sizeof(extra), "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"%s\",\"size\":%ld",
             out, out, ext, (long)st.st_size);
    return json_pair(text, ans, extra);
}

/* ---------------- 黑帧 / 场景切换检测 ---------------- */
static char *op_detect(const char *json) {
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    double thr = jget_num(json, "threshold", 10);
    if (!p[0]) return errf("缺少 path%s%s", "", "");
    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    char cmd[1400], line[512];
    snprintf(cmd, sizeof(cmd),
        "%s -hide_banner -nostats -i \"%s\" -vf \"blackdetect=d=0.4:pix_th=0.10,scdet=threshold=%.0f\" "
        "-an -f null - 2>&1", ff_path(), path, thr);
    FILE *pp = popen(cmd, "r");
    if (!pp) return errf("检测跑不起来%s%s", "", "");

    char black[1200] = {0};
    char scene[1200] = {0};
    int nb = 0, ns = 0;
    size_t kb = 0, ks = 0;
    while (fgets(line, sizeof(line), pp)) {
        char *h;
        if ((h = strstr(line, "black_start:"))) {
            /* black_start:12.3 black_end:15.6 black_duration:3.3 */
            double st = atof(h + 12);
            char *e = strstr(line, "black_end:");
            double en = e ? atof(e + 10) : 0;
            if (kb + 80 < sizeof(black)) {
                kb += (size_t)snprintf(black + kb, sizeof(black) - kb,
                                       "- %.1f ~ %.1f 秒（%.1f 秒黑屏）\n", st, en, en - st);
                nb++;
            }
        } else if ((h = strstr(line, "lavfi.scd.time:"))) {
            double t = atof(h + 15);
            if (ks + 40 < sizeof(scene)) {
                ks += (size_t)snprintf(scene + ks, sizeof(scene) - ks, "- %.1f 秒\n", t);
                ns++;
            }
        }
    }
    pclose(pp);

    char text[2600], ans[220];
    snprintf(text, sizeof(text),
        "**检测结果**（阈值 %.0f）\n\n### 黑屏片段（%d 段）\n%s\n"
        "### 场景切换（%d 处）\n%s\n",
        thr, nb, nb ? black : "（没有）\n", ns, ns ? scene : "（没有）\n");
    snprintf(ans, sizeof(ans), "黑屏 %d 段，场景切换 %d 处", nb, ns);
    return json_pair(text, ans, NULL);
}

/* ======================================================================
 * 去字幕（硬字幕）—— 保留画面，只处理字幕所在的横条
 * ====================================================================== */

/* 抽 24 帧灰度小图，按行统计"文字能量"，找出字幕带。
 * 硬字幕的特征：集中在画面下方的一条横带里，笔画造成强烈的高频对比，
 * 而且这条带子在时间上是稳定的（不像画面内容会到处动）。 */
static int detect_sub_band(const char *path, int src_w, int src_h,
                           int *out_y, int *out_h, int *out_conf) {
    const int W = 320, H = 240;
    char raw[128];
    snprintf(raw, sizeof(raw), "/tmp/desub_%ld.raw", (long)time(NULL));

    double dur = duration_of(path);
    if (dur < 1) dur = 10;
    char fps[48];
    snprintf(fps, sizeof(fps), "fps=24/%.0f", dur > 24 ? dur : 24);

    char cmd[1500];
    snprintf(cmd, sizeof(cmd),
        "%s -hide_banner -loglevel error -i \"%s\" -vf \"%s,scale=%d:%d,format=gray\" "
        "-frames:v 24 -f rawvideo -pix_fmt gray -y \"%s\"",
        ff_path(), path, fps, W, H, raw);

    if (system(cmd) != 0) { unlink(raw); return 0; }
    FILE *f = fopen(raw, "rb");
    if (!f) return 0;
    size_t fsz = (size_t)W * H;
    unsigned char *buf = malloc(fsz);
    if (!buf) { fclose(f); unlink(raw); return 0; }

    static double energy[240];
    memset(energy, 0, sizeof(energy));
    int nf = 0;
    while (fread(buf, 1, fsz, f) == fsz) {
        for (int y = 1; y < H - 1; y++) {
            double e = 0;
            const unsigned char *row = buf + (size_t)y * W;
            const unsigned char *up = buf + (size_t)(y - 1) * W;
            const unsigned char *dn = buf + (size_t)(y + 1) * W;
            for (int x = 1; x < W - 1; x++) {
                int gx = abs((int)row[x + 1] - (int)row[x - 1]);
                int gy = abs((int)dn[x] - (int)up[x]);
                if (gx + gy > 60) e += 1;      /* 强边缘像素计数 */
            }
            energy[y] += e / (double)W;
        }
        nf++;
    }
    free(buf);
    fclose(f);
    unlink(raw);
    if (nf < 3) return 0;
    for (int y = 0; y < H; y++) energy[y] /= nf;

    /* 只在下方 55% 里找；滑动窗口取"平均能量最高"的那条带 */
    int from = (int)(H * 0.45);
    int besty = -1, besth = 0;
    double best = -1;
    for (int h = 10; h <= (H - from); h += 2) {
        double acc = 0;
        for (int y = from; y + h <= H; y++) {
            double sum = 0;
            for (int k = 0; k < h; k++) sum += energy[y + k];
            double avg = sum / h;
            if (avg > best) { best = avg; besty = y; besth = h; }
            acc = avg;
        }
        (void)acc;
    }
    /* energy 是每行强边缘像素的比例(0~1)，阈值必须在 0.05 量级；
     * 之前写 1.0（当成了像素计数），探测永远返回失败。 */
    if (besty < 0 || best < 0.04) return 0;

    /* 把最亮的连续行向上/下扩展到能量降到一半的位置（字幕常有描边/上沿） */
    double peak = best;
    int y0 = besty, y1 = besty + besth;
    while (y0 > from && energy[y0 - 1] > peak * 0.45) y0--;
    while (y1 < H && energy[y1] > peak * 0.45) y1++;
    if (y1 - y0 < 6) return 0;

    /* 换算回源分辨率 */
    *out_y = (int)((double)y0 * src_h / H);
    *out_h = (int)((double)(y1 - y0) * src_h / H);
    /* 上下各留 2 像素余量，防止描边残留 */
    *out_y -= 2;
    *out_h += 4;
    if (*out_y < 0) *out_y = 0;
    if (*out_y + *out_h > src_h) *out_h = src_h - *out_y;
    *out_conf = (int)(peak * 100);
    return 1;
}

/* 取源分辨率 */
static void probe_size(const char *path, int *w, int *h) {
    *w = 0; *h = 0;
    const char *fp = fp_path();
    if (!fp) return;
    char cmd[1200], line[256];
    snprintf(cmd, sizeof(cmd),
        "%s -v error -select_streams v:0 -show_entries stream=width,height "
        "-of default=nw=1 \"%s\" 2>/dev/null", fp, path);
    FILE *pp = popen(cmd, "r");
    if (!pp) return;
    while (fgets(line, sizeof(line), pp)) {
        if (!strncmp(line, "width=", 6)) *w = atoi(line + 6);
        else if (!strncmp(line, "height=", 7)) *h = atoi(line + 7);
    }
    pclose(pp);
}

/* mode: auto(默认，自动找字幕带+模糊) | blur(手动区域) | delogo | crop(切掉) */
static char *op_desub(const char *json) {
    g_cur_kind = "desub"; g_cur_label = "去字幕";
    char p[768] = {0}, mode[16] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "mode", mode, sizeof(mode));
    if (!p[0]) return errf("缺少 path（要处理的视频）%s%s", "", "");
    if (!mode[0]) snprintf(mode, sizeof(mode), "temporal");

    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    int sw = 0, sh = 0;
    probe_size(path, &sw, &sh);
    if (!sw || !sh) return errf("读不到分辨率%s%s", "", "");

    int y = (int)jget_int(json, "y", -1);
    int hh = (int)jget_int(json, "height", 0);
    int x = (int)jget_int(json, "x", 0);
    int ww = (int)jget_int(json, "width", sw);
    int conf = 0, detected = 0;

    if (y < 0 || hh <= 0) {
        if (!detect_sub_band(path, sw, sh, &y, &hh, &conf)) {
            /* 探测失败：默认取下方 12% */
            hh = (int)(sh * 0.12);
            y = sh - hh;
            conf = 0;
        } else {
            detected = 1;
        }
    }
    if (ww <= 0 || ww > sw) ww = sw;
    if (x < 0) x = 0;
    if (x + ww > sw) ww = sw - x;

    /* 区域参数打印出来，方便核对 */
    char region[128];
    snprintf(region, sizeof(region), "x=%d y=%d w=%d h=%d", x, y, ww, hh);

    char out[900];
    out_name(path, "desub", "mp4", out, sizeof(out));

    /* ---- 时间域重建（真去除，默认）----
     * 解码 → desub_fill（按帧重建被字幕盖住的像素）→ 硬件编码。
     * 不是模糊/遮挡：填进去的像素来自同一位置"没有字幕的那一帧"。 */
    if (!strcmp(mode, "temporal") || !strcmp(mode, "rebuild")) {
        const char *tool = "/opt/llama/agent/desub_fill";
        if (access(tool, X_OK) != 0)
            return errf("缺少 desub_fill（升级到带该工具的版本）%s%s", "", "");

        int dil = (int)jget_int(json, "dilate", 4);   /* 膨胀：吃掉黑描边 */
        if (dil < 0) dil = 0;
        if (dil > 6) dil = 6;
        int K = (int)jget_int(json, "frames", 24);
        if (K < 3) K = 3;
        if (K > 48) K = 48;
        int thr = (int)jget_int(json, "white", 175);
        if (thr < 100) thr = 100;
        if (thr > 250) thr = 250;

        double fps = 25;
        {
            const char *fpp = fp_path();
            if (fpp) {
                char c2[1000], ln[128];
                snprintf(c2, sizeof(c2),
                    "%s -v error -select_streams v:0 -show_entries stream=r_frame_rate "
                    "-of default=nw=1:nk=1 \"%s\" 2>/dev/null", fpp, path);
                FILE *pp2 = popen(c2, "r");
                if (pp2) {
                    if (fgets(ln, sizeof(ln), pp2)) {
                        double a = 0, b = 0;
                        if (sscanf(ln, "%lf/%lf", &a, &b) == 2 && b > 0) fps = a / b;
                        else if (a > 0) fps = a;
                    }
                    pclose(pp2);
                }
            }
        }
        if (fps < 1 || fps > 240) fps = 25;

        double total_sec = duration_of(path);
        int async = (int)jget_int(json, "async", -1);
        if (async < 0) async = (total_sec > 30);

        char id[64], prog[512];
        snprintf(id, sizeof(id), "desub_%ld", (long)time(NULL));
        mkdir(VJOBS, 0777);
        snprintf(prog, sizeof(prog), "%s/%s.prog", VJOBS, id);

        int hw = hw_ok();
        const char *enc = hw ? ff_hw_path() : ff_path();
        char cmd[3000];
        snprintf(cmd, sizeof(cmd),
            "%s -hide_banner -loglevel error -i \"%s\" -f rawvideo -pix_fmt yuv420p - "
            "| %s -w %d -h %d -y %d -b %d -k %d -white %d -dil %d "
            "| %s -hide_banner -loglevel error -nostats -progress %s "
            "-f rawvideo -pix_fmt yuv420p -s %dx%d -r %.3f -i pipe:0 "
            "-i \"%s\" -map 0:v:0 -map 1:a? -c:v %s %s -c:a copy -shortest "
            "-movflags +faststart -y \"%s\"",
            ff_path(), path,
            tool, sw, sh, y, hh, K, thr, dil,
            enc, prog, sw, sh, fps,
            path,
            hw ? "h264_rkmpp" : "libx264",
            hw ? "-b:v 3M" : "-preset ultrafast -crf 23 -pix_fmt yuv420p",
            out);

        char text[1000], ans[240], extra[480];
        if (async) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                int fd = open("/dev/null", O_RDWR);
                if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
                execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
                _exit(127);
            }
            if (pid <= 0) return errf("后台启动失败%s%s", "", "");
            job_record(id, (int)pid, out, total_sec, "desub");

            snprintf(text, sizeof(text),
                "去字幕已在后台开始（时间域重建），完成后自动显示\n"
                "\n"
                "| 项 | 值 |\n|---|---|\n| 任务 ID | `%s` |\n"
                "| 字幕带 | x=0 y=%d w=%d h=%d |\n| 重建窗口 | 前后 %d 帧 |\n"
                "| 素材 | %.0f 秒 |\n| 输出 | `%s` |\n",
                id, y, ww, hh, K, total_sec, out);
            snprintf(ans, sizeof(ans), "去字幕已开始（时间域重建，%d 帧窗口）", K);
            snprintf(extra, sizeof(extra),
                     "\"id\":\"%s\",\"pid\":%d,\"out\":\"%s\",\"async\":1,"
                     "\"kind\":\"desub\",\"total_sec\":%.0f,\"format\":\"mp4\"",
                     id, (int)pid, out, total_sec);
            return json_pair(text, ans, extra);
        }

        int rc = run_pipe_sync(cmd);
        if (rc != 0) {
            char m[200];
            snprintf(m, sizeof(m), "时间域重建失败（退出码 %d），看 /tmp/desub_err.log", rc);
            return errf("%s%s", m, "");
        }

        struct stat st2;
        stat(out, &st2);
        snprintf(text, sizeof(text),
            "**去字幕完成（时间域重建）**\n"
            "\n"
            "| 项 | 值 |\n|---|---|\n| 字幕带 | `x=0 y=%d w=%d h=%d` |\n"
            "| 重建窗口 | 前后 %d 帧 |\n| 分辨率 | %dx%d（原样保留）|\n"
            "| 文件 | `%s` |\n| 大小 | %.1f MB |\n",
            y, ww, hh, K, sw, sh, out, st2.st_size / 1048576.0);
        snprintf(ans, sizeof(ans), "已去除字幕（时间域重建），%.1f MB",
                 st2.st_size / 1048576.0);
        snprintf(extra, sizeof(extra),
                 "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
                 out, out, (long)st2.st_size);
        return json_pair(text, ans, extra);
    }
    char vf[900];
    /* npu：调板上 /opt/py/migan.sh（MI-GAN 6M 参数跑 RK3566 NPU，实测 1.97 秒/帧）。
     * 比 CPU 上的 LaMa 快 5 倍；脚本自己会停 LLM 腾内存、跑完再恢复。 */
    if (!strcmp(mode, "npu") || !strcmp(mode, "ai")) {
        char text[700], ans[240], extra[480], cmd[900], id[64], prog[512], logf[512];
        if (access("/opt/desub-rk3566/desub-rk3566.py", R_OK) != 0)
            return errf("NPU 去字幕未安装（需要 /opt/desub-rk3566/desub-rk3566.py）%s%s", "", "");
        /* 字幕带外扩：自动探测常常只圈到字心，描边/下沿会留下淡淡残影 */
        int ny = (int)jget_int(json, "band_y", -1);
        int nh = (int)jget_int(json, "band_h", -1);
        if (ny >= 0 && nh > 0) {
            y = ny; hh = nh;
        } else {
            y = y - 4;
            if (y < 0) y = 0;
            hh = hh + 12;
            if (y + hh > sh) hh = sh - y;
        }
        double dur = duration_of(path);
        double fps = fps_of(path);
        if (fps <= 1) fps = 25;
        int nfr = (int)(dur * fps + 0.5);
        if (nfr < 1) nfr = 1;
        snprintf(id, sizeof(id), "npu_%ld", (long)time(NULL));
        mkdir("/tmp/llama_agent", 0777);   /* 重启后父目录不存在，不建的话下面全失败 */
        mkdir(VJOBS, 0777);
        snprintf(prog, sizeof(prog), "%s/%s.prog", VJOBS, id);
        snprintf(logf, sizeof(logf), "%s/%s.log", VJOBS, id);
        /* desub-rk3566.py 用 --band 表示"字幕带起始位置占画面高度的比例"，
         * 不是像素。把探测到的 y 换算成比例给它。 */
        {
            double bandf = (double)y / (double)sh;
            if (bandf < 0.30) bandf = 0.30;
            if (bandf > 0.95) bandf = 0.95;
            snprintf(cmd, sizeof(cmd),
                     "python3 /opt/desub-rk3566/desub-rk3566.py '%s' '%s' --band %.3f",
                     path, out, bandf);
        }
        int want_async = (int)jget_int(json, "async", 1);
        if (want_async) {
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                int fd = open(logf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) fd = open("/dev/null", O_WRONLY);
                if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
                execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
                _exit(127);
            }
            if (pid < 0) return errf("启动 NPU 任务失败%s%s", "", "");
            g_job_fps = fps;
            job_record(id, (int)pid, out, dur, "npu去字幕");
            snprintf(text, sizeof(text),
                     "**NPU 去字幕已开始（后台）**\n\n| 项 | 值 |\n|---|---|\n"
                     "| 任务 ID | `%s` |\n| 字幕区域 | `x=0 y=%d w=%d h=%d` |\n"
                     "| 引擎 | desub-rk3566（MI-GAN on RKNPU） |\n"
                     "| 帧数 | %d 帧（约 %.0f 分钟） |\n| 输出 | `%s` |\n",
                     id, y, sw, hh, nfr, nfr * 2.0 / 60.0, out);
            snprintf(ans, sizeof(ans),
                     "NPU 去字幕已开始（%d 帧，约 %.0f 分钟），用 job 查进度", nfr, nfr * 2.0 / 60.0);
            snprintf(extra, sizeof(extra),
                     "\"id\":\"%s\",\"async\":1,\"pid\":%d,\"out\":\"%s\","
                     "\"total_sec\":%.1f,\"kind\":\"npu去字幕\"",
                     id, (int)pid, out, dur);
            return json_pair(text, ans, extra);
        }
        int rc = run_pipe_sync(cmd);
        if (rc != 0) return errf("NPU 去字幕失败（退出码 %d），见 %s%s", logf, "");
        struct stat stn; stat(out, &stn);
        snprintf(text, sizeof(text),
                 "**NPU 去字幕完成**\n\n| 项 | 值 |\n|---|---|\n"
                 "| 字幕区域 | `x=0 y=%d w=%d h=%d` |\n| 引擎 | MI-GAN（NPU） |\n"
                 "| 文件 | `%s` |\n", y, sw, hh, out);
        snprintf(ans, sizeof(ans), "NPU 去字幕完成，%.1f MB", stn.st_size / 1048576.0);
        snprintf(extra, sizeof(extra),
                 "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
                 out, out, (long)stn.st_size);
        return json_pair(text, ans, extra);
    }
    /* crop_extend：裁掉字幕带，底部用"最后几行镜像"补齐原高度。
     * 比"拉伸回原尺寸"好：不产生纵向变形（实测拉 12% 人脸会变长）。 */
    if (!strcmp(mode, "crop_extend") || !strcmp(mode, "extend")) {
        char text[600], ans[240], extra[480];   /* 这几个是块内局部变量，别用外层的 */
        int keep = y - 4;
        if (keep < 40) keep = sh - (y + hh) - 4;
        if (keep < 40) keep = sh * 3 / 4;
        keep &= ~1;                                /* yuv420p 要求偶数高度，否则输出会少 2 行 */
        int padh = sh - keep;
        if (padh < 0) padh = 0;
        if (padh > keep) padh = keep;              /* 镜像源不够就退化成拉伸 */
        if (padh > 0)
            snprintf(vf, sizeof(vf),
                     "[0:v]split[a][b];[b]crop=%d:%d:0:%d,vflip[p];"
                     "[a]crop=%d:%d:0:0[base];[base][p]vstack=inputs=2",
                     sw, padh, keep - padh, sw, keep);
        else
            snprintf(vf, sizeof(vf), "crop=%d:%d:0:0,scale=%d:%d:flags=lanczos",
                     sw, keep, sw, sh);
        char *bad = encode_with(path, out, NULL, vf, NULL, NULL, 1);
        if (bad) return bad;
        struct stat st9;
        stat(out, &st9);
        snprintf(text, sizeof(text),
                 "**已去字幕（crop_extend：裁掉底部 %d 像素，镜像补齐，不拉伸）**\n\n"
                 "| 项 | 值 |\n|---|---|\n| 模式 | crop_extend |\n"
                 "| 原尺寸 | %dx%d |\n| 输出 | %dx%d |\n| 文件 | `%s` |\n",
                 sh - keep, sw, sh, sw, sh, out);
        snprintf(ans, sizeof(ans), "已去字幕（裁掉 %d 像素，镜像补齐，无变形），%.1f MB",
                 sh - keep, st9.st_size / 1048576.0);
        snprintf(extra, sizeof(extra),
                 "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
                 out, out, (long)st9.st_size);
        return json_pair(text, ans, extra);
    }
    /* fill：用字幕带"上方那一行"的平均色把整条带涂满。
     * crop 会拉伸画面（位置全变），fill 不动任何原有像素位置，
     * 适合 03.mp4 这种字幕压在纯色条上的片子 —— 涂完和没有字幕一模一样。 */
    if (!strcmp(mode, "fill") || !strcmp(mode, "cover")) {
        char text[600], ans[240], extra[480];
        int sy = y - 2;
        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        snprintf(vf, sizeof(vf),
                 "[0:v]split[a][b];"
                 /* 必须显式 yuv420p：overlay 的 format=auto 会产出 yuvj420p（pc 全范围），
                  * 浏览器直接报 MEDIA_ERR_SRC_NOT_SUPPORTED（实测 err=4，画面全黑） */
                 "[b]crop=%d:2:0:%d,scale=1:1:flags=area,scale=%d:%d:flags=neighbor,"
                 "format=yuv420p[p];[a][p]overlay=0:%d:format=yuv420",
                 sw, sy, sw, hh, y);
        char *bad = encode_with(path, out, NULL, vf, NULL, NULL, 1);
        if (bad) return bad;
        struct stat st8;
        stat(out, &st8);
        snprintf(text, sizeof(text),
                 "**去字幕完成（用背景色覆盖字幕带）**\n\n"
                 "| 项 | 值 |\n|---|---|\n| 字幕区域 | `x=0 y=%d w=%d h=%d` |\n"
                 "| 处理方式 | 取上方一行平均色涂满（画面位置不变、不拉伸） |\n"
                 "| 分辨率 | %dx%d（原样） |\n| 文件 | `%s` |\n",
                 y, sw, hh, sw, sh, out);
        snprintf(ans, sizeof(ans), "已去字幕（背景色覆盖，画面位置不变），%.1f MB",
                 st8.st_size / 1048576.0);
        snprintf(extra, sizeof(extra),
                 "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
                 out, out, (long)st8.st_size);
        return json_pair(text, ans, extra);
    }
    if (!strcmp(mode, "crop")) {
        /* 直接切掉：画面会变矮，但绝对不会留痕迹 */
        /* 切掉字幕带后**拉伸回原尺寸**：动画片这么拉几乎看不出来，
         * 而且绝对没有伪造像素 —— 这是唯一"零痕迹"的做法。 */
        int keep = y - 2;
        if (keep < 40) keep = sh - (y + hh) - 2;
        if (keep < 40) keep = sh - 8;
        snprintf(vf, sizeof(vf),
                 "crop=%d:%d:0:0,scale=%d:%d:flags=lanczos", sw, keep, sw, sh);
        char *bad = encode_with(path, out, NULL, vf, NULL, NULL, 1);
        if (bad) return bad;
    } else {
        /* 用 gblur 而不是 boxblur：boxblur 的 chroma_radius 上限是 12，
         * 带宽算出来的半径（17+）直接报 "Invalid chroma_param radius"。
         * gblur 的 sigma 没有这个限制，两个 ffmpeg 构建都有。 */
        double sig = jget_num(json, "blur", 0);
        if (sig <= 0) {
            sig = hh / 2.5;                  /* 跟带宽成正比，宁糊别漏字 */
            if (sig < 8) sig = 8;
            if (sig > 32) sig = 32;
        }
        /* 只模糊那一条：切出来糊完再盖回去，其余像素原样不动 */
        snprintf(vf, sizeof(vf),
            "[0:v]split=2[base][tmp];"
            "[tmp]crop=%d:%d:%d:%d,gblur=sigma=%.1f[band];"
            "[base][band]overlay=%d:%d",
            ww, hh, x, y, sig, x, y);
        char *bad = encode_with(path, out, vf, NULL, NULL, NULL, 1);
        if (bad) return bad;
    }

    struct stat st;
    stat(out, &st);
    char text[1100], ans[240], extra[460];
    snprintf(text, sizeof(text),
        "**去字幕完成**（%s）\n\n| 项 | 值 |\n|---|---|\n"
        "| 字幕区域 | `%s`%s |\n| 处理方式 | %s |\n"
        "| 分辨率 | %dx%d（原样保留）|\n| 文件 | `%s` |\n| 大小 | %.1f MB |\n",
        detected ? "自动探测到字幕带" : "手动/默认区域",
        region,
        detected ? " · 置信度中等" : "",
        !strcmp(mode, "crop") ? "切掉含字幕的部分" : "只模糊字幕带，画面其余部分不动",
        sw, sh, out, st.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "已去字幕（%s，%dx%d 保留）", region, sw, sh);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld,"
             "\"region\":\"%s\",\"detected\":%d", out, out, (long)st.st_size, region, detected);
    return json_pair(text, ans, extra);
}

/* ---------------- 入口 ---------------- */

/* 给结果 JSON 注入 "src":<源文件>，前端才能把原片和处理后的视频并排显示。
 * 只对真正产出了 mp4 的结果生效（file+format:mp4 都在）。 */
static char *with_src(char *res) {
    if (!res || !g_src_path[0]) return res;
    if (strstr(res, "\"src\":")) return res;
    if (!strstr(res, "\"format\":\"mp4\"") || !strstr(res, "\"file\":")) return res;
    const char *anchor = strstr(res, "\"format\":");
    if (!anchor) return res;
    size_t head = (size_t)(anchor - res);
    size_t cap = strlen(res) + strlen(g_src_path) + 64;
    char *out = malloc(cap);
    if (!out) return res;
    snprintf(out, cap, "%.*s\"src\":\"%s\",%s", (int)head, res, g_src_path, anchor);
    free(res);
    return out;
}
#define RET(x) do { char *r__ = (x); return with_src(r__); } while (0)

/* 画面优化（2 倍超分）：Real-ESRGAN animevideov3 跑 RKNPU。
 *
 * 和去字幕完全独立：去字幕是"抹掉字并重建那块画面"，这个是"把整幅画面变清晰"。
 * 按源片宽高比自动选模型（4:3 → 288x216 块，16:9 → 320x180 块），
 * 切块全部是原图裁剪（不缩放），每块 4 倍，拼完缩 2 倍 → 输出 2 倍。
 * 只针对 SD 级源：720p 以上是高清，跑这个只会更慢、不会更好。
 */
static char *op_sr(const char *json) {
    g_cur_kind = "sr"; g_cur_label = "画面优化";
    char p[768] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) return errf("缺少 path（要处理的视频）%s%s", "", "");

    char path[900];
    resolve_path(p, path, sizeof(path));
    if (!exists(path)) return errf("文件不存在：%s%s", path, "");

    int sw = 0, sh = 0;
    probe_size(path, &sw, &sh);
    if (!sw || !sh) return errf("读不到分辨率%s%s", "", "");
    if (sw > 1280 || sh > 720) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "源片 %dx%d 已经是高清，画面优化只针对 SD 源（720p 以下）", sw, sh);
        return errf("%s%s", msg, "");
    }

    const char *py = "/opt/sr-rk3566/sr-rk3566.py";
    if (access(py, R_OK) != 0)
        return errf("画面优化未安装（需要 sr-rk3566 包提供 /opt/sr-rk3566/sr-rk3566.py）%s%s", "", "");

    const char *mname = (sw * 9 == sh * 16) ? "16:9" : "4:3";
    const char *tile = (sw * 9 == sh * 16) ? "320x180" : "288x216";

    char out[900];
    out_name(path, "sr2x", "mp4", out, sizeof(out));

    double dur = duration_of(path);
    double fps = fps_of(path);
    if (fps <= 1) fps = 25;
    int nfr = (int)(dur * fps + 0.5);
    if (nfr < 1) nfr = 1;

    char id[64], prog[512], logf[512], cmd[1024], text[800], ans[240], extra[520];
    snprintf(id, sizeof(id), "sr_%ld", (long)time(NULL));
    mkdir("/tmp/llama_agent", 0777);
    mkdir(VJOBS, 0777);
    snprintf(prog, sizeof(prog), "%s/%s.prog", VJOBS, id);
    snprintf(logf, sizeof(logf), "%s/%s.log", VJOBS, id);
    snprintf(cmd, sizeof(cmd), "python3 %s '%s' '%s'", py, path, out);

    /* 实测 5.15 秒/帧（576x432 源 + 576x432 大模型 = 1 块，2026-11 实测 25 帧 139 秒）。
     * 源片比模型还大时会自动切块，那种情况会慢 2 倍以上，这里的估算只覆盖常见情况。 */
    double est_min = nfr * 5.2 / 60.0;

    int want_async = (int)jget_int(json, "async", 1);
    if (want_async) {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int fd = open(logf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        if (pid < 0) return errf("启动画面优化任务失败%s%s", "", "");
        g_job_fps = fps;
        job_record(id, (int)pid, out, dur, "画面优化");
        snprintf(text, sizeof(text),
                 "**画面优化已开始（后台）**\n\n| 项 | 值 |\n|---|---|\n"
                 "| 任务 ID | `%s` |\n| 源片 | `%dx%d`（%s，切块 %s）|\n"
                 "| 引擎 | Real-ESRGAN animevideov3 on RKNPU |\n"
                 "| 输出 | `%dx%d` |\n| 帧数 | %d 帧（约 %.0f 分钟）|\n| 文件 | `%s` |\n",
                 id, sw, sh, mname, tile, sw * 2, sh * 2, nfr, est_min, out);
        snprintf(ans, sizeof(ans),
                 "画面优化已开始（%d 帧，约 %.0f 分钟），用 job 查进度", nfr, est_min);
        snprintf(extra, sizeof(extra),
                 "\"id\":\"%s\",\"async\":1,\"pid\":%d,\"out\":\"%s\","
                 "\"total_sec\":%.1f,\"kind\":\"画面优化\"",
                 id, (int)pid, out, dur);
        return json_pair(text, ans, extra);
    }

    int rc = run_pipe_sync(cmd);
    if (rc != 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "画面优化失败（退出码 %d），见 ", rc);
        return errf("%s%s", msg, logf);
    }
    struct stat sts;
    memset(&sts, 0, sizeof(sts));
    stat(out, &sts);
    snprintf(text, sizeof(text),
             "**画面优化完成**\n\n| 项 | 值 |\n|---|---|\n"
             "| 源片 | `%dx%d` → `%dx%d` |\n| 引擎 | Real-ESRGAN animevideov3（NPU）|\n"
             "| 文件 | `%s`（%.1f MB）|\n",
             sw, sh, sw * 2, sh * 2, out, sts.st_size / 1048576.0);
    snprintf(ans, sizeof(ans), "画面优化完成，%.1f MB", sts.st_size / 1048576.0);
    snprintf(extra, sizeof(extra),
             "\"path\":\"%s\",\"file\":\"%s\",\"format\":\"mp4\",\"size\":%ld",
             out, out, (long)sts.st_size);
    return json_pair(text, ans, extra);
}

static char *run(const char *json_in) {
    char op[32] = {0};
    g_cur_json = json_in;
    g_src_path[0] = 0;
    jget(json_in, "path", g_src_path, sizeof(g_src_path));
    jget(json_in, "op", op, sizeof(op));
    if (!op[0]) snprintf(op, sizeof(op), "info");

    if (!strcmp(op, "hw") || !strcmp(op, "caps")) return op_hw();
    if (!strcmp(op, "info") || !strcmp(op, "probe") || !strcmp(op, "video_info") || !strcmp(op, "vinfo")) RET(op_info(json_in));
    if (!strcmp(op, "portrait") || !strcmp(op, "vertical") || !strcmp(op, "shupin"))
        RET(op_portrait(json_in));
    if (!strcmp(op, "effects") || !strcmp(op, "fx")) RET(op_effects(json_in));
    if (!strcmp(op, "trim") || !strcmp(op, "clip")) RET(op_trim(json_in));
    if (!strcmp(op, "thumb") || !strcmp(op, "frame")) RET(op_thumb(json_in));
    if (!strcmp(op, "gif")) RET(op_gif(json_in));
    if (!strcmp(op, "compress") || !strcmp(op, "small")) RET(op_compress(json_in));
    if (!strcmp(op, "audio") || !strcmp(op, "extract_audio")) RET(op_audio(json_in));
    if (!strcmp(op, "watermark") || !strcmp(op, "logo")) RET(op_watermark(json_in));
    if (!strcmp(op, "crop") || !strcmp(op, "scale")) RET(op_crop_scale(json_in));
    if (!strcmp(op, "concat") || !strcmp(op, "merge")) RET(op_concat(json_in));
    if (!strcmp(op, "split") || !strcmp(op, "segment")) RET(op_split(json_in));
    if (!strcmp(op, "sheet") || !strcmp(op, "preview")) RET(op_sheet(json_in));
    if (!strcmp(op, "convert") || !strcmp(op, "remux")) RET(op_convert(json_in));
    if (!strcmp(op, "detect") || !strcmp(op, "analyze")) RET(op_detect(json_in));
    if (!strcmp(op, "job") || !strcmp(op, "progress")) RET(op_job(json_in));
    if (!strcmp(op, "desub") || !strcmp(op, "delogo") || !strcmp(op, "nsub"))
        RET(op_desub(json_in));
    if (!strcmp(op, "sr") || !strcmp(op, "upscale") || !strcmp(op, "enhance") ||
        !strcmp(op, "qingxi") || !strcmp(op, "sr2x"))
        RET(op_sr(json_in));

    return errf("不支持的 op：%s。video 支持 hw / info / portrait / effects / trim / thumb / gif %s",
                op, "");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "video",
    "视频：portrait竖屏(crop/blur/pad) effects特效(黑白=grayscale:1 淡入=fade_in:2 "
    "变速=speed:0.5 调色=brightness/contrast/saturation 暗角=vignette:1 镜像=hflip:1 "
    "文字=text) compress压缩 audio音轨 watermark图片水印 crop/scale crop concat合并 "
    "split切片 sheet缩略图 convert转封装 detect检测 trim截取 thumb抽帧 gif job查进度 "
    "sr画面优化(2倍超分,只对SD源) desub去字幕",
    "{\"op\":\"portrait\",\"path\":\"/opt/mp4/01.mp4\",\"mode\":\"blur\"}"
    "  或 {\"op\":\"effects\",\"path\":\"/opt/mp4/01.mp4\",\"fade_in\":2,\"brightness\":0.05,\"text\":\"示例\"}"
    "  或 {\"op\":\"hw\"}",
    run
};

plugin_desc_t *plugin_entry(void) { return &desc; }
