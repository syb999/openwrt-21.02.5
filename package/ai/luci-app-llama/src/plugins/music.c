/* music.c — 音乐：MIDI 生成 / 读取 / FC 音色合成（纯 C）
 *
 * 音色参数全部照搬 Python 版 fc_synth.py 的 A/B 试听结论：
 *   SR=32000      22050 会把方波高次谐波砍掉，听着发闷
 *   pulse duty=0.5 主旋律（必须减直流，否则窄占空会累积巨大 DC 偏移）
 *   triangle       低音通道
 *   noise+exp decay 鼓
 *   ADSR a=1ms d=75ms s=42% r=30ms  瞬时起音才有"拨弦/打击"颗粒感
 *   vibrato 0.006 @5.5Hz            FC 那种轻微抖
 *
 * op：
 *   midi    {"path","notes":[{pitch,dur}],"tempo","tracks"}  生成 .mid
 *   render  {"path":"a.mid"} 或 {"out":"a.mp3","notes":[...]} 合成 mp3
 *   import  {"path":"a.mid"}                                  读现有 MIDI 再合成
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "plugin.h"

#define SR        32000
#define MAX_NOTE  4096
#define WORK_DIR  "/tmp/llama_agent"

typedef struct {
    int pitch;        /* MIDI 音高 0-127 */
    double start;     /* 秒 */
    double dur;       /* 秒 */
    int ch;           /* 0=旋律 1=低音 9=鼓 */
} note_t;

/* ---------------- JSON 取值 ---------------- */
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
        if (*p == '\\' && p[1]) { p++; out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

static double jget_num(const char *js, const char *key, double dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atof(p + strlen(pat)) : dflt;
}

static char *jerr(const char *msg) {
    size_t cap = strlen(msg) * 2 + 64;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = (size_t)snprintf(o, cap, "{\"error\":\"");
    for (const char *p = msg; *p && i + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

static char *jpair(const char *text, const char *answer) {
    size_t cap = strlen(text) * 2 + strlen(answer) * 2 + 256;
    char *o = malloc(cap);
    if (!o) return NULL;
    size_t i = 0;
    i += (size_t)snprintf(o + i, cap - i, "{\"value\":\"");
    for (const char *p = text; *p && i + 12 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else if (*p == '\r') { }
        else if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
        else o[i++] = *p;
    }
    i += (size_t)snprintf(o + i, cap - i, "\",\"answer\":\"");
    for (const char *p = answer; *p && i + 12 < cap; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if ((unsigned char)*p < 0x20) { o[i++] = ' '; }
        else o[i++] = *p;
    }
    snprintf(o + i, cap - i, "\"}");
    return o;
}

static void resolve_path(const char *in, char *out, size_t cap) {
    if (!in || !in[0]) { snprintf(out, cap, "%s", WORK_DIR); return; }
    if (in[0] == '/') snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "%s/%s", WORK_DIR, in);
}

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
}

/* ---------------- 解析 notes 数组 ----------------
 * 接受 [{"pitch":69,"dur":1}, ...]，dur 以"拍"为单位（1 = 四分音符）。
 */
static int parse_notes(const char *js, note_t *out, int maxn, double beat, int ch) {
    const char *p = js;
    int n = 0;
    double t = 0;
    while ((p = strstr(p, "\"pitch\"")) != NULL && n < maxn) {
        const char *c = strchr(p, ':');
        if (!c) break;
        int pitch = atoi(c + 1);
        double dur_beats = 1.0;
        const char *d = strstr(p, "\"dur\"");
        const char *brace = strchr(p, '}');
        if (d && (!brace || d < brace)) {
            const char *c2 = strchr(d, ':');
            if (c2) dur_beats = atof(c2 + 1);
        }
        if (pitch > 0 && pitch < 128) {
            out[n].pitch = pitch;
            out[n].start = t;
            out[n].dur = dur_beats * beat;
            out[n].ch = ch;
            t += dur_beats * beat;
            n++;
        } else if (pitch == 0) {
            t += dur_beats * beat;      /* 休止符 */
        }
        p = brace ? brace : p + 7;
    }
    return n;
}


/* 按方括号截取某一条轨（melody / bass / drums）单独解析。
 *
 * 为什么必须截：parse_notes 里的时间 t 是【顺序累加】的。整段 JSON 一次
 * 喂进去，melody 播完 bass 接着播、bass 播完 drums 接着播 —— 三轨首尾相连，
 * 总时长直接翻两三倍（8 小节 29 秒的曲子渲染出 59 秒）。
 * 每轨单独解析，各自的 start 都从 0 起，才是并行的多轨。
 */
static int parse_track(const char *js, const char *key, note_t *out, int maxn,
                       double beat, int ch) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(js, pat);
    if (!k) return 0;
    const char *b = strchr(k, '[');
    if (!b) return 0;
    int depth = 0;
    const char *e = b;
    for (; *e; e++) {
        if (*e == '[') depth++;
        else if (*e == ']') { depth--; if (depth == 0) { e++; break; } }
    }
    size_t len = (size_t)(e - b);
    if (len > 60000) len = 60000;
    char *seg = malloc(len + 1);
    if (!seg) return 0;
    memcpy(seg, b, len);
    seg[len] = 0;
    int n = parse_notes(seg, out, maxn, beat, ch);
    free(seg);
    return n;
}

/* ---------------- MIDI 写 ---------------- */
static void wr32(FILE *f, unsigned v) {
    fputc((int)((v >> 24) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)(v & 0xFF), f);
}

static void wr_var(FILE *f, unsigned v) {
    unsigned char buf[5];
    int n = 0;
    buf[n++] = (unsigned char)(v & 0x7F);
    v >>= 7;
    while (v) {
        buf[n++] = (unsigned char)((v & 0x7F) | 0x80);
        v >>= 7;
    }
    while (n--) fputc(buf[n], f);
}

/* 把一个通道的音符写成一条 MTrk。
 * 做法：把 note-on / note-off 事件按时间排序（同一时刻 off 在前，
 * 避免同音高连打时被误当成延音）。 */
typedef struct { long tick; int on; int pitch; } ev_t;

static int cmp_ev(const void *a, const void *b) {
    const ev_t *x = a, *y = b;
    if (x->tick != y->tick) return (x->tick < y->tick) ? -1 : 1;
    return x->on - y->on;      /* 0(off) 先于 1(on) */
}

static void write_track(FILE *f, note_t *notes, int n, int ch, int div, double beat,
                        int with_tempo, unsigned us_per_beat, int prog) {
    ev_t ev[MAX_NOTE * 2];
    int ne = 0;
    for (int i = 0; i < n && ne < MAX_NOTE * 2 - 2; i++) {
        long t0 = (long)(notes[i].start / beat * div + 0.5);
        long t1 = t0 + (long)(notes[i].dur / beat * div + 0.5);
        if (t1 <= t0) t1 = t0 + div / 4;
        ev[ne].tick = t0; ev[ne].on = 1; ev[ne].pitch = notes[i].pitch; ne++;
        ev[ne].tick = t1; ev[ne].on = 0; ev[ne].pitch = notes[i].pitch; ne++;
    }
    qsort(ev, (size_t)ne, sizeof(ev[0]), cmp_ev);

    /* 先写进内存，才知道长度 */
    unsigned char *buf = malloc((size_t)ne * 8 + 64);
    if (!buf) return;
    size_t len = 0;
    /* 音色：program change（0xC0|ch, prog）。以前从来没写过，所以所有风格都是同一个默认音色 */
    if (prog >= 0 && ch != 9) {
        buf[len++] = 0x00;                              /* delta time 0 */
        buf[len++] = (unsigned char)(0xC0 | (ch & 0x0F));
        buf[len++] = (unsigned char)(prog & 0x7F);
    }
    long last = 0;

    /* 速度事件必须写，否则任何软件读回来都按默认 120 BPM */
    if (with_tempo) {
        buf[len++] = 0x00;                       /* delta 0 */
        buf[len++] = 0xFF; buf[len++] = 0x51; buf[len++] = 0x03;
        buf[len++] = (unsigned char)((us_per_beat >> 16) & 0xFF);
        buf[len++] = (unsigned char)((us_per_beat >> 8) & 0xFF);
        buf[len++] = (unsigned char)(us_per_beat & 0xFF);
    }
    for (int i = 0; i < ne; i++) {
        unsigned delta = (unsigned)(ev[i].tick - last);
        last = ev[i].tick;
        /* 变长编码 */
        unsigned char vb[5];
        int vn = 0;
        vb[vn++] = (unsigned char)(delta & 0x7F);
        delta >>= 7;
        while (delta) { vb[vn++] = (unsigned char)((delta & 0x7F) | 0x80); delta >>= 7; }
        while (vn--) buf[len++] = vb[vn];

        int is_drum = (ch == 9);
        buf[len++] = (unsigned char)((ev[i].on ? 0x90 : 0x80) | (is_drum ? 9 : ch));
        buf[len++] = (unsigned char)(ev[i].pitch & 0x7F);
        buf[len++] = (unsigned char)(ev[i].on ? 100 : 0);
    }
    /* end of track */
    buf[len++] = 0x00; buf[len++] = 0xFF; buf[len++] = 0x2F; buf[len++] = 0x00;

    fwrite("MTrk", 1, 4, f);
    wr32(f, (unsigned)len);
    fwrite(buf, 1, len, f);
    free(buf);
}

