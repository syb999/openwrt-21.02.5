/* ---------------- 五线谱图片 → MIDI（内置 OMR） ----------------
 *
 * 能力边界（先说清楚，免得给假结果）：
 *   只处理「干净印刷体 + 单声部 + 高音谱号 + 无升降号」这一档。
 *   拍号按 4/4、每个符头一个四分音符（谱面时值识别不做，宁可节奏单调，
 *   也不瞎编）。识别不出五线谱就明确报错，绝不硬造旋律。
 *
 * 处理链：
 *   ffmpeg 解码成 PGM 灰度 → Otsu 二值 → 行投影找谱线（5 条一组）
 *   → 抹掉谱线 → 连通块找符头（尺寸/填充度筛选）→ y 定音高、x 定时间
 *   → 复用 do_render 出 MIDI。
 */

#define SHEET_MAX_STAFF 12
#define SHEET_MAX_HEAD  1500

typedef struct { int top, bot, space; int x0, x1; } staff_t;      /* 谱线中心（像素） */

/* 跑 ffmpeg 把任意图片转成 PGM(P5) 灰度；返回 malloc 的像素缓冲 */
static unsigned char *sheet_gray(const char *img, int scale, int *pw, int *ph) {
    char pgm[600];
    snprintf(pgm, sizeof(pgm), "/tmp/llama_agent/.sheet_%ld.pgm", (long)getpid());
    char cmd[1600];
    if (scale > 1)
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -y -i '%s' -vf scale=iw*%d:ih*%d:flags=lanczos "
                 "-pix_fmt gray -f image2 '%s' 2>/dev/null", img, scale, scale, pgm);
    else
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -y -i '%s' -pix_fmt gray -f image2 '%s' 2>/dev/null", img, pgm);
    if (system(cmd) != 0) { unlink(pgm); return NULL; }

    FILE *f = fopen(pgm, "rb");
    if (!f) { unlink(pgm); return NULL; }
    char magic[8] = {0};
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P5") != 0 ||
        fscanf(f, "%d %d %d", &w, &h, &maxv) != 3 || w <= 0 || h <= 0 ||
        w > 6000 || h > 6000) {
        fclose(f); unlink(pgm); return NULL;
    }
    fgetc(f);                                   /* 头后面那一个空白 */
    size_t n = (size_t)w * (size_t)h;
    unsigned char *px = malloc(n);
    if (!px) { fclose(f); unlink(pgm); return NULL; }
    if (fread(px, 1, n, f) != n) { free(px); fclose(f); unlink(pgm); return NULL; }
    fclose(f); unlink(pgm);
    *pw = w; *ph = h;
    return px;
}

/* Otsu：返回阈值，并原地把 px 变成 0/1（1=墨） */
static int sheet_binarize(unsigned char *px, size_t n) {
    long hist[256] = {0};
    for (size_t i = 0; i < n; i++) hist[px[i]]++;
    double total = (double)n, sum = 0;
    for (int i = 0; i < 256; i++) sum += (double)i * hist[i];
    double sumB = 0, wB = 0, best = -1;
    int thr = 128;
    for (int t = 0; t < 256; t++) {
        wB += (double)hist[t];
        if (wB == 0) continue;
        double wF = total - wB;
        if (wF == 0) break;
        sumB += (double)t * hist[t];
        double mB = sumB / wB, mF = (sum - sumB) / wF;
        double v = wB * wF * (mB - mF) * (mB - mF);
        if (v > best) { best = v; thr = t; }
    }
    for (size_t i = 0; i < n; i++) px[i] = (px[i] <= thr) ? 1 : 0;
    return thr;
}

