#!/usr/bin/env python3
# ppocr 放大测试 + 与 tesseract 组合
import glob
import os
import re
import subprocess

from PIL import Image

os.environ["TESSDATA_PREFIX"] = "/usr/share/tessdata"
WL = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"


def ppocr(path):
    try:
        r = subprocess.run(["ppocr", path], capture_output=True, text=True, timeout=30)
        return re.sub(r"[^0-9A-Za-z]", "", (r.stdout or "").strip()).lower()
    except Exception:
        return ""


def tess(path, psm="7"):
    try:
        r = subprocess.run(["tesseract", path, "stdout", "-l", "eng", "--psm", psm,
                            "-c", "tessedit_char_whitelist=" + WL],
                           capture_output=True, text=True, timeout=15)
        return re.sub(r"[^0-9A-Za-z]", "", r.stdout or "").lower()
    except Exception:
        return ""


def upscale(src, scale, out):
    im = Image.open(src).convert("RGB")
    im = im.resize((im.width * scale, im.height * scale), Image.LANCZOS)
    im.save(out)
    return out


def pick(t, p):
    """双引擎选一个：4 字符优先；都 4 字符时信 tesseract（干净图上更准）；
    只有一个 4 字符就用它；都不是就看谁更接近 4 位"""
    if len(t) == 4 and len(p) == 4:
        return t
    if len(t) == 4:
        return t
    if len(p) == 4:
        return p
    return t if abs(len(t) - 4) <= abs(len(p) - 4) else p


rows = []
for level, tag in ((0, "干净"), (1, "轻微"), (2, "强化")):
    files = sorted(glob.glob("/tmp/captcha/L%d_*.png" % level))
    stat = {"ppocr": 0, "ppocr2x": 0, "ppocr3x": 0, "tess": 0, "combo": 0}
    n = 0
    for f in files:
        ans = re.search(r"_([0-9a-z]{4})\.png$", f).group(1)
        n += 1
        p1 = ppocr(f)
        p2 = ppocr(upscale(f, 2, "/tmp/_u2.png"))
        p3 = ppocr(upscale(f, 3, "/tmp/_u3.png"))
        t1 = tess(f)
        c = pick(t1, p1)
        for k, v in (("ppocr", p1), ("ppocr2x", p2), ("ppocr3x", p3),
                     ("tess", t1), ("combo", c)):
            if v == ans:
                stat[k] += 1
    rows.append((tag, n, stat))
    print("%-6s (n=%d)  ppocr=%d  x2=%d  x3=%d  tess=%d  组合=%d" % (
        tag, n, stat["ppocr"], stat["ppocr2x"], stat["ppocr3x"],
        stat["tess"], stat["combo"]))

print()
tot = {k: sum(r[2][k] for r in rows) for k in rows[0][2]}
n_all = sum(r[1] for r in rows)
print("合计 %d 张：" % n_all)
for k, v in tot.items():
    print("  %-8s %2d/%d = %3.0f%%" % (k, v, n_all, 100.0 * v / n_all))
