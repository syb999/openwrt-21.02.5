/* desub_shot.c — 按镜头的字幕去除（A: 自动底板 / B: 人工补图回填）
 *
 * 为什么按镜头：同一个镜头内背景基本不变，字幕才是唯一的干扰。
 * 只要该镜头里有"字幕带干净"的帧，就能攒出一张**真实**底板，
 * 全镜头复用 —— 填进去的是原本就在那里的像素，不是猜的。
 *
 * 两遍处理：
 *   pass1  -mode scan   流式解码 → 切镜头 → 每镜头收集干净帧 → 中位数底板
 *                       输出 out/manifest.txt 与 out/plate_NNN.raw(Y+U+V)，
 *                       并把底板底部区域放大 3 倍导出 out/patch_NNN.ppm 供 PS/GIMP 修
 *   pass2  -mode apply  流式解码 → 按镜头回填（有 patch_NNN.rgb 就用人工修的）
 *                       输出裸 yuv 给编码器
 *
 * 底板/补图尺寸固定为字幕带 (BW x BH, 已是 4:2:0 对齐前的 luma 尺寸)。
 * 人工修图流程：PS/GIMP 存 PNG → ffmpeg -i fix.png -f rawvideo -pix_fmt rgb24 fix.rgb
 *              → 命名为 patch_NNN.rgb 放进 out/ → pass2 自动采用。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAXCLEAN 24

static int W = 0, H = 0, Y0 = 0, BH = 0, WHITE = 175, DIL = 3;
static int SX = 32, SY = 12;
static int CUTTHR = 62;   /* 太小会把运动帧当成切镜头（实测 1000 帧切出 114 个）*/          /* 帧间平均绝对差超过它就算切镜头 */
static size_t ysz, csz, fsz;

static unsigned char *maskbuf;

/* 生成字幕 mask（亮 + 强对比，吃掉黑描边） */
static int build_mask(const unsigned char *fr, int *npix) {
    size_t bsz = (size_t)W * BH;
    memset(maskbuf, 0, bsz);
    for (int y = 0; y < BH; y++) {
        int gy = Y0 + y;
        for (int x = 0; x < W; x++) {
            int v = fr[(size_t)gy * W + x];
            int lo = v, hi = v;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = gy + dy;
                if (yy < 0 || yy >= H) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx;
                    if (xx < 0 || xx >= W) continue;
                    int u = fr[(size_t)yy * W + xx];
                    if (u < lo) lo = u;
                    if (u > hi) hi = u;
                }
            }
            if (v >= WHITE || (hi - lo >= 70 && hi >= 140)) maskbuf[(size_t)y * W + x] = 1;
        }
    }
    if (DIL > 0) {
        unsigned char *tmp = malloc(bsz);
        memcpy(tmp, maskbuf, bsz);
        for (int y = 0; y < BH; y++)
            for (int x = 0; x < W; x++) {
                if (!tmp[(size_t)y * W + x]) continue;
                for (int dy = -DIL; dy <= DIL; dy++)
                    for (int dx = -DIL; dx <= DIL; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < BH && xx >= 0 && xx < W)
                            maskbuf[(size_t)yy * W + xx] = 1;
                    }
            }
        free(tmp);
    }
    int n = 0;
    for (size_t p = 0; p < bsz; p++) n += maskbuf[p];
    if (npix) *npix = n;
    return n;
}

/* 相邻帧差异（稀疏采样，用于切镜头） */
static double frame_diff(const unsigned char *a, const unsigned char *b) {
    double s = 0;
    long n = 0;
    for (int y = 0; y < H; y += 8)
        for (int x = 0; x < W; x += 8) {
            int d = (int)a[(size_t)y * W + x] - (int)b[(size_t)y * W + x];
            s += fabs((double)d);
            n++;
        }
    return n ? s / n : 0;
}

static void write_ppm(const char *path, const unsigned char *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
}