static char *op_midi(const char *json) {
    char p[512] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) snprintf(p, sizeof(p), "music.mid");
    char path[768];
    resolve_path(p, path, sizeof(path));
    if (!strstr(path, ".mid")) strncat(path, ".mid", sizeof(path) - strlen(path) - 1);
    mkdirs(path);

    double tempo = jget_num(json, "tempo", 160);
    if (tempo < 40 || tempo > 260) tempo = 160;
    double beat = 60.0 / tempo;        /* 一拍几秒 */
    int div = 96;                      /* 每拍 96 tick */

    /* notes 可以是单组，也可以是 {"melody":[...],"bass":[...]} */
    note_t *notes = malloc(sizeof(note_t) * MAX_NOTE);
    if (!notes) return NULL;
    int total = 0;

    /* 先看有没有分轨写法 */
    const char *mp = strstr(json, "\"melody\"");
    const char *bp = strstr(json, "\"bass\"");
    const char *dp = strstr(json, "\"drums\"");
    int ntrk = 0;

    if (mp || bp || dp) {
        if (mp) { total += parse_track(json, "melody", notes + total, MAX_NOTE - total, beat, 0); ntrk++; }
        if (bp) { total += parse_track(json, "bass",   notes + total, MAX_NOTE - total, beat, 1); ntrk++; }
        if (dp) { total += parse_track(json, "drums",  notes + total, MAX_NOTE - total, beat, 9); ntrk++; }
        if (!mp && strstr(json, "\"notes\""))
            total += parse_track(json, "notes", notes + total, MAX_NOTE - total, beat, 0);
    } else {
        total = parse_notes(json, notes, MAX_NOTE, beat, 0);
        ntrk = 1;
    }

    if (total == 0) { free(notes); return jerr("没有音符：需要 notes:[{\"pitch\":69,\"dur\":1}]"); }

    FILE *f = fopen(path, "wb");
    if (!f) { free(notes); return jerr("写不了 MIDI 文件"); }

    /* 头：format 1（多轨同步） */
    fwrite("MThd", 1, 4, f);
    wr32(f, 6);
    fputc(0, f); fputc(1, f);                 /* format=1 */
    fputc(0, f); fputc((int)(ntrk > 1 ? ntrk : 1), f);
    fputc((int)((div >> 8) & 0xFF), f); fputc((int)(div & 0xFF), f);

    /* 每个通道一条轨 */
    /* 注意：jget 只取字符串值，	"program":0 是数字，必须自己按数字解析 */
    int prog_mel = -1;
    {
        const char *q = strstr(json, "\"program\":");
        if (q) {
            q += 10;
            while (*q == ' ') q++;
            if (*q >= '0' && *q <= '9') prog_mel = atoi(q);
        }
    }
    int prog_bass = 33;                                   /* 默认指弹贝斯 */
    int first_trk = 0;
    for (int c = 0; c < 3; c++) {
        int want = (c == 0) ? 0 : (c == 1) ? 1 : 9;
        note_t tmp[MAX_NOTE];
        int nn = 0;
        for (int i = 0; i < total; i++)
            if (notes[i].ch == want && nn < MAX_NOTE) tmp[nn++] = notes[i];
        if (nn == 0) continue;
        unsigned us = (unsigned)(60000000.0 / tempo);
        int pg = (c == 0) ? prog_mel : (c == 1 ? prog_bass : -1);
        write_track(f, tmp, nn, want, div, beat, (first_trk++ == 0), us, pg);
    }
    fclose(f);
    free(notes);

    struct stat st;
    stat(path, &st);
    char text[600];
    snprintf(text, sizeof(text),
        "已生成 MIDI：`%s`\n\n| 项 | 值 |\n|---|---|\n"
        "| 音符数 | %d |\n| 轨数 | %d |\n| 速度 | %.0f BPM |\n| 大小 | %lld 字节 |\n",
        path, total, ntrk > 1 ? ntrk : 1, tempo, (long long)st.st_size);
    char ans[220];
    snprintf(ans, sizeof(ans), "已生成 MIDI（%d 个音符，%.0f BPM）：%s", total, tempo, path);
    return jpair(text, ans);
}