/* 找五线谱：行投影里"很长的横线"聚成组，再挑连续 5 条间距均匀的 */
static int sheet_find_staves(unsigned char *b, int w, int h, staff_t *out, int maxo, int *pspace) {
    int *rowcnt = calloc((size_t)h, sizeof(int));
    if (!rowcnt) return 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *r = b + (size_t)y * w;
        int c = 0;
        for (int x = 0; x < w; x++) c += r[x];
        rowcnt[y] = c;
    }
    /* 长横线：暗像素超过宽度的 25%（谱线会横跨整行） */
    int thresh = w / 4;
    int *line = calloc((size_t)h, sizeof(int));
    int nl = 0;
    for (int y = 0; y < h; y++)
        if (rowcnt[y] >= thresh) line[nl++] = y;
    free(rowcnt);
    if (nl < 5) { free(line); return 0; }

    /* 相邻行归并成一条线（线本身有 1~3 像素粗） */
    int cen[512], nc = 0;
    for (int i = 0; i < nl && nc < 512; i++) {
        int s = line[i], e = line[i];
        while (i + 1 < nl && line[i + 1] - e <= 2) { e = line[++i]; }
        cen[nc++] = (s + e) / 2;
    }
    free(line);

    int ns = 0, space = 0;
    for (int i = 0; i + 4 < nc && ns < maxo; i++) {
        int g1 = cen[i + 1] - cen[i], g2 = cen[i + 2] - cen[i + 1];
        int g3 = cen[i + 3] - cen[i + 2], g4 = cen[i + 4] - cen[i + 3];
        int mn = g1, mx = g1;
        if (g2 < mn) mn = g2; if (g2 > mx) mx = g2;
        if (g3 < mn) mn = g3; if (g3 > mx) mx = g3;
        if (g4 < mn) mn = g4; if (g4 > mx) mx = g4;
        if (mn < 4 || mx > mn * 2) continue;          /* 间距要大致相等 */
        out[ns].top = cen[i];
        out[ns].bot = cen[i + 4];
        out[ns].space = (mx + mn) / 2;
        out[ns].x0 = 0;
        out[ns].x1 = w - 1;
        {   /* 用最上面那条线量左右边界，谱号区就在 x0 附近 */
            int yy = cen[i];
            const unsigned char *row2 = b + (size_t)yy * w;
            int a = -1, b2 = -1;
            for (int x2 = 0; x2 < w; x2++) if (row2[x2]) { a = x2; break; }
            for (int x2 = w - 1; x2 >= 0; x2--) if (row2[x2]) { b2 = x2; break; }
            if (a >= 0 && b2 > a) { out[ns].x0 = a; out[ns].x1 = b2; }
        }
        space = out[ns].space;
        ns++;
        i += 4;                                      /* 跳过这一组 */
    }
    if (pspace) *pspace = space;
    return ns;
}

/* 判谱号：高音谱号(G)整体在谱表上方伸出两条间距以上，低音谱号(F)
 * 基本不出谱表。看开头一小段里"谱表上方"的墨量就够区分这两个了。 */
static int staff_is_treble(unsigned char *b, int w, int h, const staff_t *s) {
    int sp = s->space;
    int y0 = s->top - sp * 2, y1 = s->top - sp / 2;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    int xmax = w / 6;
    if (y1 <= y0 || xmax <= 0) return 1;
    long ink = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < xmax; x++) ink += b[(size_t)y * w + x];
    long area = (long)(y1 - y0) * xmax;
    return (ink * 100 / (area ? area : 1)) > 2;      /* 墨量 >2% 就当高音谱号 */
}


/* 符头检测：按"列的墨迹厚度"找。
 *
 * 不用连通块 —— 符头骑在谱线上，一擦线就被切成上下两半（高度不足被筛掉），
 * 不擦线又会跟谱线连成一条几像素高、几百像素宽的东西。
 * 而符头所在的那几列，墨迹的垂直厚度明显比谱线（2~3 像素）大得多，
 * 所以按列统计"最长连续墨段"的厚度，厚度够的连续列就是符头。 */
typedef struct { int x, y; int u; } head_t;   /* u = 时值，1/16 为单位 */
static char g_annot_path[600];                /* 非空时把识别结果画成 BMP */

/* ================= 符头检测（结构性方案） =================
 *
 * 之前按"列的墨迹厚度"找符头 —— 遇到连桁八分音符（符头+符干+横桁粘成一坨）
 * 就整段丢掉，实测一张真实钢琴谱只认出 29/64 个。这里换成标准三段式：
 *   ① 抹谱线：只清"横向长连通段"（长度 > 3 个间距），保留压线处的符头墨迹
 *   ② 连通块 + 分类：按包围盒尺寸/填充度分成 符头 / 符干 / 横桁
 *   ③ 合并与切分：被抹线切开的上下半块合并；一个框里两个符头的按墨量凹陷切开
 * 时值靠"符头旁边有没有符干、符干末端有没有横桁"判定。
 */


typedef struct {
    int x0, y0, x1, y1;      /* 包围盒（band 坐标，加回 y0off 才是全图坐标） */
    int n;                   /* 墨点数 */
    int kind;                /* 0=符头 1=符干 2=横桁 3=其他 */
} cc_t;

/* 抹掉横向长连通段（谱线）：逐行扫，run 长度 > 3 个间距就清掉 */
static void erase_long_runs(unsigned char *b, int w, int h, int sp) {
    int lim = sp * 3;
    for (int y = 0; y < h; y++) {
        unsigned char *row = b + (size_t)y * w;
        int x = 0;
        while (x < w) {
            if (!row[x]) { x++; continue; }
            int s0 = x;
            while (x < w && row[x]) x++;
            if (x - s0 >= lim)
                memset(row + s0, 0, (size_t)(x - s0));
        }
    }
}