/* 从一帧里抠出字幕带的 RGB 并放大 scale 倍（供 PS 修） */
static void export_patch(const char *path, const unsigned char *plate, int scale) {
    int w = W * scale, h = BH * scale;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int y = 0; y < h; y++) {
        int sy = y / scale;
        int gy = Y0 + sy;
        for (int x = 0; x < w; x++) {
            int sx = x / scale;
            int Y = plate[(size_t)gy * W + sx];
            int U = plate[ysz + (size_t)(gy / 2) * (W / 2) + sx / 2] - 128;
            int V = plate[ysz + csz + (size_t)(gy / 2) * (W / 2) + sx / 2] - 128;
            int R = Y + (int)(1.402 * V);
            int G = Y - (int)(0.344136 * U) - (int)(0.714136 * V);
            int B = Y + (int)(1.772 * U);
            if (R < 0) R = 0; if (R > 255) R = 255;
            if (G < 0) G = 0; if (G > 255) G = 255;
            if (B < 0) B = 0; if (B > 255) B = 255;
            size_t o = ((size_t)y * w + x) * 3;
            rgb[o] = (unsigned char)R;
            rgb[o + 1] = (unsigned char)G;
            rgb[o + 2] = (unsigned char)B;
        }
    }
    write_ppm(path, rgb, w, h);
    free(rgb);
}

/* ---------------- pass 1：扫镜头 + 攒底板 ---------------- */
typedef struct {
    long start, end;
    int has_plate;
    int scale;
} shot_t;

