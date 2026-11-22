#!/usr/bin/env python3
"""Video subtitle removal on rk3566: MI-GAN on the NPU + ffmpeg pipe.

  python3 desub_npu.py in.mp4 out.mp4 [--white 190] [--band 0.70]
                      [--model M] [--bin B] [--ffmpeg /usr/bin/ffmpeg-rkrga]

Frames go through: decode -> text mask (bbox in the bottom band) -> 512x512
MI-GAN inference (rknn) -> paste only the repaired box -> encode.
"""
import argparse
import collections
import os
import queue
import threading
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np

SZ = 512


_RCACHE = {}


def _idx(ih, iw, h, w):
    key = (ih, iw, h, w)
    c = _RCACHE.get(key)
    if c is None:
        ys = (np.arange(h) + 0.5) * (ih / float(h)) - 0.5
        xs = (np.arange(w) + 0.5) * (iw / float(w)) - 0.5
        y0 = np.clip(np.floor(ys).astype(np.int32), 0, ih - 1)
        x0 = np.clip(np.floor(xs).astype(np.int32), 0, iw - 1)
        c = (y0, np.clip(y0 + 1, 0, ih - 1), np.clip(ys - y0, 0, 1).astype(np.float32),
             x0, np.clip(x0 + 1, 0, iw - 1), np.clip(xs - x0, 0, 1).astype(np.float32))
        _RCACHE[key] = c
    return c


def resize_f(img, w, h):
    """float 版双线性缩放（背景估计用）。"""
    ih, iw = img.shape[:2]
    y0, y1, wy, x0, x1, wx = _idx(ih, iw, h, w)
    wy = wy[:, None, None]
    wx = wx[None, :, None]
    v = img[y0] * (1.0 - wy) + img[y1] * wy
    return v[:, x0] * (1.0 - wx) + v[:, x1] * wx


def resize(img, w, h):
    """Bilinear resize, HxWxC uint8 -> hxwC uint8（可分离两趟 + 索引缓存）。

    原始版本对每个输出像素做 4 次全图 gather（0.11s/次）；
    这里先纵向插值再横向，并把索引/权重按尺寸缓存，降到 ~0.03s。
    """
    return np.clip(resize_f(img.astype(np.float32), w, h) + 0.5, 0, 255).astype(np.uint8)


def box_blur(g, r):
    """Integral-image box blur. 用两段一维索引代替 np.ix_（省掉 meshgrid，快 3~4 倍）。"""
    g = g.astype(np.float32)
    h, w = g.shape
    gi = np.zeros((h + 1, w + 1), np.float32)
    gi[1:, 1:] = np.cumsum(np.cumsum(g, 0), 1)
    y0 = np.clip(np.arange(h) - r, 0, h)
    y1 = np.clip(np.arange(h) + r + 1, 0, h)
    x0 = np.clip(np.arange(w) - r, 0, w)
    x1 = np.clip(np.arange(w) + r + 1, 0, w)
    s = (gi[y1][:, x1] - gi[y0][:, x1]) - (gi[y1][:, x0] - gi[y0][:, x0])
    n = ((y1 - y0)[:, None] * (x1 - x0)[None, :]).astype(np.float32)
    return s / n


def text_mask_lc(gray, band_frac, contrast=30, radius=12):
    """Local-contrast (white top-hat) text mask.

    字幕是"比紧邻背景亮一截"的笔划；用绝对亮度阈值会把画面里本来就亮的
    东西（金币、白衣服、天空）也选进去，结果整片场景被重绘。
    用 gray - box_blur(gray) 只留下比周围亮的细节。
    """
    bg = box_blur(gray, radius)
    m = (gray.astype(np.float32) - bg) > contrast
    m[:int(gray.shape[0] * band_frac), :] = False
    return m