/* ---------------- MIDI 读 ---------------- */
static unsigned rd32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static unsigned rd_var(const unsigned char *p, int *used) {
    unsigned v = 0;
    int i = 0;
    while (i < 4) {
        unsigned char c = p[i++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    *used = i;
    return v;
}

/* 解析 MIDI → notes。返回音符数，失败返回 -1 */
static int read_midi(const char *path, note_t *out, int maxn, double *beat_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 14 || sz > 8 * 1024 * 1024) { fclose(f); return -1; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);

    if (memcmp(buf, "MThd", 4) != 0) { free(buf); return -1; }
    int div = (buf[12] << 8) | buf[13];
    if (div <= 0) div = 96;
    /* tempo 默认 120 → 一拍 0.5s；有 FF 51 会覆盖 */
    double beat = 0.5;

    int n = 0;
    long pos = 8 + (long)rd32(buf + 4);
    /* 正在响的音：按 (ch,pitch) 记起始 tick */
    long on_tick[16][128];
    for (int c = 0; c < 16; c++)
        for (int k = 0; k < 128; k++) on_tick[c][k] = -1;

    while (pos + 8 <= sz) {
        if (memcmp(buf + pos, "MTrk", 4) != 0) break;
        unsigned tlen = rd32(buf + pos + 4);
        long p = pos + 8;
        long end = p + (long)tlen;
        if (end > sz) end = sz;

        unsigned running = 0;
        long tick = 0;
        while (p < end) {
            int used = 0;
            unsigned delta = rd_var(buf + p, &used);
            p += used;
            tick += delta;
            if (p >= end) break;
            unsigned char st = buf[p];
            if (st & 0x80) {
                p++;
                if (st < 0xF0) running = st;
            } else {
                st = running;            /* running status */
            }
            if (st == 0xFF) {            /* meta */
                if (p >= end) break;
                unsigned char type = buf[p++];
                int u2 = 0;
                unsigned len = rd_var(buf + p, &u2);
                p += u2;
                if (type == 0x51 && len == 3 && p + 2 < end) {
                    unsigned us = ((unsigned)buf[p] << 16) | ((unsigned)buf[p + 1] << 8) | buf[p + 2];
                    if (us) beat = us / 1000000.0;
                }
                p += (long)len;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                int u2 = 0;
                unsigned len = rd_var(buf + p, &u2);
                p += u2 + (long)len;
                continue;
            }

            int ch = (int)(st & 0x0F);
            int hi = (int)(st & 0xF0);
            if (hi == 0x90 || hi == 0x80) {
                if (p + 1 >= end) break;
                int pitch = buf[p] & 0x7F;
                int vel = buf[p + 1] & 0x7F;
                p += 2;
                if (hi == 0x90 && vel > 0) {
                    on_tick[ch][pitch] = tick;
                } else if (on_tick[ch][pitch] >= 0) {
                    long t0 = on_tick[ch][pitch];
                    on_tick[ch][pitch] = -1;
                    if (n < maxn) {
                        out[n].pitch = pitch;
                        out[n].start = (double)t0 / div * beat;
                        out[n].dur = (double)(tick - t0) / div * beat;
                        out[n].ch = (ch == 9) ? 9 : (ch == 1 ? 1 : 0);
                        n++;
                    }
                }
            } else if (hi == 0xC0 || hi == 0xD0) {
                p += 1;
            } else {
                p += 2;
            }
        }
        pos = pos + 8 + (long)tlen;
    }
    free(buf);
    if (beat_out) *beat_out = beat;
    return n;
}

/* ---------------- 合成 ---------------- */
/* FC 音色参数（照搬 fc_synth.py 的试听结论） */
static double adsr_env(long i, long n) {
    const double a = 0.001, d = 0.075, s = 0.42, r = 0.030;
    long an = (long)(a * SR); if (an < 1) an = 1;
    long dn = (long)(d * SR); if (dn < 1) dn = 1;
    long rn = (long)(r * SR); if (rn < 1) rn = 1;
    long sus = n - an - dn - rn;
    if (sus < 0) sus = 0;
    if (i < an) return (double)i / an;
    if (i < an + dn) {
        double k = (double)(i - an) / dn;
        return 1.0 + (s - 1.0) * k;
    }
    if (i < an + dn + sus) return s;
    {
        long ri = i - an - dn - sus;
        double k = (double)ri / rn;
        if (k > 1) k = 1;
        return s * (1.0 - k);
    }
}

static double note_freq(int n) {
    return 440.0 * pow(2.0, (n - 69) / 12.0);
}

static unsigned rng_state = 12345;
static double frand(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return ((rng_state >> 16) & 0x7FFF) / 16384.0 - 1.0;
}

static void add_tone(float *buf, long total, double start, int pitch, double dur,
                     double vol, int wave, double duty, double vib) {
    long s0 = (long)(start * SR);
    if (s0 < 0 || s0 >= total) return;
    long n = (long)(dur * SR);
    if (n <= 0) return;
    if (s0 + n > total) n = total - s0;

    double f0 = note_freq(pitch);
    double phase = 0;
    for (long i = 0; i < n; i++) {
        double t = (double)i / SR;
        double f = f0;
        if (vib > 0) f = f0 * (1.0 + vib * sin(2 * M_PI * 5.5 * t));
        phase += f / SR;
        if (phase >= 1.0) phase -= (long)phase;

        double w;
        if (wave == 0) {              /* pulse */
            w = (phase < duty) ? 1.0 : -1.0;
            w -= (2.0 * duty - 1.0);  /* 去直流 */
        } else {                      /* triangle */
            w = 2.0 * fabs(2.0 * phase - 1.0) - 1.0;
        }
        buf[s0 + i] += (float)(w * adsr_env(i, n) * vol);
    }
}

static void add_noise(float *buf, long total, double start, double dur, double vol) {
    long s0 = (long)(start * SR);
    if (s0 < 0 || s0 >= total) return;
    long n = (long)(dur * SR);
    if (n <= 0) return;
    if (s0 + n > total) n = total - s0;
    const double decay = 0.0018;
    for (long i = 0; i < n; i++) {
        double e = exp(-(double)i / (decay * SR));
        buf[s0 + i] += (float)(frand() * e * vol);
    }
}

static int write_wav(const char *path, float *buf, long total) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    unsigned data = (unsigned)(total * 2);
    fwrite("RIFF", 1, 4, f);
    unsigned riff = 36 + data;
    fputc((int)(riff & 0xFF), f); fputc((int)((riff >> 8) & 0xFF), f);
    fputc((int)((riff >> 16) & 0xFF), f); fputc((int)((riff >> 24) & 0xFF), f);
    fwrite("WAVEfmt ", 1, 8, f);
    unsigned fmtlen = 16;
    fputc(16, f); fputc(0, f); fputc(0, f); fputc(0, f);
    fputc(1, f); fputc(0, f);                     /* PCM */
    fputc(1, f); fputc(0, f);                     /* mono */
    unsigned sr = SR;
    fputc((int)(sr & 0xFF), f); fputc((int)((sr >> 8) & 0xFF), f);
    fputc((int)((sr >> 16) & 0xFF), f); fputc((int)((sr >> 24) & 0xFF), f);
    unsigned br = SR * 2;
    fputc((int)(br & 0xFF), f); fputc((int)((br >> 8) & 0xFF), f);
    fputc((int)((br >> 16) & 0xFF), f); fputc((int)((br >> 24) & 0xFF), f);
    fputc(2, f); fputc(0, f);                     /* block align */
    fputc(16, f); fputc(0, f);                    /* bits */
    fwrite("data", 1, 4, f);
    fputc((int)(data & 0xFF), f); fputc((int)((data >> 8) & 0xFF), f);
    fputc((int)((data >> 16) & 0xFF), f); fputc((int)((data >> 24) & 0xFF), f);
    (void)fmtlen;

    /* 归一化 + 转 16bit：不归一化会削波，整首都是破音 */
    float peak = 0.0001f;
    for (long i = 0; i < total; i++) {
        float a = fabsf(buf[i]);
        if (a > peak) peak = a;
    }
    float gain = (peak > 0.95f) ? (0.95f / peak) : 1.0f;
    for (long i = 0; i < total; i++) {
        int s = (int)(buf[i] * gain * 32000.0f);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        fputc(s & 0xFF, f);
        fputc((s >> 8) & 0xFF, f);
    }
    fclose(f);
    return 0;
}

static char *do_render(note_t *notes, int nn, const char *midi_path,
                       const char *out_name, double tempo) {
    if (nn <= 0) return jerr("没有音符可渲染");

    double end_t = 0;
    for (int i = 0; i < nn; i++) {
        double e = notes[i].start + notes[i].dur;
        if (e > end_t) end_t = e;
    }
    end_t += 1.0;                                /* 尾巴留 1 秒余音 */
    long total = (long)(end_t * SR);
    if (total <= 0 || total > (long)SR * 600) return jerr("曲子太长（上限 10 分钟）");

    float *buf = calloc((size_t)total, sizeof(float));
    if (!buf) return jerr("内存不足（曲子太长？）");

    for (int i = 0; i < nn; i++) {
        note_t *x = &notes[i];
        if (x->ch == 9) {
            add_noise(buf, total, x->start, x->dur, 0.35);
        } else if (x->ch == 1) {
            add_tone(buf, total, x->start, x->pitch, x->dur, 0.34, 1, 0.5, 0.0);
        } else {
            add_tone(buf, total, x->start, x->pitch, x->dur, 0.42, 0, 0.5, 0.006);
        }
    }

    char wav[900];
    if (midi_path && midi_path[0]) {
        snprintf(wav, sizeof(wav), "%s", midi_path);
    } else {
        resolve_path((out_name && out_name[0]) ? out_name : "music", wav, sizeof(wav));
    }
    char *dot = strrchr(wav, '.');
    if (dot && (!strcmp(dot, ".mid") || !strcmp(dot, ".midi"))) *dot = 0;
    strncat(wav, ".wav", sizeof(wav) - strlen(wav) - 1);
    mkdirs(wav);

    if (write_wav(wav, buf, total) != 0) {
        free(buf);
        return jerr("写 WAV 失败");
    }
    free(buf);

    /* WAV → MP3：板上 ffmpeg 有 mp3 编码器，比自己实现编码器实在 */
    char mp3[900];
    snprintf(mp3, sizeof(mp3), "%s", wav);
    dot = strrchr(mp3, '.');
    if (dot) { *dot = 0; }
    strncat(mp3, ".mp3", sizeof(mp3) - strlen(mp3) - 1);

    char cmd[2000];
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/ffmpeg -y -loglevel error -i '%s' -codec:a libmp3lame -b:a 128k '%s' 2>&1",
        wav, mp3);
    int rc = system(cmd);

    struct stat st;
    long long mp3sz = 0;
    if (stat(mp3, &st) == 0) mp3sz = (long long)st.st_size;

    char text[1200];
    snprintf(text, sizeof(text),
        "**渲染完成**\n\n| 项 | 值 |\n|---|---|\n"
        "| 音符 | %d |\n| 时长 | %.1f 秒 |\n| 速度 | %.0f BPM |\n"
        "| WAV | `%s` |\n| MP3 | %s |\n",
        nn, end_t - 1.0, tempo, wav,
        mp3sz > 0 ? mp3 : "（ffmpeg 没生成，只有 WAV）");
    char ans[300];
    if (mp3sz > 0)
        snprintf(ans, sizeof(ans), "已合成 MP3（%d 个音符，%.1f 秒）：%s", nn, end_t - 1.0, mp3);
    else
        snprintf(ans, sizeof(ans), "已合成 WAV（%d 个音符，%.1f 秒）：%s", nn, end_t - 1.0, wav);
    (void)rc;
    return jpair(text, ans);
}

