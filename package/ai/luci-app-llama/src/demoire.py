#!/usr/bin/env python3
"""去摩尔纹（自适应频谱陷波）—— 比"降采样+中值"真正有效

原理：摩尔纹在频域是几个**孤立的高亮峰**（拍频产物）。模糊/中值只能压高频噪声，
对低频摩尔纹条带无效；正确做法是找到这些峰，把它们（连同对称点）挖掉再反变换。

用法： demoire.py <输入图片> <输出图片> [强度1-3] [最多挖几个峰]
"""
import os
import sys
import numpy as np
from PIL import Image


def notch_channel(ch, strength=2, max_peaks=16):
    h, w = ch.shape
    # 补到 2 的幂会更快，但 numpy 的 FFT 对任意尺寸也能算，直接算保尺寸
    F = np.fft.fftshift(np.fft.fft2(ch.astype(np.float32))).astype(np.complex64)
    mag = np.abs(F)

    # 只用中心区域之外的峰：摩尔纹频率一般在中高频，DC 和极低频是画面内容
    cy, cx = h // 2, w // 2
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.sqrt(((yy - cy) / (h / 2.0)) ** 2 + ((xx - cx) / (w / 2.0)) ** 2)
    # 实拍屏幕照片的摩尔纹常常落在**低**频（周期 60~130px），
    # 以前从 r>0.06 才开始找，正好把摩尔纹排除在外 → 看起来"完全没用"。
    # 现在只排除 DC 附近一小圈（拍照整体的明暗渐变），其余全找。
    valid = (r > 0.015) & (r < 0.85)

    # 只排除 DC 附近的小菱形；不要把整条坐标轴排除掉 ——
    # 实测屏幕摩尔纹的峰常常正好在轴上（例如 dy=0, dx=-9），
    # 以前的 (>=4, >=4) 守卫把它全挡掉了，等于什么都没做。
    axis_guard = (np.abs(xx - cx) + np.abs(yy - cy)) >= 6
    m = np.where(valid & axis_guard, mag, 0)
    thr = m.mean() + (1.0 + 0.6 * strength) * m.std()
    mask = np.ones_like(mag, dtype=np.float32)

    peaks = []
    work = m.copy()
    for _ in range(max_peaks):
        idx = int(np.argmax(work))
        py, px = divmod(idx, w)
        if work[py, px] < thr:
            break
        peaks.append((py, px, float(work[py, px])))
        yy0, yy1 = max(0, py - 8), min(h, py + 9)
        xx0, xx1 = max(0, px - 8), min(w, px + 9)
        work[yy0:yy1, xx0:xx1] = 0           # 抑制邻域，避免同一峰重复计

    if not peaks:
        return ch.astype(np.uint8), 0

    # 挖掉峰（半径随强度增大），并用周围环带的均值填补，避免留下黑洞
    rad = {1: 3, 2: 5, 3: 8}.get(strength, 5)
    Forig = F.copy()
    for (py, px, _v) in peaks:
        rr = np.sqrt(((py - cy) / (h / 2.0)) ** 2 + ((px - cx) / (w / 2.0)) ** 2)
        rad_p = rad if rr > 0.05 else max(rad, 8)    # 低频峰略宽，但别碰到 DC
        y0, y1 = max(0, py - rad_p), min(h, py + rad_p + 1)
        x0, x1 = max(0, px - rad_p), min(w, px + rad_p + 1)
        yy2, xx2 = np.mgrid[y0:y1, x0:x1]
        # 高斯锥形陷波：硬挖盘会在边缘留下振铃（实测会凭空多出一个 dx=±35 的假峰），
        # 用平滑衰减代替硬切，既压掉峰又不产生新的人工纹理。
        disk = ((yy2 - py) ** 2 + (xx2 - px) ** 2) <= rad_p * rad_p   # 硬挖：实测压得最狠
        # 环带均值（同一峰周围，半径 rad+3~rad+8）
        ring = ((yy2 - py) ** 2 + (xx2 - px) ** 2 > rad_p * rad_p) & \
               ((yy2 - py) ** 2 + (xx2 - px) ** 2 <= (rad_p + 10) ** 2)
        # 陷波用"按比例衰减"而不是清零/填常数：
        #   清零会连同低频结构一起挖掉（整幅变黑）；
        #   填环带均值会注入一个强低频成分（也变黑）。
        #   保留相位、只压低幅度，画面整体亮度结构不受影响。
        att = 0.05 if strength >= 2 else 0.2
        F[y0:y1, x0:x1][disk] *= att

    out = np.real(np.fft.ifft2(np.fft.ifftshift(F)))
    del F
    return np.clip(out, 0, 255).astype(np.uint8), len(peaks)




