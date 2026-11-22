#!/usr/bin/env python3
# 生成验证码测试图 —— 关键要求：字符必须完整落在画布内，底部/右侧不能有被切断的笔画
import os
import random

from PIL import Image, ImageDraw, ImageFont

OUT = "/tmp/captcha"
os.makedirs(OUT, exist_ok=True)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
CHARS = "23456789abcdefghjkmnpqrstuvwxyz"
W, H = 150, 52          # 比之前大一圈，给旋转和抖动留余量
MARGIN = 6              # 上下左右至少留这么多白边


def make(text, seed, level):
    rnd = random.Random(seed)
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)

    if level >= 2:
        for _ in range(rnd.randint(2, 4)):
            d.line([(rnd.randint(0, W), rnd.randint(0, H)),
                    (rnd.randint(0, W), rnd.randint(0, H))],
                   fill=(rnd.randint(120, 200),) * 3, width=1)
        for _ in range(rnd.randint(90, 160)):
            d.point((rnd.randint(0, W - 1), rnd.randint(0, H - 1)),
                    fill=(rnd.randint(0, 160),) * 3)

    x = MARGIN + 2
    for ch in text:
        size = rnd.randint(28, 34)
        f = ImageFont.truetype(FONT, size)

        # 先把字画在大画布上，按实际墨迹裁边，再旋转 → 不会带进空白导致算错尺寸
        pad = size
        g = Image.new("RGBA", (size * 2 + pad, size * 2 + pad), (0, 0, 0, 0))
        ImageDraw.Draw(g).text((pad // 2, pad // 2), ch, font=f,
                               fill=(rnd.randint(0, 70), rnd.randint(0, 70),
                                     rnd.randint(0, 70), 255))
        bbox = g.getbbox()
        if bbox:
            g = g.crop(bbox)
        if level >= 1:
            ang = rnd.randint(-18, 18) if level >= 2 else rnd.randint(-6, 6)
            g = g.rotate(ang, expand=1, resample=Image.BICUBIC)

        # 缩放保证高度 ≤ H - 2*MARGIN
        max_h = H - 2 * MARGIN
        if g.height > max_h:
            r = max_h / float(g.height)
            g = g.resize((max(1, int(g.width * r)), max_h), Image.LANCZOS)

        # 横向放不下就停（保证不切到右边）
        if x + g.width > W - MARGIN:
            break

        # 纵向随机位置，但严格保证整块都在画布内
        lo = MARGIN
        hi = H - MARGIN - g.height
        y = rnd.randint(lo, hi) if hi >= lo else lo
        img.paste(g, (x, y), g)
        x += g.width + rnd.randint(3, 6)

    return img


def cut_check(im):
    """检查四条边向内 2px 内有没有深色像素（有就说明被切了）"""
    px = im.convert("L").load()
    w, h = im.size
    edges = []
    for y in range(2):
        edges.append(sum(1 for x in range(w) if px[x, y] < 128))
        edges.append(sum(1 for x in range(w) if px[x, h - 1 - y] < 128))
    for x in range(2):
        edges.append(sum(1 for y in range(h) if px[x, y] < 128))
        edges.append(sum(1 for y in range(h) if px[w - 1 - x, y] < 128))
    return sum(edges)


bad = 0
rows = []
for level, tag in ((0, "干净"), (1, "轻微"), (2, "强化")):
    for i in range(10):
        rnd = random.Random(level * 100 + i * 7 + 3)
        txt = "".join(rnd.choice(CHARS) for _ in range(4))
        im = make(txt, level * 100 + i, level)
        edge = cut_check(im)
        if edge:
            bad += 1
        p = os.path.join(OUT, "L%d_%02d_%s.png" % (level, i, txt))
        im.save(p)
        rows.append((os.path.basename(p), txt, tag, edge))

with open(os.path.join(OUT, "answers.txt"), "w") as f:
    for name, txt, tag, edge in rows:
        f.write("%s\t%s\t%s\n" % (name, txt, tag))

print("生成 %d 张（每档 10 张）→ %s" % (len(rows), OUT))
print("贴边被切的: %d 张 %s" % (bad, "（0 才合格）" if bad == 0 else "← 还有问题"))
for name, txt, tag, edge in rows:
    if edge:
        print("   异常: %s 边缘深色像素 %d" % (name, edge))