/* 连通块标记（4 邻域），返回块数 */
static int cc_label(unsigned char *b, int w, int h, cc_t *out, int maxo) {
    int *lab = calloc((size_t)w * h, sizeof(int));
    int *stk = malloc(sizeof(int) * (size_t)w * h);
    if (!lab || !stk) { free(lab); free(stk); return 0; }
    int nb = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i0 = (size_t)y * w + x;
            if (!b[i0] || lab[i0]) continue;
            if (nb >= maxo) { y = h; break; }
            int sp = 0;
            stk[sp++] = (int)i0;
            lab[i0] = nb + 1;
            int cx0 = x, cx1 = x, cy0 = y, cy1 = y, cnt = 0;
            while (sp > 0) {
                int idx = stk[--sp];
                int yy = idx / w, xx = idx % w;
                cnt++;
                if (xx < cx0) cx0 = xx;
                if (xx > cx1) cx1 = xx;
                if (yy < cy0) cy0 = yy;
                if (yy > cy1) cy1 = yy;
                int nb4[4][2] = {{yy - 1, xx}, {yy + 1, xx}, {yy, xx - 1}, {yy, xx + 1}};
                for (int k = 0; k < 4; k++) {
                    int ny = nb4[k][0], nx = nb4[k][1];
                    if (ny < 0 || ny >= h || nx < 0 || nx >= w) continue;
                    size_t ni = (size_t)ny * w + nx;
                    if (!b[ni] || lab[ni]) continue;
                    lab[ni] = nb + 1;
                    stk[sp++] = (int)ni;
                }
            }
            out[nb].x0 = cx0; out[nb].x1 = cx1;
            out[nb].y0 = cy0; out[nb].y1 = cy1;
            out[nb].n = cnt;
            out[nb].kind = 3;
            nb++;
        }
    }
    free(lab); free(stk);
    return nb;
}


/* ---------- 模型检测（YOLOv8n → RKNN，RK3566 NPU）----------
 * 用训练好的符号检测器取代手写连通块：符头位置/实心空心准得多。
 * 外部程序 /opt/omr/omr_detect 输出 "cls score cx cy w h"（原图坐标），
 * cls: 0 实心符头 1 空心符头 2 横桁 3 休止 4 谱号。
 * 返回 1 表示用上了模型，0 表示没有（调用方退回手写检测）。 */
typedef struct { float cx, cy, w, h, score; } mbox_t;

static int omr_model_ready(void) {
    return access("/opt/omr/omr_detect", X_OK) == 0 &&
           access("/opt/omr/model/best.rknn", R_OK) == 0;
}

/* 跑一次检测，把各类框读进数组 */
static int omr_run_model(const char *img, int scale, mbox_t *heads, int maxh,
                         int *nh, mbox_t *beams, int *nb, mbox_t *clefs, int *nc,
                         int *outw, int *outh) {
    *nh = *nb = *nc = 0;
    char raw[600], cmd[1600];
    snprintf(raw, sizeof(raw), "/tmp/llama_agent/.omr_%ld.rgb", (long)getpid());
    if (scale > 1)
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -y -i '%s' -vf scale=iw*%d:ih*%d:flags=lanczos "
                 "-pix_fmt rgb24 '%s' 2>/dev/null", img, scale, scale, raw);
    else
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -y -i '%s' -pix_fmt rgb24 '%s' 2>/dev/null", img, raw);
    if (system(cmd) != 0) { unlink(raw); return 0; }

    /* 尺寸：从文件大小反推不了宽高，单独问一次 ffprobe */
    char pcmd[1400];
    snprintf(pcmd, sizeof(pcmd),
             "ffprobe -v error -select_streams v:0 -show_entries stream=width,height "
             "-of csv=p=0:s=x '%s' 2>/dev/null", img);
    int w = 0, h = 0;
    FILE *pf = popen(pcmd, "r");
    if (pf) { if (fscanf(pf, "%dx%d", &w, &h) != 2) { w = h = 0; } pclose(pf); }
    if (w <= 0) { unlink(raw); return 0; }
    w *= (scale > 0 ? scale : 1);
    h *= (scale > 0 ? scale : 1);

    char rcmd[1600];
    snprintf(rcmd, sizeof(rcmd),
             "env LD_LIBRARY_PATH=/opt/glibc:/opt/rknpu /opt/glibc/ld-linux-aarch64.so.1 "
             "/opt/omr/omr_detect /opt/omr/model/best.rknn '%s' %d %d 0.35 2>/dev/null",
             raw, w, h);
    FILE *pp = popen(rcmd, "r");
    if (!pp) { unlink(raw); return 0; }
    int cls; float sc, cx, cy, bw, bh;
    while (fscanf(pp, "%d %f %f %f %f %f", &cls, &sc, &cx, &cy, &bw, &bh) == 6) {
        mbox_t m = {cx, cy, bw, bh, sc};
        if (cls <= 1) { if (*nh < maxh) heads[(*nh)++] = m; }
        else if (cls == 2) { if (*nb < 200) beams[(*nb)++] = m; }
        else if (cls == 4) { if (*nc < 20) clefs[(*nc)++] = m; }
    }
    pclose(pp);
    unlink(raw);
    *outw = w; *outh = h;
    return 1;
}

