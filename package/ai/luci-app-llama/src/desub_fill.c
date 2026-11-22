/* desub_fill.c — 硬字幕去除：运动补偿 + 干净帧重建
 *
 * 前面几版为什么失败（都是实测看出来的）：
 *   · 邻近插值 → 涂开的色块，纹理没了
 *   · 时间中位数 → 字幕停留比窗口久，参考帧本身带字
 *   · PatchMatch 块合成 → 字幕带是个又宽又连续的空洞，可选纹理太少，
 *     投票退化成一片平均色（实测是一块粉色）
 *
 * 这一版的思路，针对动画片（背景静止或整体平移）：
 *   1) 记住最近一张"字幕带里几乎没有字"的干净帧（plate）
 *   2) 当前帧 vs plate 做**全局位移搜索**（在没被字幕盖住的像素上算 SAD，
 *      搜索 ±24 / ±12），得到平移量
 *   3) 把 plate 按这个位移搬过来，只填字幕像素
 *   → 填进去的就是"这个位置本来该有的画面"，不是猜出来的颜色
 *
 *   位移搜索失败（场景切换 / 复杂运动）时，退到"最近一个干净像素"，
 *   再不行才用一维插值兜底。
 *
 * 用法：-w -h -y -b [-white 175] [-dil 4] [-k 12] [-sx 24] [-sy 12] [-stats]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define MAXK 24

static int W = 0, H = 0, Y0 = 0, BH = 0;
static int WHITE = 175, DIL = 4, SX = 24, SY = 12;

int main(int argc, char **argv) {
    int K = 12, SHOW = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-y") && i + 1 < argc) Y0 = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) BH = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) K = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-white") && i + 1 < argc) WHITE = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-dil") && i + 1 < argc) DIL = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sx") && i + 1 < argc) SX = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-sy") && i + 1 < argc) SY = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-stats")) SHOW = 1;
    }
    if (W <= 0 || H <= 0) { fprintf(stderr, "need -w -h\n"); return 2; }
    if (BH <= 0) BH = H / 8;
    if (Y0 < 0) Y0 = 0;
    if (Y0 + BH > H) BH = H - Y0;
    if (K < 2) K = 2;
    if (K > MAXK) K = MAXK;
    if (WHITE < 100) WHITE = 100;
    if (WHITE > 250) WHITE = 250;
    if (DIL < 0) DIL = 0;
    if (DIL > 6) DIL = 6;
    if (SX < 0) SX = 0;
    if (SX > 96) SX = 96;
    if (SY < 0) SY = 0;
    if (SY > 48) SY = 48;

    size_t ysz = (size_t)W * H, csz = (size_t)(W / 2) * (H / 2), fsz = ysz + csz * 2;
    size_t bsz = (size_t)W * BH;

    unsigned char *cur = malloc(fsz);
    unsigned char *plate = calloc(1, fsz);
    unsigned char *mask = malloc(bsz);
    unsigned char **hist = calloc(MAXK, sizeof(unsigned char *));
    unsigned char **hmask = calloc(MAXK, sizeof(unsigned char *));
    if (!cur || !plate || !mask || !hist || !hmask) return 2;
    for (int i = 0; i < MAXK; i++) { hist[i] = malloc(fsz); hmask[i] = malloc(bsz); }

    long long nframe = 0, plate_frame = -1, nplaced = 0, nfallback = 0;
    int have_plate = 0, idx = 0, nbuf = 0;

    while (fread(cur, 1, fsz, stdin) == fsz) {
        /* ---- 1. mask：白笔画 + 膨胀（吃掉黑描边/抗锯齿） ---- */
        /* contrast_mask：只看"够亮"不行 —— 亮背景（云、高光）会被整片误判，
         * 真字幕反而可能因为压缩掉到 175 以下而漏掉。
         * 字幕的确定性特征是"亮字 + 深色描边" → 3x3 邻域内动态范围极大。
         * 判据：亮点 或 亮点附近的高对比像素。 */
        memset(mask, 0, bsz);
        for (int y = 0; y < BH; y++) {
            for (int x = 0; x < W; x++) {
                int gy = Y0 + y;
                int v = cur[(size_t)gy * W + x];
                int lo = v, hi = v;
                for (int dy = -1; dy <= 1; dy++) {
                    int yy = gy + dy;
                    if (yy < 0 || yy >= H) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx;
                        if (xx < 0 || xx >= W) continue;
                        int u = cur[(size_t)yy * W + xx];
                        if (u < lo) lo = u;
                        if (u > hi) hi = u;
                    }
                }
                if (v >= WHITE || (hi - lo >= 70 && hi >= 140)) mask[(size_t)y * W + x] = 1;
            }
        }
        if (DIL > 0) {
            unsigned char *tmp = malloc(bsz);
            memcpy(tmp, mask, bsz);
            for (int y = 0; y < BH; y++)
                for (int x = 0; x < W; x++) {
                    if (!tmp[(size_t)y * W + x]) continue;
                    for (int dy = -DIL; dy <= DIL; dy++)
                        for (int dx = -DIL; dx <= DIL; dx++) {
                            int yy = y + dy, xx = x + dx;
                            if (yy >= 0 && yy < BH && xx >= 0 && xx < W)
                                mask[(size_t)yy * W + xx] = 1;
                        }
                }
            free(tmp);
        }
        int nm = 0;
        for (size_t p = 0; p < bsz; p++) nm += mask[p];

        /* ---- 2. 干净帧：字幕带里几乎没有字 ---- */
        if ((double)nm / (double)bsz < 0.015) {
            memcpy(plate, cur, fsz);
            have_plate = 1;
            plate_frame = nframe;
        }
        int plate_fresh = have_plate && (nframe - plate_frame) < 100;

        /* ---- 3. 全局位移搜索（在干净像素上算 SAD） ---- */
        int bdx = 0, bdy = 0;
        double best_sad = 1e18;
        if (plate_fresh && nm > 0) {
            /* 先粗搜（步长 4），再细搜 */
            for (int step = 4; step >= 1; step /= 2) {
                int cxs = bdx, cys = bdy;
                int lo = (step == 4) ? -SX : cxs - 4, hi = (step == 4) ? SX : cxs + 4;
                int lo2 = (step == 4) ? -SY : cys - 4, hi2 = (step == 4) ? SY : cys + 4;
                if (lo < -SX) lo = -SX;
                if (hi > SX) hi = SX;
                if (lo2 < -SY) lo2 = -SY;
                if (hi2 > SY) hi2 = SY;
                for (int dy = lo2; dy <= hi2; dy += step) {
                    for (int dx = lo; dx <= hi; dx += step) {
                        double sad = 0.0;
                        long n = 0;
                        /* 稀疏采样（每 3 行 1 行、每 2 列 1 列），够用且快 */
                        for (int y = 0; y < BH; y += 3) {
                            int sy = Y0 + y + dy;
                            if (sy < 0 || sy >= H) { sad = 1e18; break; }
                            for (int x = 0; x < W; x += 2) {
                                size_t p = (size_t)y * W + x;
                                if (mask[p]) continue;
                                int sx = x + dx;
                                if (sx < 0 || sx >= W) { sad = 1e18; break; }
                                int a = cur[(size_t)(Y0 + y) * W + x];
                                int b = plate[(size_t)sy * W + sx];
                                int d = a - b;
                                sad += (double)(d * d);
                                n++;
                            }
                            if (sad > 1e17) break;
                        }
                        if (n < 200) continue;
                        sad /= (double)n;
                        if (sad < best_sad) { best_sad = sad; bdx = dx; bdy = dy; }
                    }
                }
            }
        }
        /* SAD 太大说明 plate 和当前帧不是同一个画面（切场景）→ 别用 */
        int use_plate = plate_fresh && best_sad < 900.0;

        /* ---- 4. 填字像素 ---- */
        if (nm > 0) {
            for (int y = 0; y < BH; y++) {
                int gy = Y0 + y;
                size_t cy = (size_t)(gy / 2) * (W / 2);
                for (int x = 0; x < W; x++) {
                    size_t p = (size_t)y * W + x;
                    if (!mask[p]) continue;
                    size_t cu = ysz + cy + (size_t)(x / 2);
                    size_t cv = ysz + csz + cy + (size_t)(x / 2);
                    int done = 0;

                    if (use_plate) {
                        int sy = gy + bdy, sx = x + bdx;
                        if (sy >= 0 && sy < H && sx >= 0 && sx < W) {
                            size_t sp = (size_t)sy * W + sx;
                            if (plate[sp] < WHITE) {      /* 来源那点也不能是字 */
                                cur[(size_t)gy * W + x] = plate[sp];
                                size_t scu = ysz + (size_t)(sy / 2) * (W / 2) + (size_t)(sx / 2);
                                size_t scv = ysz + csz + (size_t)(sy / 2) * (W / 2) + (size_t)(sx / 2);
                                cur[cu] = plate[scu];
                                cur[cv] = plate[scv];
                                done = 1;
                                nplaced++;
                            }
                        }
                    }
                    if (!done) {
                        /* 退路：最近的历史帧里同位置不是字的像素 */
                        int m = nbuf < K ? nbuf : K;
                        for (int j = 1; j <= m && !done; j++) {
                            int h = (idx - j + MAXK) % MAXK;
                            if (!hmask[h][p]) {
                                cur[(size_t)gy * W + x] = hist[h][(size_t)gy * W + x];
                                cur[cu] = hist[h][cu];
                                cur[cv] = hist[h][cv];
                                done = 1;
                            }
                        }
                    }
                    if (!done) {
                        /* 最后兜底：上下邻近像素插值 */
                        int uy = -1, dy2 = -1;
                        for (int k2 = y - 1; k2 >= 0 && (y - k2) <= 16; k2--)
                            if (!mask[(size_t)k2 * W + x]) { uy = k2; break; }
                        for (int k2 = y + 1; k2 < BH && (k2 - y) <= 16; k2++)
                            if (!mask[(size_t)k2 * W + x]) { dy2 = k2; break; }
                        if (uy >= 0 || dy2 >= 0) {
                            int sy2 = (uy >= 0) ? uy : dy2;
                            cur[(size_t)gy * W + x] = cur[(size_t)(Y0 + sy2) * W + x];
                            size_t sc = ysz + (size_t)((Y0 + sy2) / 2) * (W / 2) + (size_t)(x / 2);
                            cur[cu] = cur[sc];
                            cur[cv] = cur[ysz + csz + (size_t)((Y0 + sy2) / 2) * (W / 2) + (size_t)(x / 2)];
                        }
                        nfallback++;
                    }
                }
            }
        }

        fwrite(cur, 1, fsz, stdout);
        memcpy(hist[idx], cur, fsz);
        memcpy(hmask[idx], mask, bsz);
        idx = (idx + 1) % MAXK;
        if (nbuf < K) nbuf++;
        nframe++;
        if (SHOW && nframe % 25 == 0)
            fprintf(stderr, "f%lld masked=%.1f%% plate=%s sad=%.0f shift=(%d,%d)\n",
                    nframe, 100.0 * nm / (double)bsz,
                    use_plate ? "yes" : "no", best_sad, bdx, bdy);
    }
    if (SHOW) fprintf(stderr, "done: %lld frames, placed=%lld fallback=%lld\n",
                      nframe, nplaced, nfallback);
    return 0;
}