def notch_image_blockwise(a, strength=2, max_peaks=16, tile=384, overlap=96):
    """分块做频谱陷波 + Hann 窗融合。
    屏幕/翻拍的摩尔纹频率随位置漂移，整幅一个全局掩码只能压掉"平均峰"；
    分块后每块各自找峰，才能真正跟着条纹走。"""
    h, w, c = a.shape
    acc = np.zeros((h, w, c), np.float32)
    wsum = np.zeros((h, w, 1), np.float32)
    step = max(32, tile - overlap)
    win = np.outer(np.hanning(tile), np.hanning(tile)).astype(np.float32)[:, :, None]
    for y0 in range(0, max(1, h - overlap), step):
        for x0 in range(0, max(1, w - overlap), step):
            y1 = min(h, y0 + tile); x1 = min(w, x0 + tile)
            ty, tx = y1 - y0, x1 - x0
            if ty < 64 or tx < 64:
                continue
            block = a[y0:y1, x0:x1]
            outb = np.zeros_like(block)
            for ch in range(3):
                outb[:, :, ch], _ = notch_channel(block[:, :, ch], strength, max_peaks)
            wgt = win[:ty, :tx]
            acc[y0:y1, x0:x1] += outb.astype(np.float32) * wgt
            wsum[y0:y1, x0:x1] += wgt
    safe = np.maximum(wsum, 1e-6)
    blended = acc / safe                     # 先整体归一化，别用布尔索引再广播（会炸内存）
    m = (wsum[..., 0] > 1e-3)[..., None]
    res = np.where(m, blended, a.astype(np.float32))
    return np.clip(res, 0, 255).astype(np.uint8)


def smooth_chroma(img, radius=2.5):
    """屏幕摩尔纹的另一半是**彩色**条纹（彩虹带），来自子像素的拍频。
    做法：转到 YCbCr，只把色度（Cb/Cr）大力模糊，亮度保持不动 ——
    文字/格线在亮度里，不会被糊掉，而彩虹带会消失。"""
    if not hasattr(img, "convert"):
        img = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    y, cb, cr = img.convert("YCbCr").split()
    from PIL import ImageFilter
    cb = cb.filter(ImageFilter.GaussianBlur(radius))
    cr = cr.filter(ImageFilter.GaussianBlur(radius))
    return Image.merge("YCbCr", (y, cb, cr)).convert("RGB")

def main():
    if len(sys.argv) < 3:
        print("用法: demoire.py <in> <out> [strength 1-3] [max_peaks]")
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    strength = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    max_peaks = int(sys.argv[4]) if len(sys.argv) > 4 else 16

    im = Image.open(src).convert("RGB")
    # 内存保护：3B 模型常驻要吃掉 2.2GB，剩给我们的不多。
    # 大图先降到长边 1400 再算 FFT（摩尔纹是低频，降采样不影响），省 4 倍内存。
    long_side = max(im.size)
    if long_side > 1400:
        r = 1400.0 / long_side
        small = im.resize((max(2, int(im.size[0] * r)), max(2, int(im.size[1] * r))), Image.LANCZOS)
        a = np.asarray(small)
        scale_back = True
    else:
        a = np.asarray(im)
        scale_back = False
    if strength >= 3:
        import subprocess as sp
        tmp = dst + ".rs.png"
        vf = ("scale=trunc(iw*0.70/2)*2:trunc(ih*0.70/2)*2:flags=area,"
              "scale=iw/0.70:ih/0.70:flags=lanczos,unsharp=5:5:0.7")
        r = sp.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-vf", vf, tmp],
                   capture_output=True, text=True)
        if r.returncode == 0:
            import shutil as sh
            sh.move(tmp, dst)
            print("peaks_notched=resample")
            return 0
        print("resample 失败，退回频谱陷波")

    if os.environ.get("DEMOIRE_BLOCK", "0") == "1":
        res = notch_image_blockwise(a, strength, max_peaks)
        total = -1                       # 分块模式不统计总峰数
    else:
        res = np.zeros_like(a)
        total = 0
        for c in range(3):
            out, n = notch_channel(a[:, :, c], strength, max_peaks)
            res[:, :, c] = out
            total += n
    if scale_back:
        res = np.asarray(Image.fromarray(res).resize(im.size, Image.LANCZOS))
    Image.fromarray(res).save(dst)
    print("peaks_notched=%d" % total)
    out = smooth_chroma(out, radius=2.0 + strength)
    return 0


if __name__ == "__main__":
    sys.exit(main())