/* 抹线后的工作区：band 内的像素拷贝 + 抹线 + 连通块 + 判定符头和时值。
 * 输出 band 坐标（y 要加 y0off）。 */
static int detect_heads(unsigned char *src, int w, int h, const staff_t *s,
                        head_t *out, int maxo) {
    int sp = s->space;
    int y0 = s->top - sp * 3, y1 = s->bot + sp * 3;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    int bh = y1 - y0;
    if (bh <= 0 || sp < 4) return 0;

    unsigned char *band = malloc((size_t)w * bh);
    if (!band) return 0;
    for (int y = 0; y < bh; y++)
        memcpy(band + (size_t)y * w, src + (size_t)(y0 + y) * w, (size_t)w);

    erase_long_runs(band, w, bh, sp);

    static cc_t cc[4000];
    int nc = cc_label(band, w, bh, cc, 4000);
    if (!nc) { free(band); return 0; }

    /* 分类 */
    for (int i = 0; i < nc; i++) {
        int bw = cc[i].x1 - cc[i].x0 + 1;
        int bh2 = cc[i].y1 - cc[i].y0 + 1;
        double fill = (double)cc[i].n / (double)(bw * bh2);
        if (bw >= sp * 2 && bh2 <= sp)            cc[i].kind = 2;   /* 横桁 */
        else if (bw <= (int)(sp * 0.45) && bh2 >= (int)(sp * 1.2))
                                                  cc[i].kind = 1;   /* 符干 */
        else if (bw >= (int)(sp * 0.45) && bw <= (int)(sp * 1.8)
                 && bh2 >= (int)(sp * 0.35) && bh2 <= (int)(sp * 1.5)
                 && fill >= 0.18 && fill <= 1.0)
                                                  cc[i].kind = 0;   /* 符头 */
        else                                      cc[i].kind = 3;
    }

    /* 符头列表（band 坐标），先收着，后面合并/切分/定时值 */
    typedef struct { int x, y, bw, bh; int filled; } hc_t;
    static hc_t hs[2000];
    int nh = 0;
    for (int i = 0; i < nc && nh < 2000; i++) {
        if (cc[i].kind != 0) continue;
        int bw = cc[i].x1 - cc[i].x0 + 1;
        int bh2 = cc[i].y1 - cc[i].y0 + 1;
        double fill = (double)cc[i].n / (double)(bw * bh2);
        hs[nh].x = (cc[i].x0 + cc[i].x1) / 2;
        hs[nh].y = (cc[i].y0 + cc[i].y1) / 2;
        hs[nh].bw = bw;
        hs[nh].bh = bh2;
        hs[nh].filled = (fill > 0.55) ? 1 : 0;
        nh++;
    }

    /* 合并：被抹线切开的上下半块（同一个 x、竖直相距不到 1 个间距） */
    for (int i = 0; i < nh; i++) {
        if (hs[i].bw < 0) continue;
        for (int j = i + 1; j < nh; j++) {
            if (hs[j].bw < 0) continue;
            int dx = hs[i].x - hs[j].x;
            int dy = hs[i].y - hs[j].y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= sp * 0.4 && dy <= sp * 1.0 &&
                hs[i].bh <= sp * 0.8 && hs[j].bh <= sp * 0.8) {
                hs[i].y = (hs[i].y * hs[i].bh + hs[j].y * hs[j].bh) / (hs[i].bh + hs[j].bh);
                hs[i].bh = hs[i].bh + hs[j].bh;
                if (hs[j].filled) hs[i].filled = 1;
                hs[j].bw = -1;                        /* 标记已合并 */
            }
        }
    }

    /* 输出 + 时值（找附近符干、符干末端的横桁） */
    int nout = 0;
    int midY = (s->top + s->bot) / 2 - y0;          /* band 坐标下的中线 */
    for (int i = 0; i < nh && nout < maxo; i++) {
        if (hs[i].bw < 0) continue;

        /* ③ 先精判实心/空心：中心 1/3 区域墨量 >40% 才是实心符头。
         *    只靠整块填充度会把"被抹线削掉一半"的实心符误判成空心。 */
        {
            int cx0 = hs[i].x - hs[i].bw / 6, cx1 = hs[i].x + hs[i].bw / 6;
            int cy0 = hs[i].y - hs[i].bh / 6, cy1 = hs[i].y + hs[i].bh / 6;
            long ink = 0, area = 0;
            for (int yy = cy0; yy <= cy1; yy++) {
                if (yy < 0 || yy >= bh) continue;
                for (int xx = cx0; xx <= cx1; xx++) {
                    if (xx < 0 || xx >= w) continue;
                    ink += band[(size_t)yy * w + xx];
                    area++;
                }
            }
            hs[i].filled = (area && ink * 100 / area > 40) ? 1 : 0;
        }

        /* ① 谱号区：每行开头 4 个间距内不取符头（谱号碎片尺寸跟符头一样，
         *    只能靠位置排除） */
        if (hs[i].x < s->x0 + sp * 4) continue;

        /* ② 整小节休止符：贴中线、实心、宽而扁。跟全音符尺寸接近，
         *    但它是贴中线而不是贴某条线，按位置+形状排除 */
        if (hs[i].filled && hs[i].bh <= (int)(sp * 0.8)
            && hs[i].bw >= (int)(sp * 1.1) && hs[i].bw <= (int)(sp * 2.3)) {
            int dy = hs[i].y - midY;
            if (dy < 0) dy = -dy;
            if (dy <= sp) continue;
        }

        /* 切分：一个框里两个符头（宽度明显 > 1.4 间距）→ 平分两半 */
        int dup = 0;
        if (hs[i].bw > (int)(sp * 1.45)) {
            /* 看这个宽块的中间列墨量：真的有凹陷才算两个符头粘一起 */
            int cx = hs[i].x + 0;   /* band 坐标下的中心由下面统一用 x 反查 */
            (void)cx;
            int colmin = 1 << 20, colmax = 0;
            for (int xx = hs[i].x - sp / 6; xx <= hs[i].x + sp / 6; xx++) {
                if (xx < 0 || xx >= w) continue;
                int cnt = 0;
                for (int yy = hs[i].y - hs[i].bh / 2; yy <= hs[i].y + hs[i].bh / 2; yy++)
                    if (yy >= 0 && yy < bh && band[(size_t)yy * w + xx]) cnt++;
                if (cnt < colmin) colmin = cnt;
                if (cnt > colmax) colmax = cnt;
            }
            if (colmax > 0 && colmin * 100 / colmax < 45) dup = 1;
        }
        int copies = dup ? 2 : 1;
        for (int c = 0; c < copies && nout < maxo; c++) {
            int hx = hs[i].x + (dup ? (c ? hs[i].bw / 4 : -hs[i].bw / 4) : 0);
            int hy = hs[i].y;
            /* 找符干：kind==1 的块，x 落在符头左右 0.6 间距内、竖直与符头相接 */
            int stem_up = 0, stem_dn = 0;
            for (int k = 0; k < nc; k++) {
                if (cc[k].kind != 1) continue;
                int sx = (cc[k].x0 + cc[k].x1) / 2;
                if (sx < hx - sp * 0.7 || sx > hx + sp * 0.7) continue;
                int sbh = cc[k].y1 - cc[k].y0 + 1;
                if (sbh < sp) continue;
                if (cc[k].y1 <= hy + sp * 0.3) stem_up = 1;              /* 符干朝上 */
                else if (cc[k].y0 >= hy - sp * 0.3) stem_dn = 1;         /* 朝下 */
            }
            /* 硬约束：符头要么旁边有符干，要么是空心符（全音符）。
             * 没有这个，谱号碎片、力度记号、小节号都会被当成符头 ——
             * 实测不加这条会从 29 个虚报到 219 个。 */
            if (!stem_up && !stem_dn && hs[i].filled) continue;
            int u;
            if (!stem_up && !stem_dn) u = 16;                  /* 没符干 = 全音符 */
            else if (!hs[i].filled) u = 8;                     /* 空心带符干 = 二分 */
            else {
                /* 实心带符干：看符干末端有没有横桁 */
                int beam = 0;
                for (int k = 0; k < nc; k++) {
                    if (cc[k].kind != 2) continue;
                    int bx = (cc[k].x0 + cc[k].x1) / 2;
                    if (bx < hx - sp * 2 || bx > hx + sp * 2) continue;
                    if (stem_up && cc[k].y0 <= hy - sp * 1.5 && cc[k].y0 >= hy - sp * 4.5) beam++;
                    if (stem_dn && cc[k].y1 >= hy + sp * 1.5 && cc[k].y1 <= hy + sp * 4.5) beam++;
                }
                u = beam >= 2 ? 1 : (beam == 1 ? 2 : 4);
            }
            out[nout].x = hx;
            out[nout].y = hy + y0;                            /* 转回全图坐标 */
            out[nout].u = u;
            nout++;
        }
    }
    free(band);
    return nout;
}