static char *op_render(const char *json) {
    char p[512] = {0}, out[512] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "out", out, sizeof(out));
    double tempo = jget_num(json, "tempo", 160);

    /* 给了 path 又有 notes：直接按 notes 渲染；只给 path：读 MIDI */
    note_t *notes = malloc(sizeof(note_t) * MAX_NOTE);
    if (!notes) return NULL;

    int nn = 0;
    double beat = 60.0 / tempo;
    if (strstr(json, "\"melody\"") || strstr(json, "\"notes\"")) {
        nn  = parse_track(json, "melody", notes, MAX_NOTE, beat, 0);
        nn += parse_track(json, "notes",  notes + nn, MAX_NOTE - nn, beat, 0);
        nn += parse_track(json, "bass",   notes + nn, MAX_NOTE - nn, beat, 1);
        nn += parse_track(json, "drums",  notes + nn, MAX_NOTE - nn, beat, 9);
    }
    char midi_path[768] = {0};
    if (p[0]) {
        resolve_path(p, midi_path, sizeof(midi_path));
        if (!nn && strstr(midi_path, ".mid")) {
            double b = beat;
            nn = read_midi(midi_path, notes, MAX_NOTE, &b);
            if (b > 0) beat = b;
        }
    }
    char *r = do_render(notes, nn, midi_path, out, 60.0 / beat);
    free(notes);
    return r;
}

static char *op_import(const char *json) {
    char p[512] = {0};
    jget(json, "path", p, sizeof(p));
    if (!p[0]) return jerr("缺少 path（要导入的 .mid）");
    char path[768];
    resolve_path(p, path, sizeof(path));

    note_t *notes = malloc(sizeof(note_t) * MAX_NOTE);
    if (!notes) return NULL;
    double beat = 0.5;
    int nn = read_midi(path, notes, MAX_NOTE, &beat);
    if (nn < 0) { free(notes); return jerr("读不了这个 MIDI（格式不对或文件坏）"); }
    if (nn == 0) { free(notes); return jerr("MIDI 里没有音符"); }

    char *r = do_render(notes, nn, path, NULL, 60.0 / beat);
    free(notes);
    return r;
}

/* ---------------- op: bgm（风格模板生成） ----------------
 *
 * 为什么不让模型编音符：3B 每次给的 pitch/dur 都不同，一段 8 秒的 BGM
 * 要调三四次 music 才凑合，动辄五六分钟，还不一定好听。风格→音阶/节奏/
 * 低音/鼓 是固定的乐理套路，用模板 + 固定种子生成，秒级出结果且可复现。
 */
typedef struct {
    const char *id;        /* 内部名 */
    const char *keys;      /* 中文/英文关键字，| 分隔 */
    int tempo;
    int root;              /* 主音 MIDI */
    const int *scale;      /* 音阶 */
    int slen;
    int drums;             /* 0/1 */
    int density;           /* 每小节音符数 4/6/8 */
    int mood;              /* 0=明亮 1=暗淡（决定上行/下行偏好） */
} bgm_style_t;

static const int SC_MAJOR[]  = {0, 2, 4, 5, 7, 9, 11};
static const int SC_MINOR[]  = {0, 2, 3, 5, 7, 8, 10};
static const int SC_PENTA[]  = {0, 2, 4, 7, 9};
static const int SC_DORIAN[] = {0, 2, 3, 5, 7, 9, 10};

static const bgm_style_t BGM_STYLES[] = {
    /* id        关键字                                             tempo root scale     slen drums dens mood */
    {"happy",   "欢快|轻松|明快|愉快|happy|upbeat",                      138, 60, SC_MAJOR,  7, 1, 8, 0},
    {"calm",    "舒缓|安静|温柔|放松|calm|chill|ambient",                76,  57, SC_MAJOR,  7, 0, 4, 0},
    {"sad",     "悲伤|忧伤|难过|伤感|sad|melancholy",                    66,  57, SC_MINOR,  7, 0, 4, 1},
    {"tense",   "紧张|紧迫|悬疑|危机|tense|thriller",                    150, 52, SC_MINOR,  7, 1, 8, 1},
    {"mystery", "神秘|诡异|悬疑感|mystery|eerie",                        84,  50, SC_DORIAN, 7, 0, 6, 1},
    {"electronic","电子|科技|赛博|electronic|synth|cyber",               128, 48, SC_MINOR,  7, 1, 8, 0},
    {"game",    "8bit|8位|游戏|像素|红白机|game|chiptune",               160, 60, SC_MAJOR,  7, 1, 8, 0},
    {"march",   "进行曲|激昂|heroic|march|epic",                        120, 55, SC_MAJOR,  7, 1, 6, 0},
    {"chinese", "中国风|古风|五声|chinese|oriental",                     92,  60, SC_PENTA,  5, 0, 6, 0},
    {NULL, NULL, 0, 0, NULL, 0, 0, 0, 0}
};

/* 固定种子的 LCG：同样的风格 + 小节数，每次生成结果一致（可复现） */
static unsigned int bgm_rand(unsigned int *st) {
    *st = (*st * 1103515245u + 12345u) & 0x7fffffffu;
    return *st;
}


/* ---------- 和弦进行（用音阶级数表示，0 基）----------
 * 只靠"音阶内随机走音"听起来是散的；真正的曲子靠和弦骨架 + 动机重复。
 * 这里给每个调式几套常见进行（I-V-vi-IV、vi-IV-I-V、ii-V-I 等），
 * 主和弦在最后一小节回归，形成终止感。 */
static const int PROG_MAJOR[3][8] = {
    {0, 4, 5, 3, 0, 4, 5, 3},        /* I-V-vi-IV */
    {0, 5, 3, 4, 0, 5, 3, 4},        /* I-vi-IV-V */
    {3, 0, 4, 5, 3, 0, 4, 5},        /* IV-I-V-vi */
};
static const int PROG_MINOR[3][8] = {
    {0, 5, 2, 6, 0, 5, 2, 6},        /* i-VI-III-VII */
    {0, 3, 5, 4, 0, 3, 5, 4},        /* i-iv-VI-V */
    {0, 6, 5, 4, 0, 6, 5, 4},        /* i-VII-VI-V */
};
static const int PROG_DORIAN[2][8] = {
    {0, 3, 0, 6, 0, 3, 6, 3},
    {0, 6, 3, 0, 0, 6, 3, 6},
};
static const int PROG_PENTA[2][8] = {
    {0, 3, 4, 0, 0, 4, 3, 0},
    {0, 4, 0, 3, 0, 4, 3, 4},
};


/* ---------- 经典乐句库（全部公版：巴洛克/古典/民谣，已过版权期）----------
 * 用法：随机挑一条，按当前调式移调后作为开头 2 小节的旋律素材，
 * 后面再接自动生成的部分 —— 这样既有"耳熟的动机"，又不是整段照抄。
 * deg 是按大调音阶级数（0=do），dur 单位是拍，mode: 0=大调 1=小调 2=五声 */
typedef struct {
    const char *name;
    const signed char *deg;
    const float *dur;
    int n;
    int mode;
} phrase_t;

/* 欢乐颂（贝多芬第九）3 3 4 5 5 4 3 2 1 1 2 3 3 2 2 */
static const signed char PH_ODE[]  = {2,2,3,4,4,3,2,1,0,0,1,2,2,1,1};
static const float PH_ODE_D[]      = {1,1,1,1,1,1,1,1,1,1,1,1,1.5f,0.5f,2};
/* 小星星 1 1 5 5 6 6 5 / 4 4 3 3 2 2 1 */
static const signed char PH_TWINKLE[] = {0,0,4,4,5,5,4,3,3,2,2,1,1};
static const float PH_TWINKLE_D[]     = {1,1,1,1,1,1,2,1,1,1,1,1,1};
/* 致爱丽丝开头（E D# E D# E B D C A）→ 用级数近似 2,#1,2,#1,2,6,1,0,5 */
static const signed char PH_ELISE[] = {2,2,2,2,2,6,1,0,5};
static const float PH_ELISE_D[]     = {0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,2};
/* 绿袖子（小调）0,2,3,4,5,4,3,2 … */
static const signed char PH_GREEN[] = {0,2,3,4,5,4,3,2,0,2,3,4,3,2,1};
static const float PH_GREEN_D[]     = {1,1,2,1,1,1,1,2,1,1,2,1,1,1,2};
/* 卡农主题（大调级进+跳进）0,4,2,5,3,6,4,0 */
static const signed char PH_CANON[] = {0,4,2,5,3,6,4,0,2,4,6,7};
static const float PH_CANON_D[]     = {2,2,2,2,2,2,2,2,1,1,1,1};
/* 茉莉花（五声）3,3,5,5,6,5,3,2,1,1,2,3 */
static const signed char PH_JASMINE[] = {3,3,5,5,6,5,3,2,1,1,2,3};
static const float PH_JASMINE_D[]     = {1,1,1,1,1,1,1,1,1,1,1,2};
/* 摇篮曲（勃拉姆斯）0,0,4,0,0,4,0,4,5,4,2 … */
static const signed char PH_LULLABY[] = {0,0,4,0,0,4,0,4,5,4,2,0};
static const float PH_LULLABY_D[]     = {1,1,2,1,1,2,1,1,1,1,3};


