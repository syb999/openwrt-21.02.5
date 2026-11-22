#!/usr/bin/env python3
"""sr-rk3566.py —— 视频画面优化（2 倍超分，Real-ESRGAN animevideov3 on RKNPU）

用法: sr-rk3566.py <输入.mp4> <输出.mp4>

自动按分辨率选模型：
    宽高比接近 4:3  → av3_res.rknn     切块 288x216
    宽高比接近 16:9 → av3_320x180.rknn 切块 320x180
源片高于 720p 直接拒绝（已经是高清，跑超分只会更慢）。
切块全部是原图裁剪，不缩放；每块 4 倍，拼完缩 2 倍得到 2x 输出。
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

import numpy as np

DIR = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(DIR, 'anime4k_rknn')
LOADER = '/opt/glibc/ld-linux-aarch64.so.1'
LIBPATH = '/opt/glibc:/opt/rknpu'
FF = '/usr/bin/ffmpeg-rkrga' if os.path.exists('/usr/bin/ffmpeg-rkrga') else 'ffmpeg'

# 进度文件（可选，第 3 个参数）。video 插件的 op_job 只认 ffmpeg 的
# -progress 格式（out_time_us / progress=end），所以这里照那个格式写。
# 以前这个参数根本不存在，sr 任务的 .prog 永远是空文件 —— 页面上进度
# 一直 0%，跑完了还显示"已停止"。
PROG = sys.argv[3] if len(sys.argv) > 3 else ''


def progress(sec, done=False):
    if not PROG:
        return
    try:
        with open(PROG, 'w') as f:
            f.write('out_time_us=%d\n' % int(max(0.0, sec) * 1000000))
            f.write('progress=%s\n' % ('end' if done else 'continue'))
    except Exception:
        pass

# 块边缘丢弃宽度（源像素）。
# 模型在块边界做 zero-padding，边缘 M 像素的输出是坏的（偏暗/偏亮/发虚）。
# 每块只保留中心区域，边缘交给相邻块的中心来补 → 接缝两边都是"好像素"。
# M 越大越安全但块数越多；16 对 288x216 的块够用。SR_MARGIN=24 可再稳一点。
TILE_MARGIN = int(os.environ.get('SR_MARGIN', '32'))

MODELS = [
    # 大块模型（优先）：源片 ≤ 模型尺寸时只有 1 块 —— 零接缝、实推面积最小最快。
    #   576x432  ← 03.mp4 那类 4:3 SD（576x432）正好一块
    ('4:3', 4.0 / 3.0, os.path.join(DIR, 'av3_640x480.rknn'), 640, 480),
    ('4:3', 4.0 / 3.0, os.path.join(DIR, 'av3_576x432.rknn'), 576, 432),
    ('16:9', 16.0 / 9.0, os.path.join(DIR, 'av3_640x360.rknn'), 640, 360),
    # 小块模型（兜底）：源片比大模型还大时才用，靠切块覆盖。
    ('4:3', 4.0 / 3.0, os.path.join(DIR, 'av3_res.rknn'), 288, 216),
    ('16:9', 16.0 / 9.0, os.path.join(DIR, 'av3_320x180.rknn'), 320, 180),
]


def die(msg):
    sys.stderr.write('error: %s\n' % msg)
    sys.exit(1)


def probe(path):
    r = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                        '-show_entries', 'stream=width,height,r_frame_rate,sample_aspect_ratio,display_aspect_ratio',
                        '-of', 'json', path], capture_output=True, text=True)
    try:
        st = json.loads(r.stdout)['streams'][0]
    except Exception:
        die('ffprobe 失败: %s' % path)
    w, h = int(st['width']), int(st['height'])
    num, den = st.get('r_frame_rate', '25/1').split('/')
    fps = float(num) / float(den or 1)
    if fps < 1 or fps > 240:
        fps = 25.0
    dar = 0.0
    d = st.get('display_aspect_ratio', '') or ''
    if ':' in d:
        a, b = d.split(':')
        try:
            dar = float(a) / float(b)
        except Exception:
            dar = 0.0
    if dar <= 0:
        sar = st.get('sample_aspect_ratio', '') or ''
        if ':' in sar and sar != '0:1':
            a, b = sar.split(':')
            try:
                dar = w / float(h) * float(a) / float(b)
            except Exception:
                dar = 0.0
    if dar <= 0:
        dar = w / float(h)
    return w, h, fps, dar


def pick_model(w, h):
    ar = w / float(h)
    best = None
    for name, tar, path, tw, th in MODELS:
        nx = (w + tw - 1) // tw
        ny = (h + th - 1) // th
        waste = (nx * tw * ny * th - w * h) / float(w * h)
        score = abs(ar - tar) * 2.0 + waste
        if best is None or score < best[0]:
            best = (score, name, path, tw, th, nx, ny, waste)
    return best


def main():
    if len(sys.argv) < 3:
        die('用法: sr-rk3566.py <输入.mp4> <输出.mp4>')
    src, dst = sys.argv[1], sys.argv[2]
    if not os.path.exists(src):
        die('文件不存在: %s' % src)

    w, h, fps, dar = probe(src)
    if max(w, h) > 1280 and min(w, h) > 720:
        die('源片 %dx%d 已经是高清，不需要跑超分' % (w, h))
    score, mname, model, tw, th, nx, ny, waste = pick_model(w, h)
    if not os.path.exists(model):
        die('模型缺失: %s' % model)
    sys.stderr.write('源 %dx%d %.3ffps DAR=%.3f → 模型 %s（块 %dx%d，%dx%d=%d 块，补齐 %.1f%%）\n'
                     % (w, h, fps, dar, mname, tw, th, nx, ny, nx * ny, waste * 100))

    tmp = tempfile.mkdtemp(prefix='sr_', dir='/opt/llama/out' if os.path.isdir('/opt/llama/out') else '/tmp')
    raw_in = os.path.join(tmp, 'in.rgb')
    raw_out = os.path.join(tmp, 'out.rgb')
    env = dict(os.environ, LD_LIBRARY_PATH=LIBPATH)
    fsz = w * h * 3
    n = 0                      # 提前定义：解码失败时 finally 里的进度也要能算

    try:
        sys.stderr.write('解码...\n')
        progress(0)
        # 解码这一大段（约占总时间三分之一）也要有进度。
        # ffmpeg 的 -progress 文件在收尾时会写 progress=end，直接拿给插件会被
        # 当成"任务完成"（推理还没开始），所以解码写到旁路文件，由线程把
        # out_time_us 转发到真正的进度文件，不转发那个 end。
        dec_prog = (PROG + '.dec') if PROG else '/dev/null'
        stop_poll = threading.Event()

        def mirror():
            while not stop_poll.is_set():
                try:
                    with open(dec_prog) as df:
                        for ln in df:
                            if ln.startswith('out_time_us='):
                                progress(int(ln.split('=')[1]) / 1000000.0)
                                break
                except Exception:
                    pass
                stop_poll.wait(1.0)

        pth = threading.Thread(target=mirror, daemon=True)
        if PROG:
            pth.start()
        try:
            subprocess.run([FF, '-y', '-v', 'error', '-nostats', '-progress', dec_prog,
                            '-i', src, '-f', 'rawvideo',
                            '-pix_fmt', 'rgb24', raw_in], check=True)
        finally:
            stop_poll.set()

        tw_out, th_out = w * 2, h * 2
        # SR_MODE=single：整帧一次推理，绝对没有块边界 → 绝对没有接缝。
        #   做法：把整帧按面积平均降到模型尺寸（288x216 / 320x180），跑一次 4 倍，
        #   输出正好是源片的 2 倍。代价是源片 1:1 细节先被降采样（整体略软），
        #   但换来彻底无缝，而且只要 1 次推理（比切块快 4~9 倍）。
        # SR_MODE=tile（默认）：切块保原图细节，块边界丢弃 M 像素交给邻块中心。
        single = os.environ.get('SR_MODE', '').lower() in ('1', 'single', 'one')
        if single:
            xs, ys, M = [0], [0], 0
            sys.stderr.write('单块模式（整帧一次推理，无接缝）\n')
        else:
            # 块边缘 M 个源像素的模型输出是坏的（模型在块边界做 zero-padding，
            # 感受野被截断 → 偏暗/偏亮/发虚）。所以每块只保留【中心】部分，
            # 边缘直接丢弃，由相邻块的中心来补 —— Real-ESRGAN 的标准做法。
            M = TILE_MARGIN
            sx = max(1, tw - 2 * M)
            sy = max(1, th - 2 * M)
            xs = list(range(0, max(1, w - tw + 1), sx))
            ys = list(range(0, max(1, h - th + 1), sy))
            if xs[-1] + tw < w:
                xs.append(max(0, w - tw))
            if ys[-1] + th < h:
                ys.append(max(0, h - th))
            sys.stderr.write('切块 %dx%d 块（丢弃边缘 %d px）\n' % (len(xs), len(ys), M))

        def fit_tile(img):
            """整帧 → th×tw（单块模式的输入）：整数比用面积平均，否则最近邻。"""
            hh, ww = img.shape[:2]
            f = img.astype(np.float32)
            if hh % th == 0 and ww % tw == 0:
                ry, rx = hh // th, ww // tw
                return f.reshape(th, ry, tw, rx, 3).mean(axis=(1, 3))
            yi = np.arange(th) * hh // th
            xi = np.arange(tw) * ww // tw
            return f[yi][:, xi]

        def fit_out(o4):
            """模型输出 → (th_out, tw_out)（单块模式：4x 输出约等于 2x 源）。"""
            H, W = o4.shape[:2]
            if H == th_out and W == tw_out:
                return o4.astype(np.float32)
            yi = np.arange(th_out) * H // th_out
            xi = np.arange(tw_out) * W // tw_out
            return o4[yi][:, xi].astype(np.float32)

        fin = open(raw_in, 'rb')
        fout = open(raw_out, 'wb')
        n = 0
        while True:
            buf = fin.read(fsz)
            if len(buf) < fsz:
                break
            img = np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 3)
            out = np.zeros((th_out, tw_out, 3), np.float32)
            if single:
                tile = fit_tile(img)
                np.ascontiguousarray(tile.astype(np.uint8)).tofile(os.path.join(tmp, 't.rgb'))
                r = subprocess.run([LOADER, BIN, model, os.path.join(tmp, 't.rgb'),
                                    os.path.join(tmp, 'o.rgb'), str(tw), str(th)],
                                   env=env, capture_output=True)
                if r.returncode != 0:
                    die('推理失败：%s' % r.stderr[-200:])
                o4 = np.fromfile(os.path.join(tmp, 'o.rgb'), dtype=np.uint8).reshape(th * 4, tw * 4, 3)
                out = fit_out(o4) + 0.5
                fout.write(np.clip(out, 0, 255).astype(np.uint8).tobytes())
                n += 1
                progress(n / fps)
                if n % 25 == 0:
                    sys.stderr.write('  %d frames\n' % n)
                continue
            for y0 in ys:
                for x0 in xs:
                    tile = img[y0:y0 + th, x0:x0 + tw]
                    th_, tw_ = tile.shape[0], tile.shape[1]
                    if th_ < th or tw_ < tw:
                        tile = np.pad(tile, ((0, th - th_), (0, tw - tw_), (0, 0)),
                                      mode='edge')
                    np.ascontiguousarray(tile).tofile(os.path.join(tmp, 't.rgb'))
                    r = subprocess.run([LOADER, BIN, model, os.path.join(tmp, 't.rgb'),
                                        os.path.join(tmp, 'o.rgb'), str(tw), str(th)],
                                       env=env, capture_output=True)
                    if r.returncode != 0:
                        die('推理失败（块 %d,%d）：%s' % (y0, x0, r.stderr[-200:]))
                    o = np.fromfile(os.path.join(tmp, 'o.rgb'), dtype=np.uint8).reshape(th * 4, tw * 4, 3)
                    half = o.reshape(th * 2, 2, tw * 2, 2, 3).mean(axis=(1, 3))
                    # 只取"中心"部分：离块的边界 >= M（贴画面外沿的那侧除外）
                    cx0 = 0 if x0 == 0 else M
                    cx1 = min(tw_, tw - M) if x0 + tw < w else tw_
                    cy0 = 0 if y0 == 0 else M
                    cy1 = min(th_, th - M) if y0 + th < h else th_
                    if cx1 <= cx0 or cy1 <= cy0:
                        continue
                    out[(y0 + cy0) * 2:(y0 + cy1) * 2,
                        (x0 + cx0) * 2:(x0 + cx1) * 2] = \
                        half[cy0 * 2:cy1 * 2, cx0 * 2:cx1 * 2] + 0.5
            fout.write(np.clip(out, 0, 255).astype(np.uint8).tobytes())
            n += 1
            progress(n / fps)
            if n % 25 == 0:
                sys.stderr.write('  %d frames\n' % n)
        fin.close()
        fout.close()
        if n == 0:
            die('没读到帧')
        progress(n / fps)

        sys.stderr.write('编码...\n')
        # 音频从原片流复制；-aspect 必须写，否则 SAR 为空、播放器按 16:9 拉伸
        subprocess.run([FF, '-y', '-v', 'error',
                        '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                        '-s', '%dx%d' % (tw_out, th_out), '-r', '%.6f' % fps,
                        '-i', raw_out, '-i', src,
                        '-map', '0:v:0', '-map', '1:a?',
                        '-aspect', ('%d:%d' % (round(dar * 1000), 1000)),
                        '-c:v', 'h264_rkmpp', '-pix_fmt', 'nv12', '-b:v', '20M',
                        '-c:a', 'copy', '-shortest', dst], check=True)
    finally:
        # 失败时不要写 progress=end —— 那会让插件把崩溃当成"完成 100%"。
        # 正常结束的 end 在编码之后单独写。
        shutil.rmtree(tmp, ignore_errors=True)

    progress(n / fps, done=True)
    sz = os.path.getsize(dst) if os.path.exists(dst) else 0
    sys.stderr.write('done: %d frames -> %s (%.1f MB, %dx%d, 模型 %s)\n'
                     % (n, dst, sz / 1048576.0, tw_out, th_out, mname))


main()