/* ---------- 把识别结果画到图上（24 位 BMP，浏览器能直接显示） ----------
 * 方框颜色按时值区分：红=四分/更短，蓝=二分，绿=全音符，黄=节拍器外的长音。
 * 不用 ffmpeg drawtext、不引字体：只用矩形，省得踩引号/滤镜的坑。 */
static int write_bmp_annot(const char *out, unsigned char *gray, int w, int h,
                          const head_t *hs, int n) {
    int div = (w > 1200) ? 3 : ((w > 700) ? 2 : 1);      /* 输出缩小，别几 MB */
    int ow = w / div, oh = h / div;
    if (ow < 1 || oh < 1) return -1;
    int rowbytes = (ow * 3 + 3) & ~3;
    long isize = (long)rowbytes * oh;
    FILE *f = fopen(out, "wb");
    if (!f) return -1;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    long fsize = 54 + isize;
    memcpy(hdr + 2, &fsize, 4);
    int off = 54;
    memcpy(hdr + 10, &off, 4);
    int ih = 40;
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &ow, 4);
    memcpy(hdr + 22, &oh, 4);
    short planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &isize, 4);
    fwrite(hdr, 1, 54, f);

    unsigned char *row = malloc((size_t)rowbytes);
    if (!row) { fclose(f); return -1; }
    for (int y = oh - 1; y >= 0; y--) {             /* BMP 自下而上 */
        for (int x = 0; x < ow; x++) {
            unsigned char v = gray[(size_t)(y * div) * w + x * div] ? 0 : 255;
            row[x * 3] = v; row[x * 3 + 1] = v; row[x * 3 + 2] = v;
        }
        for (int i = 0; i < n; i++) {
            int cx = hs[i].x / div, cy = hs[i].y / div;
            if (cy != y) continue;
            int u = hs[i].u;
            unsigned char B = (u <= 4) ? 40 : (u == 8 ? 200 : 60);
            unsigned char G = (u <= 4) ? 40 : (u == 8 ? 120 : 180);
            unsigned char R = (u <= 4) ? 220 : (u == 8 ? 40 : 40);
            int r2 = (div > 1) ? 5 : 9;
            for (int d = -r2; d <= r2; d++) {
                int xx = cx + d;
                if (xx < 0 || xx >= ow) continue;
                row[xx * 3] = B; row[xx * 3 + 1] = G; row[xx * 3 + 2] = R;
            }
        }
        fwrite(row, 1, (size_t)rowbytes, f);
    }
    free(row);
    fclose(f);
    return 0;
}