/* 友谊地久天长（苏格兰民谣 1788，公版）：5 1 1 1 3 2 1 → 以 sol(4) 起头 */
static const signed char PH_AULD[] = {4,0,0,0,2,1,0,4,0,0,2,1};
static const float PH_AULD_D[]     = {1,1,1,2,1,1,3,1,1,1,2,2};
/* 铃儿响叮当（1857，公版）：3 3 3 | 3 3 3 | 3 5 1 2 3 → 以 mi(2) 起头 */
static const signed char PH_JINGLE[] = {2,2,2,2,2,2,2,4,0,1,2};
static const float PH_JINGLE_D[]     = {1,1,1,1,1,1,1,2,1,1,3};
/* 送别（旋律 Ordway 1851，公版）：5 3 5 1 7 1 6 → 以 sol(4) 起头 */
static const signed char PH_FAREWELL[] = {4,2,4,7,6,7,5,7,5,4};
static const float PH_FAREWELL_D[]     = {2,2,3,1,1,2,2,1,2,4};

static const phrase_t PHRASES[] = {
    {"欢乐颂",   PH_ODE,     PH_ODE_D,     15, 0},
    {"小星星",   PH_TWINKLE, PH_TWINKLE_D, 13, 0},
    {"致爱丽丝", PH_ELISE,   PH_ELISE_D,    9, 0},
    {"绿袖子",   PH_GREEN,   PH_GREEN_D,   15, 1},
    {"卡农",     PH_CANON,   PH_CANON_D,   12, 0},
    {"茉莉花",   PH_JASMINE, PH_JASMINE_D, 12, 2},
    {"摇篮曲",   PH_LULLABY, PH_LULLABY_D, 12, 0},
    {"友谊地久天长", PH_AULD,     PH_AULD_D,     12, 0},   /* sol 起 */
    {"铃儿响叮当",   PH_JINGLE,   PH_JINGLE_D,   11, 0},   /* mi 起 */
    {"送别",         PH_FAREWELL, PH_FAREWELL_D, 10, 0},   /* sol 起 */
};


/* 风格对应的 GM 音色号（0 基）：以前 MIDI 里从不写 program change，
 * 所以不管什么风格，播放器都用同一个默认音色 —— 这是"听起来都一个味"的主因之一。 */
static int style_program(const bgm_style_t *st) {
    if (!strcmp(st->id, "happy"))      return 0;    /* 大钢琴 */
    if (!strcmp(st->id, "calm"))       return 4;    /* 电钢琴 */
    if (!strcmp(st->id, "sad"))        return 42;   /* 大提琴 */
    if (!strcmp(st->id, "tense"))      return 81;   /* 合成 lead */
    if (!strcmp(st->id, "mystery"))    return 88;   /* Pad */
    if (!strcmp(st->id, "electronic")) return 80;   /* 方波 lead */
    if (!strcmp(st->id, "game"))       return 80;
    if (!strcmp(st->id, "march"))      return 56;   /* 小号 */
    if (!strcmp(st->id, "chinese"))    return 105;  /* 古筝 */
    return 0;
}

static const int *bgm_prog(const bgm_style_t *st, int *plen, unsigned int *seed) {
    unsigned int r = bgm_rand(seed);
    if (st->scale == SC_MINOR)  { *plen = 8; return PROG_MINOR[r % 3]; }
    if (st->scale == SC_DORIAN) { *plen = 8; return PROG_DORIAN[r % 2]; }
    if (st->scale == SC_PENTA)  { *plen = 8; return PROG_PENTA[r % 2]; }
    *plen = 8; return PROG_MAJOR[r % 3];
}

/* 取音阶第 deg 级的音高（deg 可以越界，自动上下八度） */
static int bgm_deg_pitch(const bgm_style_t *st, int deg) {
    int oct = 0;
    while (deg < 0) { deg += st->slen; oct--; }
    while (deg >= st->slen) { deg -= st->slen; oct++; }
    return st->root + st->scale[deg] + oct * 12;
}

/* 三和弦音（根/三/五） */
static void bgm_triad(const bgm_style_t *st, int root_deg, int *p3) {
    p3[0] = bgm_deg_pitch(st, root_deg);
    p3[1] = bgm_deg_pitch(st, root_deg + 2);
    p3[2] = bgm_deg_pitch(st, root_deg + 4);
}

static const bgm_style_t *bgm_pick(const char *style) {
    if (style && style[0]) {
        for (int i = 0; BGM_STYLES[i].id; i++)
            if (!strcasecmp(style, BGM_STYLES[i].id)) return &BGM_STYLES[i];
        for (int i = 0; BGM_STYLES[i].id; i++) {
            char keys[256];
            snprintf(keys, sizeof(keys), "%s", BGM_STYLES[i].keys);
            char *sv = NULL;
            for (char *k = strtok_r(keys, "|", &sv); k; k = strtok_r(NULL, "|", &sv))
                if (k[0] && strstr(style, k)) return &BGM_STYLES[i];
        }
    }
    return NULL;     /* 随意 → 由调用方挑一个 */
}