def refine_rows(mask, pad=4, frac=0.30, min_px=10):
    """只保留一条字幕行：在所有"行掩码密集"的连续段里取质量最大的那段。

    底层带里常混有别的亮细节（金币边缘、衣角），它们在竖直方向连不成一条
    和字幕同样密的带。按段取质量最大的一段，掩码包围盒就不会被拉到画面里，
    后面取"周边环带"时也就不会被画面内容污染。
    """
    rows = mask.sum(1)
    if rows.max() < min_px:
        return np.zeros_like(mask)
    thr = max(min_px, rows.max() * frac)
    sel = rows > thr

    best, cur = None, None
    for y in range(len(sel) + 1):
        on = y < len(sel) and sel[y]
        if on and cur is None:
            cur = [y, y]
        elif on:
            cur[1] = y
        elif cur is not None:
            if best is None or rows[cur[0]:cur[1] + 1].sum() > rows[best[0]:best[1] + 1].sum():
                best = cur
            cur = None
    if best is None:
        return np.zeros_like(mask)

    out = np.zeros_like(mask)
    y0 = max(0, best[0] - pad)
    y1 = min(mask.shape[0], best[1] + pad + 1)
    out[y0:y1] = mask[y0:y1]
    return out


def erode(mask, radius):
    return ~dilate(~mask, radius)


def feather_blend(base, filled, hole, width=5):
    """把填充结果按距离渐变地混进原图，消掉修补区的硬边框。

    只在掩码包围盒（外扩 width+2）里算 —— 全图算一次要 400ms，
    光是这步就吃掉一帧一半的时间。
    """
    ys, xs = np.nonzero(hole)
    if len(ys) == 0:
        return base
    y0 = max(0, int(ys.min()) - width - 2)
    y1 = min(hole.shape[0], int(ys.max()) + width + 3)
    x0 = max(0, int(xs.min()) - width - 2)
    x1 = min(hole.shape[1], int(xs.max()) + width + 3)

    sh = hole[y0:y1, x0:x1]
    core = erode(sh, width)
    alpha = np.zeros(sh.shape, np.float32)
    alpha[core] = 1.0

    cur = core.copy()
    for i in range(width, 0, -1):
        nxt = dilate(cur, 1) & sh
        alpha[nxt & ~cur] = 1.0 - (i / float(width + 1))
        cur = nxt

    a = alpha[..., None]
    out = base.copy()
    sb = base[y0:y1, x0:x1].astype(np.float32)
    sf = filled[y0:y1, x0:x1].astype(np.float32)
    out[y0:y1, x0:x1] = np.clip(sb * (1.0 - a) + sf * a + 0.5, 0, 255).astype(np.uint8)
    return out


def ring_std(big, ring):
    """环带的鲁棒标准差：先按亮度中位数 3*MAD 剔除字幕白描边等离群点。"""
    ys, xs = np.nonzero(ring)
    if len(ys) == 0:
        return 255.0
    if len(ys) > 4000:
        pick = np.linspace(0, len(ys) - 1, 4000).astype(int)
        ys, xs = ys[pick], xs[pick]
    lum = big[ys, xs].astype(np.float32).mean(1)
    med = float(np.median(lum))
    mad = float(np.median(np.abs(lum - med)))
    keep = np.abs(lum - med) < 3.0 * max(mad, 3.0)
    if keep.sum() >= 100:
        ys, xs = ys[keep], xs[keep]
    return float(np.std(big[ys, xs].astype(np.float32), axis=0).max())


