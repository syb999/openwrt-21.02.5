#!/usr/bin/env python3
# 造一个"拼图滑块"验证码页（京东/极验那种）：背景图挖个洞，左边一块拼图，
# 拖动滑块把拼图送到洞口才算通过。用来验证插件的拖拽能力。
import os
import random

from PIL import Image, ImageDraw, ImageFilter

OUT = "/tmp/slidertest"
os.makedirs(OUT, exist_ok=True)

W, H = 380, 190
PIECE = 46          # 拼图块边长
GAP_X = 232         # 缺口左边缘 x（固定，方便断言）


def make_bg(seed):
    """造一张"照片感"的图：渐变底 + 几块彩色圆 + 噪点"""
    rnd = random.Random(seed)
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    # 渐变
    for y in range(H):
        c = (int(30 + y * 0.5), int(50 + y * 0.3), int(80 + y * 0.6))
        d.line([(0, y), (W, y)], fill=c)
    # 随机色块
    for _ in range(14):
        x, y = rnd.randint(-30, W), rnd.randint(-30, H)
        r = rnd.randint(20, 70)
        col = (rnd.randint(30, 220), rnd.randint(30, 220), rnd.randint(30, 220))
        d.ellipse([x, y, x + r * 2, y + r], fill=col)
    # 噪点
    px = im.load()
    for _ in range(4000):
        x, y = rnd.randint(0, W - 1), rnd.randint(0, H - 1)
        px[x, y] = (rnd.randint(0, 255),) * 3
    return im.filter(ImageFilter.GaussianBlur(0.6))


GAP_Y = 72

# ---- 底图：挖洞 ----
bg = make_bg(2026)
mask = Image.new("L", (W, H), 0)
md = ImageDraw.Draw(mask)
# 洞做成拼图形状（带凸起），更接近真实
md.rectangle([GAP_X, GAP_Y, GAP_X + PIECE, GAP_Y + PIECE], fill=255)
md.ellipse([GAP_X + PIECE - 10, GAP_Y + PIECE // 2 - 9,
            GAP_X + PIECE + 10, GAP_Y + PIECE // 2 + 9], fill=255)

hole = Image.new("RGB", (W, H), (255, 255, 255))
dark = Image.new("RGB", (W, H), (20, 26, 34))
bg_cut = Image.composite(dark, bg, mask)                 # 洞里压暗
outline = mask.filter(ImageFilter.FIND_EDGES)
bg_cut = Image.composite(Image.new("RGB", (W, H), (255, 255, 255)), bg_cut, outline)

bg_cut.save(os.path.join(OUT, "bg.png"))

# ---- 拼图块：从原图抠出来，带白边 ----
piece = Image.new("RGBA", (PIECE + 24, PIECE + 24), (0, 0, 0, 0))
crop = bg.crop((GAP_X, GAP_Y, GAP_X + PIECE, GAP_Y + PIECE)).convert("RGBA")
pmask = mask.crop((GAP_X, GAP_Y, GAP_X + PIECE, GAP_Y + PIECE))
piece.paste(crop, (8, 8), pmask)
pd = ImageDraw.Draw(piece)
pd.rectangle([8, 8, 8 + PIECE, 8 + PIECE], outline=(255, 255, 255, 230), width=2)
piece.save(os.path.join(OUT, "piece.png"))

HTML = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>拼图滑块验证</title>
<style>
body{margin:0;background:#eef1f6;font-family:system-ui,sans-serif;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#fff;padding:26px;border-radius:12px;box-shadow:0 8px 30px rgba(0,0,0,.12)}
h3{margin:0 0 14px;font-size:16px;color:#1b2733}
.stage{position:relative;width:%(W)dpx;height:%(H)dpx;border-radius:8px;overflow:hidden;
       user-select:none}
.stage img.bg{width:100%%;height:100%%;display:block}
.piece{position:absolute;left:8px;top:%(GAPY)dpx;width:%(PW)dpx;height:%(PH)dpx;
       cursor:grab;z-index:3;pointer-events:none}
.track{position:relative;margin-top:14px;height:38px;background:#f2f5f9;
       border:1px solid #dde3ea;border-radius:19px}
.fillbar{position:absolute;left:0;top:0;height:100%%;width:0;background:#c9e4ff;
         border-radius:19px}
.thumb{position:absolute;left:0;top:0;width:46px;height:38px;background:#fff;
       border:1px solid #c6ced8;border-radius:19px;cursor:grab;
       display:flex;align-items:center;justify-content:center;font-size:16px;
       color:#5b6b7c;z-index:2}
.thumb:active{cursor:grabbing}
.tip{margin-top:10px;font-size:12.5px;color:#8b98a5;height:16px}
.ok{color:#1a9c62}
</style></head>
<body><div class="card">
<h3>安全验证</h3>
<div class="stage" id="stage">
  <img class="bg" src="bg.png" alt="">
  <img class="piece" id="pc" src="piece.png" alt="">
</div>
<div class="track" id="track">
  <div class="fillbar" id="fill"></div>
  <div class="thumb" id="thumb">&rarr;</div>
</div>
<div class="tip" id="tip">拖动滑块完成拼图</div>
</div>
<script>
var GAP = %(GAPX)d, PIECE = %(PIECE)d, START_L = 8;
var thumb=document.getElementById('thumb'), pc=document.getElementById('pc');
var fill=document.getElementById('fill'), tip=document.getElementById('tip');
var drag=false, startX=0, cur=0;

function setPos(x){
  cur = Math.max(0, Math.min(x, %(W)d - 54));
  thumb.style.left = cur + 'px';
  fill.style.width = (cur + 23) + 'px';
  /* 拼图块跟着滑块走：滑块从 0 动到 W-54，拼图从 8 动到 8+(W-54) */
  pc.style.left = (START_L + cur) + 'px';
}
thumb.addEventListener('mousedown', function(e){ drag=true; startX=e.clientX-cur; e.preventDefault(); });
document.addEventListener('mousemove', function(e){
  if(!drag) return;
  setPos(e.clientX - startX);
});
document.addEventListener('mouseup', function(){
  if(!drag) return;
  drag=false;
  var landed = START_L + cur;
  if(Math.abs(landed - GAP) <= 8){
    tip.className='tip ok'; tip.textContent='验证通过';
    document.title='验证通过';
    var t='vt_'+Math.random().toString(36).slice(2,10);
    document.cookie='slider_sid='+t+';path=/';
    localStorage.setItem('slider_token', t);
  } else {
    tip.textContent='验证失败，再试一次（差 ' + Math.round(landed-GAP) + ' px）';
    setPos(0);
  }
});
</script>
</body></html>"""

html = HTML % {"W": W, "H": H, "PW": PIECE + 24, "PH": PIECE + 24,
               "GAPX": GAP_X, "GAPY": GAP_Y, "PIECE": PIECE}
with open(os.path.join(OUT, "s1.html"), "w", encoding="utf-8") as f:
    f.write(html)

print("背景 %dx%d，缺口 x=%d（拼图块从 x=8 起，所以要拖 %d px）" % (
    W, H, GAP_X, GAP_X - 8))
print("生成 →", OUT)
for n in sorted(os.listdir(OUT)):
    print("  ", n)