static char *op_bgm(const char *json) {
    char style[128] = {0}, p[512] = {0};
    jget(json, "style", style, sizeof(style));
    if (!style[0]) jget(json, "task", style, sizeof(style));   /* 允许直接传整句 */
    jget(json, "path", p, sizeof(p));

    const bgm_style_t *st = bgm_pick(style);
    int picked_at_random = 0;
    if (!st) {
        /* 随意 / 没指定：按当前秒数轮换，避免每次都同一个 */
        int n = 0;
        while (BGM_STYLES[n].id) n++;
        st = &BGM_STYLES[(int)(time(NULL) % n)];
        picked_at_random = 1;
    }

    long bars = (long)jget_num(json, "bars", 0);
    double tempo = jget_num(json, "tempo", st->tempo);
    if (tempo < 40 || tempo > 260) tempo = st->tempo;

    /* 时长：优先 seconds 字段，其次从整句里抠 "30秒" 这种写法。
     * 用户说"生成 30 秒的 BGM"，就得真给 30 秒 —— 以前完全不看这个数字。 */
    double want_sec = jget_num(json, "seconds", 0);
    if (want_sec <= 0 && style[0]) {
        const char *u = strstr(style, "秒");
        if (u) {
            const char *q = u;
            while (q > style && (q[-1] == ' ' || q[-1] == '\t')) q--;
            const char *end = q;
            while (q > style && q[-1] >= '0' && q[-1] <= '9') q--;
            if (end > q) {
                char num[16];
                size_t L = (size_t)(end - q);
                if (L < sizeof(num)) {
                    memcpy(num, q, L);
                    num[L] = 0;
                    want_sec = atof(num);
                }
            }
        }
    }

    if (bars <= 0) {
        if (want_sec > 0) {
            /* 一小节 4 拍 → 每小节 4*60/tempo 秒 */
            double bar_sec = 4.0 * 60.0 / tempo;
            bars = (long)(want_sec / bar_sec + 0.5);
        } else {
            bars = 8;
        }
    }
    if (bars < 1) bars = 1;
    if (bars > 64) bars = 64;
    /* 时长要准：bars 定下来后微调 tempo，使 bars*4*60/tempo ≈ want_sec。
     * 138→141 BPM 这种人耳几乎听不出，但总长能精确到 ±0.2 秒。 */
    if (want_sec > 0 && bars > 0) {
        double t2 = bars * 240.0 / want_sec;
        if (t2 >= 40.0 && t2 <= 220.0) tempo = t2;
    }
    if (want_sec > 0 && bars * 4.0 * 60.0 / tempo > want_sec * 1.15) {
        /* 宁可短一点也不超太多 */
        bars = (long)(want_sec * tempo / 240.0);
        if (bars < 1) bars = 1;
    }

    char base[512];
    if (p[0]) snprintf(base, sizeof(base), "%s", p);
    else snprintf(base, sizeof(base), "%s/bgm_%s_%ld", WORK_DIR, st->id, (long)time(NULL));
    /* 去掉用户可能带的扩展名 */
    char *dot = strrchr(base, '.');
    if (dot && (!strcmp(dot, ".mid") || !strcmp(dot, ".mp3") || !strcmp(dot, ".wav")))
        *dot = 0;

    unsigned int seed = 0;
    for (const char *q = st->id; *q; q++) seed = seed * 131u + (unsigned char)*q;
    seed = seed * 131u + (unsigned int)bars;
    /* 每次生成要有变化：以前种子只由风格+小节数决定，同样的参数跑四次就是同一首。
     * 想要可复现就传 seed 参数。 */
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        seed ^= (unsigned int)time(NULL) * 2654435761u;
        seed ^= (unsigned int)(ts.tv_nsec & 0x7fffffff);
    }

    /* 组装 melody / bass / drums 三段 */
    char mel[12000] = {0}, bas[3000] = {0}, drm[3000] = {0};
    size_t mi = 0, bi = 0, di = 0;
    int nn = 0;

    int plen = 0;
    const int *prog = bgm_prog(st, &plen, &seed);

    /* 55% 的概率用一条经典乐句开头（按调式匹配），后面再接自动生成 */
    int use_ph = -1;
    if ((bgm_rand(&seed) % 100) < 55) {
        int want = (st->scale == SC_MINOR) ? 1 : ((st->scale == SC_PENTA) ? 2 : 0);
        int cand[8], nc = 0;
        for (int i = 0; i < (int)(sizeof(PHRASES) / sizeof(PHRASES[0])); i++)
            if (PHRASES[i].mode == want) cand[nc++] = i;
        if (!nc)
            for (int i = 0; i < (int)(sizeof(PHRASES) / sizeof(PHRASES[0])); i++)
                if (PHRASES[i].mode == 0) cand[nc++] = i;
        if (nc) use_ph = cand[bgm_rand(&seed) % (unsigned)nc];
    }
    /* 造一个 2 小节动机（8 个音级）：以主/三/五为骨架，级进为主，偶尔跳进。
     * 后面每 2 小节复用它并做移位变奏 —— 这是"听起来像曲子"的关键。 */
    /* 三个动机（各 4 音）→ AABA 结构。以前只有 1 个 8 音动机整曲反复，
     * 所以"同一个风格每次都像同一首"。 */
    int motif[3][4];
    for (int m = 0; m < 3; m++) {
        int base = (m == 1) ? 2 : 0;
        for (int k = 0; k < 4; k++) {
            unsigned int r = bgm_rand(&seed);
            int step = (int)(r % 3) - 1;
            if (st->mood == 1 && (r & 4)) step = -abs(step);
            if (k == 0) base = (m == 2) ? 4 : 0;
            else base += step;
            if (base < -3) base += 4;
            if (base > 9)  base -= 4;
            motif[m][k] = base;
        }
    }
    /* 节奏池按风格分三组（以前全风格共用一批图案，happy/tense 抽到的一样） */
    static const double RHY_DENSE[6][8] = {   /* 欢快/紧张：密、切分 */
        {1,1,1,1,0,0,0,0}, {0.5,0.5,1,1,1,0,0,0}, {1,0.5,0.5,2,0,0,0,0},
        {0.5,0.5,0.5,0.5,1,1,0,0}, {0.5,1,0.5,1,1,0,0,0}, {1,0.5,0.5,1,0.5,0.5,0,0}
    };
    static const double RHY_SPARSE[5][8] = {  /* 舒缓/悲伤：疏、长音 */
        {2,2,0,0,0,0,0,0}, {3,1,0,0,0,0,0,0}, {1,1,2,0,0,0,0,0},
        {4,0,0,0,0,0,0,0}, {2,1,1,0,0,0,0,0}
    };
    static const double RHY_MID[6][8] = {     /* 神秘/国风/电子：中密度 */
        {1,1,2,0,0,0,0,0}, {1.5,0.5,1,1,0,0,0,0}, {2,1,1,0,0,0,0,0},
        {1,2,1,0,0,0,0,0}, {1,1,1,1,0,0,0,0}, {0.5,0.5,1,2,0,0,0,0}
    };
    int rhyset = 1;
    if (!strcmp(st->id, "happy") || !strcmp(st->id, "tense") || !strcmp(st->id, "game")
        || !strcmp(st->id, "march")) rhyset = 0;
    else if (!strcmp(st->id, "calm") || !strcmp(st->id, "sad")) rhyset = 2;
    /* 段落结构每次随机：AABA / ABAB / AABB */
    int start_off = (int)(bgm_rand(&seed) % 3);   /* 起音落在和弦的 根/三/五 */
    int start_skip = (int)(bgm_rand(&seed) % 4);  /* 开头 0~3 拍留白，形成弱起 */
    int sect = (int)(bgm_rand(&seed) % 3);
    int sect_map[4];
    if (sect == 0) { sect_map[0]=0; sect_map[1]=0; sect_map[2]=1; sect_map[3]=0; }
    else if (sect == 1) { sect_map[0]=0; sect_map[1]=1; sect_map[2]=0; sect_map[3]=1; }
    else { sect_map[0]=0; sect_map[1]=0; sect_map[2]=1; sect_map[3]=1; }

    for (long bar = 0; bar < bars; bar++) {
        /* 低音：跟和弦根音走（不再一直是主音），节奏型随风格 */
        int croot = prog[bar % plen];
        int bp = bgm_deg_pitch(st, croot) - 12;
        if (bp < 28) bp += 12;
        if (st->drums) {
            /* 有鼓的曲子：根音 + 五度交替（2+1+1） */
            bi += (size_t)snprintf(bas + bi, sizeof(bas) - bi, "%s{\"pitch\":%d,\"dur\":2}",
                                   bi ? "," : "", bp);
            bi += (size_t)snprintf(bas + bi, sizeof(bas) - bi, ",{\"pitch\":%d,\"dur\":1}",
                                   bgm_deg_pitch(st, croot + 4) - 12);
            bi += (size_t)snprintf(bas + bi, sizeof(bas) - bi, ",{\"pitch\":%d,\"dur\":1}", bp);
        } else {
            /* 舒缓：整小节长音，第二小节换五度，制造呼吸 */
            int p2 = (bar % 2) ? bgm_deg_pitch(st, croot + 4) - 12 : bp;
            bi += (size_t)snprintf(bas + bi, sizeof(bas) - bi, "%s{\"pitch\":%d,\"dur\":4}",
                                   bi ? "," : "", p2);
        }
        /* 鼓：底鼓(36) 军鼓(38) 踩镲(42)；每 4 小节最后一拍过门 */
        if (st->drums) {
            const char *pat = (bar % 4 == 3)
                ? ",{\"pitch\":36,\"dur\":1},{\"pitch\":38,\"dur\":1},{\"pitch\":38,\"dur\":0.5},{\"pitch\":38,\"dur\":0.5},{\"pitch\":45,\"dur\":0.5},{\"pitch\":43,\"dur\":0.5}"
                : ",{\"pitch\":36,\"dur\":1},{\"pitch\":38,\"dur\":1},{\"pitch\":36,\"dur\":1},{\"pitch\":38,\"dur\":1}";
            if (!di) di += (size_t)snprintf(drm, sizeof(drm), "{\"pitch\":42,\"dur\":0.5}");
            for (int k = 0; k < 8; k++)
                di += (size_t)snprintf(drm + di, sizeof(drm) - di, ",{\"pitch\":42,\"dur\":0.5}");
            di += (size_t)snprintf(drm + di, sizeof(drm) - di, "%s", pat);
        }
        /* 先看这一段有没有经典乐句要铺 */
        int ph_used = 0;
        if (use_ph >= 0) {
            const phrase_t *ph = &PHRASES[use_ph];
            double acc = 0.0;
            for (int i = 0; i < ph->n; i++) {
                double t0 = acc;
                acc += ph->dur[i];
                if (t0 + 0.001 < (double)bar * 4.0) continue;
                if (t0 >= (double)(bar + 1) * 4.0) break;
                int pitch = bgm_deg_pitch(st, ph->deg[i]) + 12;
                if (pitch < 46) pitch += 12;
                if (pitch > 96) pitch -= 12;
                mi += (size_t)snprintf(mel + mi, sizeof(mel) - mi,
                                       "%s{\"pitch\":%d,\"dur\":%.3g}",
                                       mi ? "," : "", pitch, ph->dur[i]);
                nn++;
                ph_used = 1;
            }
            if (acc >= 4.0 * (double)bars) use_ph = -1;   /* 乐句用完，后面转回生成 */
        }
        /* 旋律：按节奏型走；AABA 结构取动机；强拍落和弦音；偶尔休止 */
        if (!ph_used) {
            int p3[3]; bgm_triad(st, croot, p3);
            int rh;
            if (rhyset == 0) rh = (int)(bgm_rand(&seed) % 6);
            else if (rhyset == 2) rh = (int)(bgm_rand(&seed) % 5);
            else rh = (int)(bgm_rand(&seed) % 6);
            /* 开头不再强制固定节奏型：以前每首前两拍结构相同，听起来"总是一样" */
            if (bar == bars - 1) rh = (rhyset == 2) ? 3 : 0;   /* 结尾长音收 */
            int mt = sect_map[bar % 4];
            const double *row = (rhyset == 0) ? RHY_DENSE[rh]
                              : (rhyset == 2) ? RHY_SPARSE[rh] : RHY_MID[rh];
            double pos = 0.0;
            for (int k = 0; k < 8; k++) {
                double d = row[k];
                if (d <= 0) break;
                int strong = (pos < 0.01) || (pos > 1.99 && pos < 2.01);
                unsigned int r = bgm_rand(&seed);
                /* 弱起：整曲最开头留 0~3 拍，让每首的"进点"不同 */
                if (bar == 0 && pos + 0.001 < (double)start_skip) { pos += d; continue; }
                /* 弱拍偶尔留白（休止），比"每拍都塞音"更像真实旋律 */
                if (!strong && (r % 9) == 0) { pos += d; continue; }
                int pitch;
                if (strong) {
                    pitch = p3[(int)((bar * 2 + (int)pos + start_off) % 3)];
                    while (pitch < st->root) pitch += 12;
                } else {
                    int md = motif[mt][k % 4];
                    if (r % 7 == 0) md += ((r & 8) ? 1 : -1);
                    pitch = bgm_deg_pitch(st, md) + 12;
                }
                if (bar == bars - 1 && k == 0) pitch = st->root + 12;
                if (pitch < 46) pitch += 12;
                if (pitch > 96) pitch -= 12;
                mi += (size_t)snprintf(mel + mi, sizeof(mel) - mi,
                                       "%s{\"pitch\":%d,\"dur\":%.3g}",
                                       mi ? "," : "", pitch, d);
                nn++;
                pos += d;
            }
        }
    }

    /* 先出 MIDI */
    char j1[20000];
    snprintf(j1, sizeof(j1),
             "{\"path\":\"%s.mid\",\"tempo\":%.4g,\"program\":%d,\"melody\":[%s],\"bass\":[%s]%s%s%s}",
             base, tempo, style_program(st), mel, bas,
             di ? ",\"drums\":[" : "", di ? drm : "",
             di ? "]}" : "}");
    char *mid_res = op_midi(j1);

    /* 再合成 MP3 */
    char j2[20000];
    snprintf(j2, sizeof(j2),
             "{\"out\":\"%s\",\"tempo\":%.4g,\"melody\":[%s],\"bass\":[%s]%s%s%s}",
             base, tempo, mel, bas,
             di ? ",\"drums\":[" : "", di ? drm : "",
             di ? "]}" : "}");
    char *mp3_res = op_render(j2);
    free(mid_res);

    if (!mp3_res) { free(mp3_res); return jerr("合成 MP3 失败"); }

    char text[2000], ans[300];
    snprintf(text, sizeof(text),
        "**BGM 已生成**（风格模板，确定性生成）\n\n"
        "| 项 | 值 |\n|---|---|\n"
        "| 风格 | `%s`%s |\n"
        "| 速度 | %.0f BPM |\n"
        "| 小节 | %ld（约 %.1f 秒） |\n"
        "| 旋律音符 | %d 个 |\n"
        "| 配器 | 旋律%s%s |\n"
        "| MIDI | `%s.mid` |\n"
        "| MP3 | `%s.mp3` |\n\n"
        "要换风格就改 style：欢快 / 舒缓 / 悲伤 / 紧张 / 神秘 / 电子 / game / march / chinese",
        st->id, picked_at_random ? "（随机挑的）" : "",
        tempo, bars, bars * 4 * 60.0 / tempo, nn,
        bas[0] ? " + 低音" : "", di ? " + 鼓" : "",
        base, base);
    snprintf(ans, sizeof(ans), "BGM 已生成：%s（%s，%.0f BPM，%ld 小节）",
             st->id, picked_at_random ? "随机" : "指定", tempo, bars);
    free(mp3_res);
    return jpair(text, ans);
}