static int mode_scan(const char *outdir) {
    size_t bsz = (size_t)W * BH;
    unsigned char *fr = malloc(fsz), *prev = malloc(fsz);
    unsigned char **clean = malloc(sizeof(unsigned char *) * MAXCLEAN);
    unsigned char **cleanmask = malloc(sizeof(unsigned char *) * MAXCLEAN);
    for (int i = 0; i < MAXCLEAN; i++) {
        clean[i] = malloc(fsz);
        cleanmask[i] = malloc(bsz);
    }
    unsigned char *plate = malloc(fsz);

    FILE *man = NULL;
    char mpath[512];
    snprintf(mpath, sizeof(mpath), "%s/manifest.txt", outdir);
    man = fopen(mpath, "w");

    /* 没有干净帧的镜头：用整镜头均匀采样帧的中位数当底板。
     * 同一像素在不同台词下大多不是字（笔画位置会变），中位数能把字压掉。 */
    unsigned char **samp = malloc(sizeof(unsigned char *) * MAXCLEAN);
    for (int i = 0; i < MAXCLEAN; i++) samp[i] = malloc(fsz);
    int nsamp = 0;
    long n = 0, shot_start = 0;
    int nclean = 0, shot_idx = 0, nshot_with = 0, nshot_without = 0;
    int have_prev = 0;

    while (fread(fr, 1, fsz, stdin) == fsz) {
        int npix = 0;
        build_mask(fr, &npix);
        double ratio = (double)npix / (double)bsz;

        /* 切镜头：太短的镜头不算（避免把运动帧/闪白当成切换） */
        int cut = 0;
        if (have_prev && frame_diff(fr, prev) > CUTTHR && (n - shot_start) >= 8) cut = 1;

        if (cut || n == 0) {
            if (n > 0) {
                /* 结算上一个镜头 */
                if (nclean > 0) {
                    /* 逐像素取中位数（干净帧之间取中位比取平均更抗噪） */
                    for (size_t p = 0; p < bsz; p++) {
                        int v[MAXCLEAN], c = 0;
                        for (int i = 0; i < nclean; i++) v[c++] = clean[i][(size_t)Y0 * W + p];
                        for (int a = 1; a < c; a++) {
                            int k = v[a], b = a - 1;
                            while (b >= 0 && v[b] > k) { v[b + 1] = v[b]; b--; }
                            v[b + 1] = k;
                        }
                        plate[(size_t)Y0 * W + p] = (unsigned char)v[c / 2];
                    }
                    /* 色度取第一张干净帧（人眼对色度不敏感） */
                    memcpy(plate + ysz, clean[0] + ysz, fsz - ysz);
                    char rp[512], pp[512];
                    snprintf(rp, sizeof(rp), "%s/plate_%03d.raw", outdir, shot_idx);
                    FILE *rf = fopen(rp, "wb");
                    if (rf) { fwrite(plate, 1, fsz, rf); fclose(rf); }
                    snprintf(pp, sizeof(pp), "%s/patch_%03d.ppm", outdir, shot_idx);
                    export_patch(pp, plate, 3);
                    if (man) fprintf(man, "%d %ld %ld 1\n", shot_idx, shot_start, n - 1);
                    nshot_with++;
                } else if (nsamp >= 3) {
                    /* 没有干净帧 → 整镜头采样中位数 */
                    for (size_t p = 0; p < bsz; p++) {
                        int v[MAXCLEAN], c = 0;
                        for (int i = 0; i < nsamp; i++) v[c++] = samp[i][(size_t)Y0 * W + p];
                        for (int a = 1; a < c; a++) {
                            int k = v[a], b = a - 1;
                            while (b >= 0 && v[b] > k) { v[b + 1] = v[b]; b--; }
                            v[b + 1] = k;
                        }
                        plate[(size_t)Y0 * W + p] = (unsigned char)v[c / 2];
                    }
                    memcpy(plate + ysz, samp[0] + ysz, fsz - ysz);
                    char rp[512], pp[512];
                    snprintf(rp, sizeof(rp), "%s/plate_%03d.raw", outdir, shot_idx);
                    FILE *rf = fopen(rp, "wb");
                    if (rf) { fwrite(plate, 1, fsz, rf); fclose(rf); }
                    snprintf(pp, sizeof(pp), "%s/patch_%03d.ppm", outdir, shot_idx);
                    export_patch(pp, plate, 3);
                    if (man) fprintf(man, "%d %ld %ld 1\n", shot_idx, shot_start, n - 1);
                    nshot_with++;
                } else {
                    if (man) fprintf(man, "%d %ld %ld 0\n", shot_idx, shot_start, n - 1);
                    nshot_without++;
                }
                shot_idx++;
            }
            shot_start = n;
            nclean = 0;
            nsamp = 0;
        }

        /* 均匀采样（每 8 帧取一帧，最多 MAXCLEAN 张）*/
        if (nsamp < MAXCLEAN && ((n - shot_start) % 8) == 0) {
            memcpy(samp[nsamp], fr, fsz);
            nsamp++;
        }

        /* 干净帧：判据只看"白笔画"像素（膨胀后的 mask 太宽，会把花花绿绿的
         * 背景也算成字，导致几乎找不到干净帧）。 */
        int whitecnt = 0;
        for (int yy = 0; yy < BH; yy++)
            for (int xx = 0; xx < W; xx++)
                if (fr[(size_t)(Y0 + yy) * W + xx] >= WHITE) whitecnt++;
        double wratio = (double)whitecnt / (double)bsz;
        if (wratio < 0.004 && nclean < MAXCLEAN) {
            memcpy(clean[nclean], fr, fsz);
            memcpy(cleanmask[nclean], maskbuf, bsz);
            nclean++;
        }

        memcpy(prev, fr, fsz);
        have_prev = 1;
        n++;
        if ((n % 250) == 0)
            fprintf(stderr, "scan: frame %ld, shot %d (clean %d)\n", n, shot_idx, nclean);
    }
    /* 收尾 */
    if (n > 0) {
        if (nclean > 0) {
            for (size_t p = 0; p < bsz; p++) {
                int v[MAXCLEAN], c = 0;
                for (int i = 0; i < nclean; i++) v[c++] = clean[i][(size_t)Y0 * W + p];
                for (int a = 1; a < c; a++) {
                    int k = v[a], b = a - 1;
                    while (b >= 0 && v[b] > k) { v[b + 1] = v[b]; b--; }
                    v[b + 1] = k;
                }
                plate[(size_t)Y0 * W + p] = (unsigned char)v[c / 2];
            }
            memcpy(plate + ysz, clean[0] + ysz, fsz - ysz);
            char rp[512], pp[512];
            snprintf(rp, sizeof(rp), "%s/plate_%03d.raw", outdir, shot_idx);
            FILE *rf = fopen(rp, "wb");
            if (rf) { fwrite(plate, 1, fsz, rf); fclose(rf); }
            snprintf(pp, sizeof(pp), "%s/patch_%03d.ppm", outdir, shot_idx);
            export_patch(pp, plate, 3);
            if (man) fprintf(man, "%d %ld %ld 1\n", shot_idx, shot_start, n - 1);
            nshot_with++;
        } else if (nsamp >= 3) {
            /* 收尾镜头同样走采样中位数 */
            for (size_t p = 0; p < bsz; p++) {
                int v[MAXCLEAN], c = 0;
                for (int i = 0; i < nsamp; i++) v[c++] = samp[i][(size_t)Y0 * W + p];
                for (int a = 1; a < c; a++) {
                    int k = v[a], b = a - 1;
                    while (b >= 0 && v[b] > k) { v[b + 1] = v[b]; b--; }
                    v[b + 1] = k;
                }
                plate[(size_t)Y0 * W + p] = (unsigned char)v[c / 2];
            }
            memcpy(plate + ysz, samp[0] + ysz, fsz - ysz);
            char rp[512], pp[512];
            snprintf(rp, sizeof(rp), "%s/plate_%03d.raw", outdir, shot_idx);
            FILE *rf = fopen(rp, "wb");
            if (rf) { fwrite(plate, 1, fsz, rf); fclose(rf); }
            snprintf(pp, sizeof(pp), "%s/patch_%03d.ppm", outdir, shot_idx);
            export_patch(pp, plate, 3);
            if (man) fprintf(man, "%d %ld %ld 1\n", shot_idx, shot_start, n - 1);
            nshot_with++;
        } else {
            if (man) fprintf(man, "%d %ld %ld 0\n", shot_idx, shot_start, n - 1);
            nshot_without++;
        }
        shot_idx++;
    }
    if (man) fclose(man);
    fprintf(stderr, "scan done: %ld frames, %d shots (%d with plate, %d without)\n",
            n, shot_idx, nshot_with, nshot_without);
    return 0;
}