/* 五线谱图 → 音符数组。返回音符数，0 表示没认出来 */
static int sheet_to_notes(const char *img, note_t *notes, int *xs, int *uis, int maxn,
                          int *pstaves, int *pheads, char *why, size_t whycap) {
    /* 第一趟：原尺寸量"谱线间距"。OMR 的老规矩：间距 ≥15 像素才可靠
     * （Audiveris 在 7 像素时直接判"分辨率太低，建议 300 DPI"）。
     * 间距太小就用 ffmpeg lanczos 放大再测 —— 手机上截的谱图大多是这种。 */
    int w = 0, h = 0;
    unsigned char *px = sheet_gray(img, 1, &w, &h);
    if (!px) { snprintf(why, whycap, "图片读不进来（ffmpeg 解不了这张图）"); return 0; }
    sheet_binarize(px, (size_t)w * (size_t)h);

    staff_t st[SHEET_MAX_STAFF];
    int space = 0;
    int ns = sheet_find_staves(px, w, h, st, SHEET_MAX_STAFF, &space);
    int upscaled = 1;
    if (ns > 0 && space > 0 && space < 15) {
        int sc = (16 + space - 1) / space;
        if (sc < 2) sc = 2;
        if (sc > 4) sc = 4;
        unsigned char *px2 = sheet_gray(img, sc, &w, &h);
        if (px2) {
            free(px);
            px = px2;
            sheet_binarize(px, (size_t)w * (size_t)h);
            ns = sheet_find_staves(px, w, h, st, SHEET_MAX_STAFF, &space);
            upscaled = sc;
        }
    }
    if (!ns) {
        free(px);
        snprintf(why, whycap, "没找到五线谱（没检测到 5 条等距横线）：这张图不是谱面，或拍得太斜/太糊");
        return 0;
    }
    if (upscaled > 1)
        fprintf(stderr, "  [omr] 原图谱线间距太小，已放大 %d 倍后再识别（间距 %.0f px）\n",
                upscaled, (double)space);

    /* 高音谱号的音名表：从谱表最下面一条线(E4=64)往上，每半个间距一级 */
    /* 以谱表底线 E4 为 0 级，往上的半音偏移：E F G A B C D = 0 1 3 5 7 8 10 */
    static const int STEP_T[7] = {0, 1, 3, 5, 7, 8, 10};      /* 从 E 起 */
    static const int STEP_B[7] = {0, 2, 4, 5, 7, 9, 10};      /* 从 G 起 */
    int nn = 0, heads = 0;
    head_t *hdall = malloc(sizeof(head_t) * 2000);
    int ndall = 0;

    /* ---- 优先用训练好的检测器（YOLOv8n on NPU）----
     * 手写的连通块法在密集/连桁谱面上位置和实心判定都不稳；
     * 模型给的是"符头中心 + 实心/空心"，把几何部分（谱表/音高/时值）留给后面的逻辑。 */
    mbox_t *mheads = NULL, *mbeams = NULL, *mclefs = NULL;
    int nmh = 0, nmb = 0, nmc = 0, mw = 0, mh = 0, use_model = 0;
    if (omr_model_ready() && getenv("OMR_NO_MODEL") == NULL) {
        mheads = malloc(sizeof(mbox_t) * 800);
        mbeams = malloc(sizeof(mbox_t) * 200);
        mclefs = malloc(sizeof(mbox_t) * 20);
        if (mheads && mbeams && mclefs &&
            omr_run_model(img, upscaled, mheads, 800, &nmh, mbeams, &nmb, mclefs, &nmc, &mw, &mh) &&
            nmh > 3) {
            use_model = 1;
            fprintf(stderr, "  [omr] 模型检测：符头 %d、横桁 %d、谱号 %d（图 %dx%d）\n",
                    nmh, nmb, nmc, mw, mh);
        }
    }
    head_t *hd = malloc(sizeof(head_t) * 4000);
    if (!hd) { free(px); return 0; }

    for (int s = 0; s < ns && nn < maxn; s++) {
        int sp = st[s].space;
        if (sp < 4) continue;
        /* 大谱表判定（实测数据驱动）：
         * 两只老虎那张谱是 5 行 × 2 组 = 10 组，组内和组间的垂直间距几乎一样
         * （253px 对 272px，都是 ~10 个五线间距，因为歌词占了一行），
         * 所以"按间距配对"分不开高/低音谱表。
         * 可靠规则：谱表数成偶数时，按上下顺序奇偶配对 —— 偶数=上行(高音)，奇数=下行(低音)。
         * 谱表数为奇数（单行谱）时退回按谱号墨迹判断。 */
        int treble, lower_staff = 0;
        if (ns % 2 == 0) {
            treble = (s % 2 == 0) ? 1 : 0;
            lower_staff = (s % 2 == 1);
        } else {
            treble = staff_is_treble(px, w, h, &st[s]);
        }
        /* 注意：不能简单地"跳过下行谱表" —— 实测这张谱的符头大部分落在下行，
         * 一跳就只剩 2 个音。旋律提取要在音符生成之后再按"同一时刻取最高音"做。 */
        (void)lower_staff;
        /* 按列厚度找符头（谱线不擦：符头骑在线上，擦线会把它切成两半） */
        int nb;
        if (use_model) {
            /* 把这个谱表范围内的模型符头转成 head_t（x,y,u）
             * u 是时值（1/16 为单位）：先按实心/空心给基础值，
             * 横桁覆盖到的实心符头降成八分（2）。 */
            nb = 0;
            int stem_heads[4000], nstem = 0;
            for (int mi = 0; mi < nmh && nb < 4000; mi++) {
                int my = (int)mheads[mi].cy;
                int mx = (int)mheads[mi].cx;
                if (my < st[s].top - sp * 3 || my > st[s].bot + sp * 3) continue;
                int hollow = 0;
                /* 空心/实心由模型给不了（合并成一类了），用图上的墨量判定 */
                int x0 = mx - sp / 2, x1 = mx + sp / 2, y0 = my - sp / 2, y1 = my + sp / 2;
                int ink = 0, area = 0;
                for (int yy = y0; yy <= y1; yy++)
                    for (int xx = x0; xx <= x1; xx++) {
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        area++;
                        if (px[(size_t)yy * w + xx]) ink++;
                    }
                if (area > 0 && ink * 100 / area < 45) hollow = 1;
                int u = hollow ? 8 : 4;                 /* 空心→二分, 实心→四分 */
                for (int bi = 0; bi < nmb; bi++) {
                    float dx = mheads[mi].cx - mbeams[bi].cx;
                    float dy = mheads[mi].cy - mbeams[bi].cy;
                    if (fabsf(dx) < mbeams[bi].w * 0.6f && fabsf(dy) < mbeams[bi].h * 0.8f + sp) {
                        u = 2;                           /* 有横桁 → 八分 */
                        break;
                    }
                }
                /* 横桁检不到时用几何判时值：符头左右两侧扫竖列墨迹找符干，
                 * 有符干 → 至少八分；相邻符干符头间距近 → 两者都判八分 */
                if (u == 4) {
                    int has_stem = 0;
                    for (int side = -1; side <= 1 && !has_stem; side += 2) {
                        for (int dxp = 1; dxp <= (int)(sp * 0.7f) && !has_stem; dxp++) {
                            int xr = mx + side * ((int)(sp * 0.35f) + dxp);
                            if (xr < 0 || xr >= w) continue;
                            for (int dir = -1; dir <= 1 && !has_stem; dir += 2) {
                                int run = 0, gap = 0;
                                for (int k = 1; k < sp * 3; k++) {
                                    int yy = my + dir * k;
                                    if (yy < 0 || yy >= h) break;
                                    if (px[(size_t)yy * w + xr]) { run++; gap = 0; }
                                    else if (++gap > 3) break;
                                }
                                if (run >= (int)(sp * 1.5f)) has_stem = 1;
                            }
                        }
                    }
                    if (has_stem) {
                        /* 有符干：先记四分，等间距复核时再把"靠得很近的一对"降成八分 */
                        if (nstem < 4000) stem_heads[nstem++] = nb;
                    }
                }
                hd[nb].x = mx; hd[nb].y = my; hd[nb].u = u;
                nb++;
            }
            /* stem_heads 存的是处理顺序，必须先按 x 排序，否则"相邻"不是谱面上的相邻 */
            for (int a = 0; a < nstem; a++)
                for (int b = a + 1; b < nstem; b++)
                    if (hd[stem_heads[b]].x < hd[stem_heads[a]].x) {
                        int t = stem_heads[a]; stem_heads[a] = stem_heads[b]; stem_heads[b] = t;
                    }
            {
                for (int a = 0; a + 1 < nstem; a++) {
                    int i0 = stem_heads[a], i1 = stem_heads[a + 1];
                    int gapx = hd[i1].x - hd[i0].x;
                    if (gapx < 0) gapx = -gapx;          /* 符头未必按 x 排序 */
                    if (gapx > 0 && gapx < (int)(sp * 3.0f)) {
                        hd[i0].u = 2;
                        hd[i1].u = 2;
                    }
                }
            }
        } else {
            nb = detect_heads(px, w, h, &st[s], hd, 4000);
        }
        for (int i = 0; i < nb && ndall < 2000; i++) hdall[ndall++] = hd[i];
        for (int i = 0; i < nb && nn < maxn; i++) {
            /* y → 音高：以最下面那条线为锚点，每半间距一级。
             * 高音谱号底线 E4(64)，音名 E F G A B C D；
             * 低音谱号底线 G2(43)，音名 G A B C D E F。 */
            double steps = (double)(st[s].bot - hd[i].y) / (sp / 2.0);
            int n = (int)floor(steps / 2.0 + 0.5);                /* 半间距一级 → 全间距两级 */
            int idx = n;                                          /* n=0 → E4 */
            int oct = 0;
            while (idx < 0) { idx += 7; oct--; }
            while (idx >= 7) { idx -= 7; oct++; }
            const int *STEPSEL = treble ? STEP_T : STEP_B;
            int midi = (treble ? 64 : 43) + STEPSEL[idx] + 12 * oct;
            if (midi < 36 || midi > 96) continue;
            /* 取最高声部：同一 x 位置（同一拍）如果有更高的符头，就丢掉这个音。
             * 钢琴谱里低音伴奏与旋律同拍出现，不筛就会把主旋律盖掉
             * （两只老虎一直"听不出旋律"就是这原因）。 */
            int is_low = 0;
            for (int q = 0; q < ndall; q++) {
                if (hdall[q].x > hd[i].x - sp && hdall[q].x < hd[i].x + sp &&
                    hdall[q].y < hd[i].y - sp / 2) { is_low = 1; break; }
            }
            if (is_low) continue;
            notes[nn].pitch = midi;
            notes[nn].start = 0;
            notes[nn].dur = 0;
            notes[nn].ch = 0;
            if (xs) xs[nn] = hd[i].x;
            if (uis) uis[nn] = hd[i].u > 0 ? hd[i].u : 4;
            nn++;
            heads++;
        }
    }
    if (pstaves) *pstaves = ns;
    if (g_annot_path[0] && hdall) write_bmp_annot(g_annot_path, px, w, h, hdall, ndall);
    free(hdall);
    if (pheads) *pheads = heads;
    if (!nn) {
        free(hd);
        free(px);
        snprintf(why, whycap, "找到 %d 组五线谱，但没认出符头（可能是手写/装饰体，或分辨率太低）", ns);
        return 0;
    }
    return nn;
}
