#!/usr/bin/env python3
# 生成验证码图片 + 两个带验证码的登录页
import os
import random

from PIL import Image, ImageDraw, ImageFont

OUT = "/tmp/captchatest"
os.makedirs(OUT, exist_ok=True)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
CHARS = "23456789abcdefghjkmnpqrstuvwxyz"
W, H = 150, 52          # 跟 gen_captcha.py 一致：图大一圈，字符不会被切
MARGIN = 6


def make(text, seed, level):
    """字符必须完整落在画布内 —— 之前 H=40 时字形高 40~46，paste 到 y=2~8
    直接把下半截画到图外，截出来的验证码是残的，OCR 当然认不出。"""
    rnd = random.Random(seed)
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    if level >= 2:
        for _ in range(rnd.randint(3, 4)):
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
        max_h = H - 2 * MARGIN
        if g.height > max_h:
            r = max_h / float(g.height)
            g = g.resize((max(1, int(g.width * r)), max_h), Image.LANCZOS)
        if x + g.width > W - MARGIN:
            break
        lo, hi = MARGIN, H - MARGIN - g.height
        y = rnd.randint(lo, hi) if hi >= lo else lo
        img.paste(g, (x, y), g)
        x += g.width + rnd.randint(3, 6)
    return img


CASES = [("cap_clean.png", "9mk6", 0, "p8", "干净验证码（tesseract 应能识别）"),
         ("cap_hard.png", "w7zt", 2, "p9", "干扰线+旋转（大概率识别失败）")]

PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>%s</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#eef1f6;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#fff;padding:30px 28px;border-radius:12px;width:340px;
      box-shadow:0 8px 30px rgba(0,0,0,.12)}
h2{margin:0 0 18px;font-size:19px}
input{width:100%%;box-sizing:border-box;padding:10px;margin-bottom:12px;
      border:1px solid #d6dae1;border-radius:7px;font-size:14px}
.row{display:flex;gap:10px;align-items:center;margin-bottom:12px}
.row input{margin:0;flex:1}
.row img{height:40px;border-radius:5px;cursor:pointer;border:1px solid #e2e5ea}
button{width:100%%;padding:11px;border:0;border-radius:7px;background:#2563eb;
       color:#fff;font-size:15px;cursor:pointer}
.msg{margin-top:10px;font-size:13px;color:#e33}
.ok{color:#2a7}
</style></head>
<body>
<div class="card">
  <h2>%s</h2>
  <input id="u" placeholder="用户名">
  <input id="p" type="password" placeholder="密码">
  <div class="row">
    <input id="c" name="captcha" placeholder="验证码">
    <img src="%s" id="capimg" alt="验证码" onclick="this.src='%s?'+Date.now()">
  </div>
  <button onclick="go()">登 录</button>
  <div class="msg" id="m"></div>
</div>
<script>
const CAP = %s;
function go() {
  var u = document.getElementById('u').value;
  var p = document.getElementById('p').value;
  var c = document.getElementById('c').value.trim().toLowerCase();
  var m = document.getElementById('m');
  if (!u || !p) { m.textContent = '请填账号密码'; return; }
  if (c !== CAP) { m.textContent = '验证码错误（应为 ' + CAP + '）'; return; }
  var tok = 'ct_' + Math.random().toString(36).slice(2, 10);
  localStorage.setItem('token', tok);
  document.cookie = '%s_sid=' + tok + ';path=/';
  m.className = 'msg ok';
  m.textContent = '登录成功';
  document.title = '登录成功';
}
</script>
</body></html>"""

for fname, text, level, page, title in CASES:
    img = make(text, len(text) * 13, level)
    img.save(os.path.join(OUT, fname))
    html = PAGE % (title, title, fname, fname, repr(text), page)
    with open(os.path.join(OUT, page + ".html"), "w", encoding="utf-8") as f:
        f.write(html)
    print("  %s  →  %s.html  答案=%r" % (fname, page, text))

print("共 %d 个文件 → %s" % (len(os.listdir(OUT)), OUT))