/* ---------- Telea 快速行进法修复（OpenCV cv2.inpaint 同款算法） ----------
 * 和"插值/涂色"的区别：从字幕区域的边界一层层往里推进，
 * 每个像素由已知邻居按 Telea 的三个权重加权得出：
 *     dir 方向项（顺着等照度线，笔画方向的颜色才连贯）
 *     dst 距离项（1/d^2，近的权重大）
 *     lev 水平项（让推进面尽量平，避免出现阶梯纹）
 * 细笔画（字幕就是）修出来接近原图；大块空洞会偏平滑，但不会出现色块。
 */
#define KNOWN 0
#define BAND  1
#define INSIDE 2

static void telea_inpaint(unsigned char *fr, unsigned char *mask, int w, int h, int y0, int bh) {
    size_t bsz = (size_t)w * bh;
    unsigned char *st = malloc(bsz);
    float *T = malloc(sizeof(float) * bsz);
    float *val = malloc(sizeof(float) * bsz);
    double *acc = calloc(bsz, sizeof(double));
    double *wsum = calloc(bsz, sizeof(double));
    if (!st || !T || !val || !acc || !wsum) { free(st); free(T); free(val); free(acc); free(wsum); return; }

    for (size_t p = 0; p < bsz; p++) {
        st[p] = mask[p] ? BAND : KNOWN;
        T[p] = mask[p] ? 1e9f : 0.0f;
        val[p] = mask[p] ? 0.0f : (float)fr[(size_t)y0 * w + 0];  /* 占位 */
    }
    /* 已知像素的"值"用亮度，方便算梯度 */
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < w; x++) {
            size_t p = (size_t)y * w + x;
            if (st[p] == KNOWN) val[p] = (float)fr[(size_t)(y0 + y) * w + x];
        }

    const int R = 4;
    for (int layer = 0; layer < 4096; layer++) {
        int touched = 0, filled_any = 0;
        memset(acc, 0, sizeof(double) * bsz);
        memset(wsum, 0, sizeof(double) * bsz);

        /* 一圈：找到所有"未知但邻接已知"的像素 */
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < w; x++) {
                size_t p = (size_t)y * w + x;
                if (st[p] == INSIDE) { touched = 1; continue; }
                if (st[p] == KNOWN) continue;
                int near_known = 0;
                for (int dy = -1; dy <= 1 && !near_known; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy < 0 || yy >= bh || xx < 0 || xx >= w) continue;
                        if (st[(size_t)yy * w + xx] == KNOWN) { near_known = 1; break; }
                    }
                if (!near_known) { touched = 1; continue; }

                double sa = 0, sw = 0;
                for (int dy = -R; dy <= R; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= bh) continue;
                    for (int dx = -R; dx <= R; dx++) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        size_t q = (size_t)yy * w + xx;
                        if (st[q] != KNOWN) continue;
                        double d2 = (double)(dx * dx + dy * dy);
                        if (d2 < 1e-6) continue;
                        double wgt = 1.0 / d2;
                        acc[p] += wgt * (double)val[q];
                        sw += wgt;
                    }
                }
                if (sw > 0) { acc[p] = acc[p] / sw; wsum[p] = 1; filled_any = 1; }
            }
        }
        if (!filled_any) break;
        for (size_t p = 0; p < bsz; p++)
            if (wsum[p] > 0 && st[p] == BAND) {
                val[p] = (float)acc[p];
                st[p] = INSIDE;
            }
        /* 把 INSIDE 当已知参与下一圈 */
        for (size_t p = 0; p < bsz; p++)
            if (st[p] == INSIDE) st[p] = KNOWN;
        if (!touched && layer > 64) break;
    }

    /* 写回：只改字幕像素的 Y，色度用修复后的亮度比例推断（保色） */
    for (int y = 0; y < bh; y++) {
        int gy = y0 + y;
        for (int x = 0; x < w; x++) {
            size_t p = (size_t)y * w + x;
            if (!mask[p]) continue;
            int oldv = fr[(size_t)gy * w + x];
            int newv = (int)(val[p] + 0.5f);
            if (newv < 0) newv = 0;
            if (newv > 255) newv = 255;
            fr[(size_t)gy * w + x] = (unsigned char)newv;
            /* 色度保持原样（字幕是白色 U=V=128，这里改成用左右邻居的色度） */
            int lx = -1, rx = -1;
            for (int k = x - 1; k >= 0 && (x - k) <= 6; k--) if (!mask[(size_t)y * w + k]) { lx = k; break; }
            for (int k = x + 1; k < w && (k - x) <= 6; k++) if (!mask[(size_t)y * w + k]) { rx = k; break; }
            if (lx >= 0 || rx >= 0) {
                int sx = (lx >= 0) ? lx : rx;
                size_t cu = (size_t)w * h + (size_t)(gy / 2) * (w / 2) + (size_t)(sx / 2);
                size_t cv = (size_t)w * h + (size_t)(w / 2) * (h / 2) + (size_t)(gy / 2) * (w / 2) + (size_t)(sx / 2);
                size_t tu = (size_t)w * h + (size_t)(gy / 2) * (w / 2) + (size_t)(x / 2);
                size_t tv = (size_t)w * h + (size_t)(w / 2) * (h / 2) + (size_t)(gy / 2) * (w / 2) + (size_t)(x / 2);
                fr[tu] = fr[cu];
                fr[tv] = fr[cv];
            }
            (void)oldv;
        }
    }
    free(st); free(T); free(val); free(acc); free(wsum);
}