def match_background(gen, big, hole, ring, flat_thr=13.0, add_noise=False, seed=1234,
                     state=None, absorb=False):
    """把修补结果的低频色调对齐到周边背景。

    纯色底：铺拟合出来的平面色；纹理底：模型输出原样用。
    state 用于跨帧平滑（避免填充色逐帧微变造成的闪烁）。
    """
    if hole.sum() < 100 or ring.sum() < 200:
        return gen, hole

    out = gen.astype(np.float32).copy()
    ys, xs = np.nonzero(ring)
    if len(ys) > 4000:                      # 采样，够拟合就行
        pick = np.linspace(0, len(ys) - 1, 4000).astype(int)
        ys, xs = ys[pick], xs[pick]

    # 鲁棒剔除：字幕的白描边/晕光会落在掩码外，混进环带会把拟合色拉亮。
    # 按亮度中位数做 3*MAD 剔除，只留真正的背景像素。
    lum = big[ys, xs].astype(np.float32).mean(1)
    med = float(np.median(lum))
    mad = float(np.median(np.abs(lum - med)))
    keep = np.abs(lum - med) < 3.0 * max(mad, 3.0)
    if keep.sum() >= 100:
        ys, xs = ys[keep], xs[keep]

    A = np.stack([xs, ys, np.ones_like(xs)], 1).astype(np.float32)
    hy, hx = np.nonzero(hole)
    Hm = np.stack([hx, hy, np.ones_like(hx)], 1).astype(np.float32)

    # 周边是不是纯色（允许有线性渐变）：纯色时模型纹理毫无意义，
    # 直接用拟合出来的背景色铺平，才不会出现"另一种橙"的色块。
    # 注意 std 用"过滤后"的环带算，否则字幕白描边会把纹理场景也算成纯色。
    rstd = float(np.std(big[ys, xs].astype(np.float32), axis=0).max())
    flat = rstd < flat_thr
    if not flat:
        return gen, hole                      # 纹理场景：模型输出原样用，不做色调偏移

    # 局部背景估计：对"非洞"像素做归一化卷积（box blur 的分子/分母），
    # 得到每个像素处"周边只有背景时"的颜色。平面拟合在字幕带上下渐变时
    # 会整体偏高/偏低 5~6 个色阶，留下一块矩形；局部估计能精确跟随渐变。
    m = (~hole).astype(np.float32)
    hh = int(hy.max() - hy.min()) if len(hy) else 20
    rr = max(12, hh // 2 + 6)

    def background(mask):
        # 背景是低频的：在 1/4 分辨率上估计再放大，快 16 倍
        sm = 4
        si = big[::sm, ::sm].astype(np.float32)
        mk = mask[::sm, ::sm]
        r4 = max(2, rr // sm)
        d = box_blur((~mk).astype(np.float32), r4)
        out4 = np.empty_like(si)
        for cc in range(3):
            n = box_blur(si[..., cc] * (~mk), r4)
            out4[..., cc] = n / np.maximum(d, 1e-3)
        return resize_f(out4, SZ, SZ)

    bg = background(hole)

    # 残余吸收：字幕的抗锯齿/描边常常落在膨胀后的掩码之外。
    # 有 --margin 兜底时这步可以省掉（省一遍背景估计，约 50ms/帧）。
    if absorb:
        res = np.abs(big.astype(np.float32) - bg).max(2)
        near = dilate(hole, 6)
        extra = near & (res > 10.0) & ~hole
        if extra.sum() > 0:
            hole = hole | extra
            hy, hx = np.nonzero(hole)
            bg = background(hole)

    rng = np.random.default_rng(seed)
    for c in range(3):
        sigma = float(np.std((big[..., c].astype(np.float32) - bg[..., c])[ring])) if ring.any() else 0.0
        noise = (rng.normal(0.0, max(sigma, 0.8), size=len(hy)).astype(np.float32)
                 if add_noise else 0.0)
        out[hy, hx, c] = np.clip(bg[..., c][hy, hx] + noise, 0, 255)

    return out.astype(np.uint8), hole


def gray_of(rgb):
    """Rec.601 luma as uint8 array (H, W). 用 uint32 防止乘 299 溢出 uint16。"""
    g = (rgb[..., 0].astype(np.uint32) * 299 + rgb[..., 1].astype(np.uint32) * 587 +
         rgb[..., 2].astype(np.uint32) * 114) // 1000
    return g.astype(np.uint8)


def dilate(mask, radius):
    """Box dilation via cumulative sums (O(N) per axis, not 2r passes of np.roll).

    np.roll 版本在 radius=34 时要跑 136 次全图拷贝（~70ms/帧）；
    累积和窗口法两次扫描就够（~5ms）。
    """
    if radius <= 0:
        return mask
    r = radius
    h, w = mask.shape

    p = np.zeros((h, w + 2 * r), bool)
    p[:, r:r + w] = mask
    c = np.cumsum(p, axis=1, dtype=np.int32)
    c = np.concatenate([np.zeros((h, 1), np.int32), c], axis=1)
    m = (c[:, 2 * r + 1:] - c[:, :-2 * r - 1]) > 0

    p = np.zeros((h + 2 * r, w), bool)
    p[r:r + h] = m
    c = np.cumsum(p, axis=0, dtype=np.int32)
    c = np.concatenate([np.zeros((1, w), np.int32), c], axis=0)
    return (c[2 * r + 1:] - c[:-2 * r - 1]) > 0


def text_mask(rgb, white, band):
    """Bounding box of bright text pixels in the bottom band -> (x1,y1,x2,y2) or None."""
    h, w = rgb.shape[:2]
    y0 = int(h * band)
    sub = rgb[y0:, :, :]
    g = (sub[..., 0].astype(np.uint32) * 299 + sub[..., 1].astype(np.uint32) * 587 +
         sub[..., 2].astype(np.uint32) * 114) // 1000
    ys, xs = np.where(g > white)
    if len(ys) < 30:                     # 没有字幕
        return None
    pad = 10
    return (max(0, int(xs.min()) - pad), max(0, int(ys.min()) + y0 - pad),
            min(w, int(xs.max()) + pad), min(h, int(ys.max()) + y0 + pad))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp')
    ap.add_argument('out')
    ap.add_argument('--white', type=int, default=45, help='局部对比度阈值（gray - 邻域均值）')
    ap.add_argument('--band', type=float, default=0.78)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument('--model', default=os.path.join(here, 'migan_noclip.rknn'))
    ap.add_argument('--bin', default=os.path.join(here, 'migan_rknn'))
    ap.add_argument('--loader', default='/opt/glibc/ld-linux-aarch64.so.1')
    ap.add_argument('--libpath', default='/opt/glibc:/opt/rknpu')
    ap.add_argument('--ffmpeg', default='/usr/bin/ffmpeg-rkrga')
    ap.add_argument('--hwdec', action='store_true', help='用 rkmpp 硬解（慢消费时会丢帧）')
    ap.add_argument('--dilate', type=int, default=10, help='文字掩码膨胀半径（像素，512 空间）')
    ap.add_argument('--ring', type=int, default=24, help='色调校准用的环带宽度（像素）')
    ap.add_argument('--ring-pad', type=int, default=28, help='取环带时包围盒外扩量')
    ap.add_argument('--feather', type=int, default=6, help='边界羽化宽度（像素）')
    ap.add_argument('--margin', type=int, default=8, help='掩码外再扩一圈，吞掉编码振铃')
    ap.add_argument('--absorb', action='store_true', help='再做一遍残余吸收（慢，通常不需要）')
    ap.add_argument('--no-thread', action='store_true', help='关掉流水线重叠（同步跑，调试用）')
    ap.add_argument('--no-reuse', action='store_true', help='关掉逐句复用')
    ap.add_argument('--reuse-mask', type=float, default=0.03, help='复用：掩码变化阈值')
    ap.add_argument('--reuse-bg', type=float, default=6.0, help='复用：背景平均差阈值')
    ap.add_argument('--flat-thr', type=float, default=13.0, help='纯色底判定阈值')
    ap.add_argument('--noise', action='store_true', help='纯色底填充时加匹配噪声（默认不加）')
    ap.add_argument('--lc-radius', type=int, default=12, help='局部对比度的邻域半径')
    ap.add_argument('--bitrate', default='6M')
    ap.add_argument('--frames', type=int, default=0, help='只处理前 N 帧（调试）')
    a = ap.parse_args()

    probe = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
         'stream=width,height,r_frame_rate', '-of', 'csv=p=0', a.inp],
        capture_output=True, text=True).stdout.strip().split(',')
    W, H = int(probe[0]), int(probe[1])
    num, den = probe[2].split('/')
    fps = float(num) / float(den)
    sys.stderr.write('%dx%d @ %.2ffps\n' % (W, H, fps))

    tmp = os.environ.get('DESUB_TMP') or ('/dev/shm' if os.path.isdir('/dev/shm') else '/tmp')
    d = tempfile.mkdtemp(dir=tmp)

    # 三段式：① 先把整段快速解码成裸 rgb 文件（消费端不阻塞，硬解不会丢帧/中断），
    #         ② 逐帧过 NPU 处理并写入另一个裸文件，
    #         ③ 最后一次性用 h264_rkmpp 编码。
    #
    # 为什么不能边解边处理：下游每帧要 0.45s（NPU），实测 rkmpp 在这种慢消费下
    # 到第 111 帧就断了（250 帧只出来 111 帧）；软件解码同样在慢消费下丢帧。
    # 编码侧也一样，边处理边喂 h264_rkmpp 会按实时节奏丢帧，必须文件输入。
    raw_in = os.path.join(d, 'in.rgb')
    dec_cmd = [a.ffmpeg, '-y', '-v', 'error']
    if a.hwdec:
        dec_cmd += ['-hwaccel', 'rkmpp']
    dec_cmd += ['-i', a.inp, '-f', 'rawvideo', '-pix_fmt', 'rgb24', raw_in]
    r = subprocess.run(dec_cmd)
    if r.returncode != 0:
        sys.stderr.write('decode failed (rc=%d)\n' % r.returncode)
        return
    dec = open(raw_in, 'rb')
    rawpath = os.path.join(d, 'out.rgb')
    fw = open(rawpath, 'wb')
    p_img = os.path.join(d, 'i.rgb')
    p_msk = os.path.join(d, 'm.gray')
    p_out = os.path.join(d, 'o.rgb')
    fsz = SZ * SZ * 3

    n = 0
    tstate = {}          # 跨帧状态：掩码并集 / 迟滞 / 填充色 EMA
    t_npu = 0.0
    cache = None         # 逐句复用：上一帧修好的补丁
    pending = collections.deque()

    class NpuWorker:
        """常驻推理进程 + 后台线程。

        NPU 本身是单线程的，重叠的意义是：主线程在准备第 N+1 帧（掩码/背景/
        羽化，约 0.6s）时，后台线程正在等第 N 帧的推理结果（1.15s），
        两者并行 → 每帧从 1.75s 降到 1.15s。
        """
        def __init__(self, args):
            self.a = args
            self.proc = None
            self.qin = queue.Queue(maxsize=3)
            self.qout = queue.Queue(maxsize=3)
            self.th = threading.Thread(target=self._loop, daemon=True)
            self.th.start()

        def _spawn(self):
            env = dict(os.environ)
            env['LD_LIBRARY_PATH'] = self.a.libpath
            if os.path.exists(self.a.loader):
                cmd = [self.a.loader, self.a.bin, self.a.model, 'x', 'x', 'x', 'inv', 'loop']
            else:
                cmd = [self.a.bin, self.a.model, 'x', 'x', 'x', 'inv', 'loop']
            # migan_rknn 是 glibc 二进制（librknnrt 也是），本系统是 musl，
            # 必须用 /opt/glibc 的 loader 启动并指定 LD_LIBRARY_PATH。
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL, env=env)

        def _loop(self):
            fsz = SZ * SZ * 3
            while True:
                item = self.qin.get()
                if item is None:
                    break
                img, msk = item
                out = b''
                try:
                    if self.proc is None:
                        self._spawn()
                    self.proc.stdin.write(img)
                    self.proc.stdin.write(msk)
                    self.proc.stdin.flush()
                    out = self.proc.stdout.read(fsz)
                except Exception:
                    self.proc = None
                    out = b''
                self.qout.put(out)

        def submit(self, img, msk):
            self.qin.put((img, msk))

        def collect(self):
            return self.qout.get()

        def close(self):
            self.qin.put(None)
            if self.proc is not None:
                try:
                    self.proc.stdin.close()
                    self.proc.wait(timeout=5)
                except Exception:
                    pass

    npu = NpuWorker(a)

    def paste(ctx, gen, hole_used):
        small = resize(gen, W, H)
        hw = resize(hole_used.astype(np.uint8)[..., None] * 255, W, H)[..., 0] > 127
        fw.write(feather_blend(ctx['frame'], small, hw, a.feather).tobytes())

    def finish(ctx, gen):
        """跑色调对齐 → 贴回，并把结果存进复用缓存。"""
        nonlocal cache
        g2, hh = match_background(gen, ctx['big'], ctx['hole'], ctx['ring'],
                                  a.flat_thr, a.noise, state=tstate, absorb=a.absorb)
        paste(ctx, g2, hh)
        cache = {'hole': hh, 'gen': g2, 'big': ctx['big']}

    def flush_one():
        ent = pending.popleft()
        if ent['kind'] == 'npu':
            outb = npu.collect()
            if len(outb) == fsz:
                finish(ent['ctx'], np.frombuffer(outb, dtype=np.uint8).reshape(SZ, SZ, 3))
            else:
                sys.stderr.write('inference failed (short read)\n')
                fw.write(ent['ctx']['frame'].tobytes())
        elif ent['kind'] == 'reuse':
            paste(ent['ctx'], ent['gen'], ent['hole'])
        elif ent['kind'] == 'flat':
            g2 = ent['ctx']['big'].copy()
            g2, hh = match_background(g2, ent['ctx']['big'], ent['ctx']['hole'],
                                      ent['ctx']['ring'], a.flat_thr, a.noise,
                                      state=tstate, absorb=a.absorb)
            paste(ent['ctx'], g2, hh)
        else:
            fw.write(ent['ctx']['frame'].tobytes())

    def push(entry):
        """入队；保持最多 2 帧在途，超出就按顺序吐出一个（保证输出帧序）。"""
        pending.append(entry)
        while len(pending) >= 2:
            flush_one()

    while True:
        buf = dec.read(W * H * 3)
        if len(buf) < W * H * 3:
            break
        frame = np.frombuffer(buf, dtype=np.uint8).reshape(H, W, 3)

        # 局部对比度检测字幕笔划（绝对亮度阈值会把亮场景元素也选中）
        # 在半分辨率上检测：box_blur 的开销是 O(N)，256² 比 512² 快 4 倍，
        # 掩码本身还要靠膨胀扩边，半分辨率完全够用。
        g = gray_of(frame)
        hsz = SZ // 2
        gs = resize(g[..., None], hsz, hsz)[..., 0]
        hole_h = text_mask_lc(gs, a.band, a.white, max(4, a.lc_radius // 2))
        hole_h = refine_rows(hole_h)
        hole = np.repeat(np.repeat(hole_h, 2, axis=0), 2, axis=1)[:SZ, :SZ]

        # 时序稳定：① 迟滞（进入 80px / 保持 25px，避免有字无字反复跳）
        #           ② 与前一阵掩码取并集（区域不逐帧伸缩）——两者合起来消除"边框抖动"
        npx = int(hole.sum())
        if npx >= 80:
            tstate['hold'] = 3
        else:
            tstate['hold'] = max(0, tstate.get('hold', 0) - 1)

        if npx < 60 and tstate.get('hold', 0) <= 0:
            tstate['hole'] = None
            push({'kind': 'none', 'ctx': {'frame': frame}})
        else:
            big = resize(frame, SZ, SZ)

            prev_hole = tstate.get('hole')
            tstate['hole'] = hole.copy()
            if prev_hole is not None:
                hole = hole | prev_hole
            if hole.sum() < 30:            # 掩码为空（迟滞刚触发），原样输出
                push({'kind': 'none', 'ctx': {'frame': frame}})
                n += 1
                continue
            hole = dilate(hole, a.dilate)
            # 白字周围有编码器的 mosquito noise（振铃），把字填掉之后那圈噪声
            # 会单独暴露成"边框外一圈颗粒"。再向外扩 margin 像素，把它一起吞掉，
            # 让羽化落在干净背景上。
            hole = dilate(hole, a.margin)
            # 掩码外围环带：用来把修补结果的整体色调拉回周边色，
            # 否则纯色底上会留下一块"污渍"（模型输出偏亮/偏暗）。
            # 只取掩码包围盒外扩一圈的范围：带外的画面内容（金币/人脸）
            # 会把平均色拉偏，导致修补区变成一块更暗的方块。
            ring = dilate(hole, a.dilate + a.ring) & ~hole
            ys, xs = np.where(hole)
            pad = a.dilate + a.ring + a.ring_pad
            by1, by2 = max(0, ys.min() - pad), min(SZ, ys.max() + pad)
            bx1, bx2 = max(0, xs.min() - pad), min(SZ, xs.max() + pad)
            keep_box = np.zeros_like(ring)
            keep_box[by1:by2, bx1:bx2] = True
            ring &= keep_box

            ctx = {'frame': frame, 'big': big, 'hole': hole, 'ring': ring}

            # 纯色/渐变底：最终用的是"局部背景估计"，模型输出会被整体丢弃，
            # 没必要白跑一次 1.15s 的 NPU 推理。
            # 加 3 个色阶的迟滞：ring_std 在阈值附近抖动时不要反复换填充方式，
            # 否则两条路径的色差会在成片里表现为"突然亮一下"（实测 9 个色阶）。
            rs = ring_std(big, ring)
            mode = tstate.get('mode', 'flat' if rs < a.flat_thr else 'npu')
            if mode == 'flat' and rs > a.flat_thr + 3.0:
                mode = 'npu'
            elif mode == 'npu' and rs < a.flat_thr - 3.0:
                mode = 'flat'
            tstate['mode'] = mode

            if mode == 'flat':
                tstate['skip'] = tstate.get('skip', 0) + 1
                push({'kind': 'flat', 'ctx': ctx})
            elif (cache is not None and not a.no_reuse
                  and (hole != cache['hole']).mean() < a.reuse_mask
                  and float(np.abs(big[int(SZ * a.band):] -
                                   cache['big'][int(SZ * a.band):]).mean()) < a.reuse_bg):
                # 逐句复用：文字遮罩没变、背景带几乎没变 → 直接贴上一帧修好的补丁。
                # 台词通常停 1~3 秒，这几帧不必重算，省推理又天然不抖。
                tstate['reuse'] = tstate.get('reuse', 0) + 1
                push({'kind': 'reuse', 'ctx': ctx, 'gen': cache['gen'],
                      'hole': cache['hole']})
            else:
                tstate['npu'] = tstate.get('npu', 0) + 1
                keep = np.where(hole, 0, 255).astype(np.uint8)
                erased = big.copy()
                erased[hole] = 127
                npu.submit(erased.tobytes(), keep.tobytes())
                push({'kind': 'npu', 'ctx': ctx})
                if a.no_thread:
                    while pending:
                        flush_one()

        n += 1
        if n % 25 == 0:
            sys.stderr.write('  %d frames\n' % n)
        if a.frames and n >= a.frames:
            break

    while pending:
        flush_one()
    npu.close()

    dec.close()
    fw.close()

    # 音轨从原片直接复制过来（-map 1:a? 表示"有音轨就带上，没有也不报错"）。
    # 视频是从处理后的裸帧重新编码的，音频不重新编码 → 原片声音原样保留。
    enc_cmd = [a.ffmpeg, '-y', '-v', 'error',
               '-f', 'rawvideo', '-pix_fmt', 'rgb24',
               '-s', '%dx%d' % (W, H), '-r', '%.6f' % fps, '-i', rawpath]
    if a.inp:
        enc_cmd += ['-i', a.inp, '-map', '0:v:0', '-map', '1:a?', '-c:a', 'copy']
    enc_cmd += ['-c:v', 'h264_rkmpp', '-pix_fmt', 'nv12', '-b:v', a.bitrate]
    # 只处理前 N 帧时，音频要比视频短，否则播放器会以为还有画面
    if a.frames and fps > 0:
        enc_cmd += ['-t', '%.3f' % (n / fps)]
    else:
        enc_cmd += ['-shortest']
    enc_cmd += [a.out]
    r = subprocess.run(enc_cmd)
    if r.returncode != 0:
        sys.stderr.write('encode failed (rc=%d)\n' % r.returncode)
    shutil.rmtree(d, ignore_errors=True)
    sys.stderr.write('NPU=%d 复用=%d 纯色跳过=%d\n' % (tstate.get('npu',0), tstate.get('reuse',0), tstate.get('skip',0)))
    sys.stderr.write('done: %d frames -> %s\n' % (n, a.out))


if __name__ == '__main__':
    main()