/* ---------------- 入口 ---------------- */
#include "music_sheet.inc"

/* 五线谱图片 → MIDI。谱面识别见 music_sheet.inc。 */
static char *op_sheet(const char *json) {
    char p[512] = {0}, out[512] = {0};
    jget(json, "path", p, sizeof(p));
    jget(json, "out", out, sizeof(out));
    char path[768] = {0};
    if (p[0]) {
        resolve_path(p, path, sizeof(path));
    } else {
        /* 没给 path：用 uploads 里最新的图片 */
        DIR *d = opendir(WORK_DIR "/uploads");
        if (d) {
            struct dirent *e;
            struct stat st2;
            time_t best = 0;
            while ((e = readdir(d)) != NULL) {
                const char *x = strrchr(e->d_name, '.');
                if (!x) continue;
                if (strcasecmp(x, ".jpg") && strcasecmp(x, ".jpeg") && strcasecmp(x, ".png")
                    && strcasecmp(x, ".bmp") && strcasecmp(x, ".webp")) continue;
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
    if (!path[0]) return jerr("没有谱面图片：先上传一张五线谱，或给 path=图片路径");

    double tempo = jget_num(json, "tempo", 96);
    if (tempo < 40 || tempo > 240) tempo = 96;

    note_t *notes = malloc(sizeof(note_t) * MAX_NOTE);
    int *xs = malloc(sizeof(int) * MAX_NOTE);
    int *uis = malloc(sizeof(int) * MAX_NOTE);
    if (!notes || !xs || !uis) return NULL;
    int ns = 0, nh = 0;
    char why[400] = {0};
    mkdir("/tmp/llama_agent/gen", 0777);
    snprintf(g_annot_path, sizeof(g_annot_path),
             "/tmp/llama_agent/gen/omr_%ld.bmp", (long)time(NULL));
    int nn = sheet_to_notes(path, notes, xs, uis, MAX_NOTE, &ns, &nh, why, sizeof(why));
    if (!nn) {
        free(notes); free(xs); free(uis);
        return jerr(why[0] ? why : "没认出谱面");
    }

    /* 按 x 排序（三列一起搬），保证从左到右 */
    for (int i = 1; i < nn; i++) {
        note_t kn = notes[i]; int kx = xs[i], ku = uis[i];
        int j = i - 1;
        while (j >= 0 && xs[j] > kx) { notes[j + 1] = notes[j]; xs[j + 1] = xs[j]; uis[j + 1] = uis[j]; j--; }
        notes[j + 1] = kn; xs[j + 1] = kx; uis[j + 1] = ku;
    }

    /* 时间轴：按识别出的时值累加（1/16 为单位）。四分音符=4u=1 拍 */
    double beat = 60.0 / tempo, t = 0;
    int hist[17] = {0};
    for (int i = 0; i < nn; i++) {
        int u = uis[i];
        if (u < 1) u = 4;
        if (u > 16) u = 16;
        hist[u]++;
        double beats = u / 4.0;
        notes[i].start = t;
        notes[i].dur = beats * beat;
        notes[i].ch = 0;
        t += beats * beat;
    }
    double total_sec = t;

    /* ---------- 节拍处理 ----------
     * 只按识别出来的时值排时间轴有两个毛病：
     *   ① 时值判错（比如八分判成四分）会让整段节奏塌掉；
     *   ② 每个音各自留 5% 空隙 → 听着"断断续续"、没有连贯的拍子。
     * 这里做三件事：onset 对齐到八分网格、时值填满到下一个音（连奏）、
     * 再叠一层每拍一下的节拍声（可关：metronome=0）。 */
    double grid = beat / 2.0;                        /* 八分音符网格 */
    int metro = (int)jget_num(json, "metronome", 1);
    int nn_melody = nn;                     /* 报告用：不含节拍器 */
    for (int i = 0; i < nn; i++) {
        double g = (double)(long)(notes[i].start / grid + 0.5) * grid;
        notes[i].start = g;
    }
    for (int i = 0; i < nn; i++) {
        double end = (i + 1 < nn) ? notes[i + 1].start : notes[i].start + grid;
        if (end <= notes[i].start) end = notes[i].start + grid;
        notes[i].dur = (end - notes[i].start) * 0.98;   /* 连奏：填满，只留 2% 断点 */
        if (notes[i].dur < grid * 0.9) notes[i].dur = grid * 0.9;
    }
    if (nn > 0) total_sec = notes[nn - 1].start + notes[nn - 1].dur;

    if (metro) {
        /* 每拍一个短促高音当节拍器（96 BPM 时 1 拍 = 0.625 秒），
         * 第一拍加重（更响更高），这样"拍子"是听得见的 */
        int nb2 = (int)(total_sec / beat) + 1;
        int cap2 = nn + nb2;
        int k = nn;
        for (int b2 = 0; b2 < nb2 && k < MAX_NOTE; b2++) {
            notes[k].pitch = (b2 % 4 == 0) ? 88 : 81;
            notes[k].start = b2 * beat;
            notes[k].dur = 0.045;
            notes[k].ch = 9;                       /* 单独一轨，别混进旋律 */
            k++;
        }
        nn = k;
    }

    /* 时值分布写进日志，方便核对（1u=十六分 2=八分 4=四分 8=二分 16=全） */
    fprintf(stderr, "  [omr] 时值分布 16分=%d 8分=%d 4分=%d 2分=%d 全=%d 总长=%.1fs\n",
            hist[1], hist[2], hist[4], hist[8], hist[16], total_sec);

    /* 音域兜底：整段挪到 C4–C6 之间，免得全是加线外的怪音 */
    int lo = 127, hi = 0;
    for (int i = 0; i < nn; i++) {
        if (notes[i].pitch < lo) lo = notes[i].pitch;
        if (notes[i].pitch > hi) hi = notes[i].pitch;
    }
    /* 单谱表还跨两个八度以上 → 谱号/加线判断不可靠，拒绝。
     * 多谱表（钢琴大谱表、钢琴+小提琴）本来就跨三四个八度，不能一刀切。 */
    if (ns == 1 && hi - lo > 24) {
        free(notes);
        return jerr("认出的音域过宽（超过两个八度），多半不是单声部谱面，先不生成，避免出一段乱旋律");
    }

    /* 1) 写 .mid —— 复用 op_midi 的写入器（把音符序列打包成它的入参） */
    char midi_path[768] = {0};
    if (out[0]) {
        resolve_path(out, midi_path, sizeof(midi_path));
    } else {
        mkdir("/tmp/llama_agent/gen", 0777);
        snprintf(midi_path, sizeof(midi_path), WORK_DIR "/gen/sheet_%ld.mid", (long)time(NULL));
    }
    if (!strstr(midi_path, ".mid")) strncat(midi_path, ".mid", sizeof(midi_path) - strlen(midi_path) - 1);

    size_t cap = 4096 + (size_t)nn * 48;
    char *mj = malloc(cap);
    char *mr = NULL;
    if (mj) {
        size_t o = (size_t)snprintf(mj, cap,
            "{\"op\":\"midi\",\"tempo\":%d,\"path\":\"%s\",\"notes\":[", (int)tempo, midi_path);
        for (int i = 0; i < nn && o + 60 < cap; i++) {
            double beats = notes[i].dur / beat;
            if (beats < 0.25) beats = 0.25;
            o += (size_t)snprintf(mj + o, cap - o, "%s{\"pitch\":%d,\"dur\":%.2f}",
                                  i ? "," : "", notes[i].pitch, beats);
        }
        snprintf(mj + o, cap - o, "]}");
        mr = op_midi(mj);
        free(mj);
    }

    /* 2) 顺手合成一段 MP3 试听（不影响 .mid 交付） */
    char wavbase[900];
    snprintf(wavbase, sizeof(wavbase), "%s", midi_path);
    char *dotm = strrchr(wavbase, '.');
    if (dotm) *dotm = 0;
    char *rr = do_render(notes, nn, wavbase, NULL, tempo);
    free(rr);

    char mp3[900] = {0};
    snprintf(mp3, sizeof(mp3), "%s.mp3", wavbase);
    free(notes); free(xs); free(uis);
    free(mr);

    char *res = malloc(1400);
    if (!res) return NULL;
    snprintf(res, 1400,
        "{\"ok\":1,\"path\":\"%s\",\"file\":\"%s\",\"mp3\":\"%s\",\"image\":\"%s\","
        "\"notes\":%d,\"staves\":%d,"
        "\"engine\":\"内置 OMR（PP 谱线投影 + 符头厚度）\","
        "\"answer\":\"从五线谱认出 %d 个音符（%d 组谱表），已生成 MIDI：%s\"}",
        midi_path, mp3, mp3, g_annot_path, nn_melody, ns, nn_melody, ns, midi_path);
    return res;
}

static char *run(const char *json_in) {
    char op[32] = {0};
    jget(json_in, "op", op, sizeof(op));
    if (!op[0]) {
        if (strstr(json_in, "\"notes\"") || strstr(json_in, "\"melody\""))
            snprintf(op, sizeof(op), "midi");
        else if (strstr(json_in, "\"path\"") && strstr(json_in, ".mid"))
            snprintf(op, sizeof(op), "import");
        else if (strstr(json_in, "五线谱") || strstr(json_in, "乐谱") || strstr(json_in, "曲谱")
                 || strstr(json_in, ".jpg") || strstr(json_in, ".png"))
            snprintf(op, sizeof(op), "omr");
        else
            snprintf(op, sizeof(op), "render");
    }

    if (!strcmp(op, "midi") || !strcmp(op, "make"))   return op_midi(json_in);
    if (!strcmp(op, "render") || !strcmp(op, "play")) return op_render(json_in);
    if (!strcmp(op, "import") || !strcmp(op, "wav"))  return op_import(json_in);
    if (!strcmp(op, "bgm") || !strcmp(op, "music"))   return op_bgm(json_in);
    /* 注意：op 名字不能叫 sheet —— video 插件的 sheet 是"缩略图阵列"，
     * op_to_plugin 会先把它抢走。用 omr / sheetmusic。 */
    if (!strcmp(op, "omr") || !strcmp(op, "sheetmusic") || !strcmp(op, "五线谱"))
        return op_sheet(json_in);

    return jerr("不支持的 op：midi / render / import / bgm / omr（五线谱图片→MIDI）");
}

static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "music",
    "音乐（纯 C）：**要背景音乐/BGM 一律用 op=bgm 加 style**，插件按风格模板确定性生成"
    "（比你逐个音符编快得多，也不会跑调）；style 可选 欢快/舒缓/悲伤/紧张/神秘/电子/game/march/chinese，"
    "不给就随机。其它：omr=**五线谱图片→MIDI**（高音谱号单声部印刷体），midi=手写音符出 MIDI，render=合成 MP3，import=读 MIDI 转 MP3",
    "{\"op\":\"midi\",\"path\":\"/tmp/llama_agent/a.mid\",\"tempo\":160,"
    "\"notes\":[{\"pitch\":72,\"dur\":1},{\"pitch\":74,\"dur\":1}]}"
    "  或分轨 {\"op\":\"midi\",\"melody\":[...],\"bass\":[...],\"drums\":[...]}"
    "  或 {\"op\":\"render\",\"path\":\"/tmp/llama_agent/a.mid\"}"
    "  或 {\"op\":\"import\",\"path\":\"/tmp/llama_agent/x.mid\"}",
    run,
    "midi,music,render,import,音乐,作曲,合成"
};

plugin_desc_t *plugin_entry(void) { return &desc; }