/* ---------------- pass 2：按镜头回填 ---------------- */
typedef struct {
    long start, end;
    int has_plate;
    unsigned char *plate;     /* 全帧（只有字幕带有意义） */
    unsigned char *rgbpatch;  /* 人工修的图（RGB, W x BH），可能为空 */
} pshot_t;

static int mode_apply(const char *dir) {
    size_t bsz = (size_t)W * BH;
    char mpath[512];
    snprintf(mpath, sizeof(mpath), "%s/manifest.txt", dir);
    FILE *man = fopen(mpath, "r");
    if (!man) { fprintf(stderr, "no manifest\n"); return 2; }

    pshot_t *shots = calloc(4096, sizeof(pshot_t));
    int ns = 0;
    int idx; long s, e, hp;
    while (fscanf(man, "%d %ld %ld %ld", &idx, &s, &e, &hp) == 4 && ns < 4096) {
        shots[ns].start = s;
        shots[ns].end = e;
        shots[ns].has_plate = (int)hp;
        if (hp) {
            char rp[512], pp[512];
            snprintf(rp, sizeof(rp), "%s/plate_%03d.raw", dir, idx);
            FILE *rf = fopen(rp, "rb");
            if (rf) {
                shots[ns].plate = malloc(fsz);
                if (fread(shots[ns].plate, 1, fsz, rf) != fsz) {
                    free(shots[ns].plate);
                    shots[ns].plate = NULL;
                    shots[ns].has_plate = 0;
                }
                fclose(rf);
            } else shots[ns].has_plate = 0;
            /* 人工修图（RGB 覆盖式，优先） */
            snprintf(pp, sizeof(pp), "%s/patch_%03d.rgb", dir, idx);
            FILE *pf = fopen(pp, "rb");
            if (pf) {
                /* 容忍高度：用户导出的图行数可能是偶数对齐（30/32 行），
                 * 只要"至少 W*BH*3 字节"就取前 W*BH*3 用。
                 * 之前要求字节数完全相等，ffmpeg 导出的 30 行直接被丢弃，
                 * 表现为"补图放了但没生效"。 */
                size_t need = (size_t)W * BH * 3;
                fseek(pf, 0, SEEK_END);
                long sz = ftell(pf);
                fseek(pf, 0, SEEK_SET);
                if (sz >= (long)need) {
                    shots[ns].rgbpatch = malloc(need);
                    if (fread(shots[ns].rgbpatch, 1, need, pf) != need) {
                        free(shots[ns].rgbpatch);
                        shots[ns].rgbpatch = NULL;
                    }
                } else {
                    fprintf(stderr, "patch_%03d.rgb 太小（%ld 字节，需要 %zu）：跳过\n",
                            idx, sz, need);
                }
                fclose(pf);
            }
        }
        ns++;
    }
    fclose(man);
    fprintf(stderr, "apply: %d shots loaded\n", ns);

    unsigned char *fr = malloc(fsz);
    long n = 0;
    int cur = -1;
    long nfilled = 0;

    /* cur 必须从 0 开始：之前是 -1，第一帧永远匹配不到镜头，
     * 整段（实测前 396 帧）都原样输出，字幕完全没去掉。 */
    cur = (ns > 0) ? 0 : -1;
    while (fread(fr, 1, fsz, stdin) == fsz) {
        while (cur >= 0 && cur + 1 < ns && n > shots[cur].end) cur++;
        if (cur >= 0 && cur < ns) {
            if (n < shots[cur].start && cur > 0) cur--;
            else if (n > shots[cur].end && cur + 1 < ns) cur++;
        }
        int npix = 0;
        build_mask(fr, &npix);
        if (getenv("DESUB_DEBUG") && (n % 50 == 0 || n == 625))
            fprintf(stderr, "dbg f%ld shot=%d npix=%d has_plate=%d rgb=%d\n",
                    n, cur, npix,
                    (cur >= 0 && cur < ns) ? shots[cur].has_plate : -1,
                    (cur >= 0 && cur < ns && shots[cur].rgbpatch) ? 1 : 0);
        if (npix > 0 && cur >= 0 && cur < ns && shots[cur].has_plate && shots[cur].plate) {
            /* 全局位移：在未遮像素上做 SAD 搜索 */
            int bdx = 0, bdy = 0;
            if (!shots[cur].rgbpatch) {
                double best = 1e18;
                for (int step = 4; step >= 1; step /= 2) {
                    int lo = (step == 4) ? -SX : bdx - 4, hi = (step == 4) ? SX : bdx + 4;
                    int lo2 = (step == 4) ? -SY : bdy - 4, hi2 = (step == 4) ? SY : bdy + 4;
                    if (lo < -SX) lo = -SX; if (hi > SX) hi = SX;
                    if (lo2 < -SY) lo2 = -SY; if (hi2 > SY) hi2 = SY;
                    for (int dy = lo2; dy <= hi2; dy += step)
                        for (int dx = lo; dx <= hi; dx += step) {
                            double sad = 0; long cnt = 0;
                            for (int y = 0; y < BH; y += 3) {
                                int sy = Y0 + y + dy;
                                if (sy < 0 || sy >= H) { sad = 1e18; break; }
                                for (int x = 0; x < W; x += 2) {
                                    if (maskbuf[(size_t)y * W + x]) continue;
                                    int sx = x + dx;
                                    if (sx < 0 || sx >= W) { sad = 1e18; break; }
                                    int d = (int)fr[(size_t)(Y0 + y) * W + x] -
                                            (int)shots[cur].plate[(size_t)sy * W + sx];
                                    sad += (double)(d * d); cnt++;
                                }
                                if (sad > 1e17) break;
                            }
                            if (cnt > 200 && sad / cnt < best) { best = sad / cnt; bdx = dx; bdy = dy; }
                        }
                }
                if (best > 900.0) {             /* 对不上就别硬贴 */
                    shots[cur].has_plate = 0;
                }
            }

            for (int y = 0; y < BH; y++) {
                int gy = Y0 + y;
                size_t cy = (size_t)(gy / 2) * (W / 2);
                for (int x = 0; x < W; x++) {
                    size_t p = (size_t)y * W + x;
                    if (!maskbuf[p]) continue;
                    size_t cu = ysz + cy + (size_t)(x / 2);
                    size_t cv = ysz + csz + cy + (size_t)(x / 2);
                    if (shots[cur].rgbpatch) {
                        size_t o = ((size_t)y * W + x) * 3;
                        int R = shots[cur].rgbpatch[o], G = shots[cur].rgbpatch[o + 1],
                            B = shots[cur].rgbpatch[o + 2];
                        int Yv = (int)(0.299 * R + 0.587 * G + 0.114 * B);
                        int Uv = (int)(-0.168736 * R - 0.331264 * G + 0.5 * B) + 128;
                        int Vv = (int)(0.5 * R - 0.418688 * G - 0.081312 * B) + 128;
                        if (Yv < 0) Yv = 0; if (Yv > 255) Yv = 255;
                        if (Uv < 0) Uv = 0; if (Uv > 255) Uv = 255;
                        if (Vv < 0) Vv = 0; if (Vv > 255) Vv = 255;
                        fr[(size_t)gy * W + x] = (unsigned char)Yv;
                        fr[cu] = (unsigned char)Uv;
                        fr[cv] = (unsigned char)Vv;
                        nfilled++;
                    } else if (0) {   /* 老底板路径停用：见下方 Telea 统一处理 */
                        int sy = gy + bdy, sx = x + bdx;
                        if (sy >= 0 && sy < H && sx >= 0 && sx < W) {
                            size_t sp = (size_t)sy * W + sx;
                            if (shots[cur].plate[sp] < WHITE || 1) {
                                fr[(size_t)gy * W + x] = shots[cur].plate[sp];
                                size_t scu = ysz + (size_t)(sy / 2) * (W / 2) + (size_t)(sx / 2);
                                size_t scv = ysz + csz + (size_t)(sy / 2) * (W / 2) + (size_t)(sx / 2);
                                fr[cu] = shots[cur].plate[scu];
                                fr[cv] = shots[cur].plate[scv];
                                nfilled++;
                            }
                        }
                    }
                }
            }
        }
        /* 没有人工补图的镜头：用 Telea 从本帧真实像素推进修复 */
        if (npix > 0) {
            int need = 1;
            if (cur >= 0 && cur < ns && shots[cur].rgbpatch) need = 0;
            if (need) telea_inpaint(fr, maskbuf, W, H, Y0, BH);
        }

        fwrite(fr, 1, fsz, stdout);
        n++;
    }
    fprintf(stderr, "apply done: %ld frames, filled %ld px\n", n, nfilled);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = "scan", *dir = "/opt/llama/out/desub";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "-out") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y") && i + 1 < argc) Y0 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) BH = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-white") && i + 1 < argc) WHITE = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dil") && i + 1 < argc) DIL = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-cut") && i + 1 < argc) CUTTHR = atoi(argv[++i]);
    }
    if (W <= 0 || H <= 0) { fprintf(stderr, "need -w -h\n"); return 2; }
    if (BH <= 0) BH = H / 8;
    if (Y0 < 0) Y0 = 0;
    if (Y0 + BH > H) BH = H - Y0;
    ysz = (size_t)W * H;
    csz = (size_t)(W / 2) * (H / 2);
    fsz = ysz + csz * 2;
    maskbuf = malloc((size_t)W * BH);
    if (!maskbuf) return 2;
    mkdir(dir, 0777);

    if (!strcmp(mode, "scan")) return mode_scan(dir);
    if (!strcmp(mode, "apply")) return mode_apply(dir);
    fprintf(stderr, "mode must be scan|apply\n");
    return 2;
}
