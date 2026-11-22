import os
from PIL import Image, ImageDraw, ImageFont

os.makedirs("/www/captest", exist_ok=True)
CODE = "8F3K"
W, H = 150, 52
img = Image.new("RGB", (W, H), (250, 250, 250))
d = ImageDraw.Draw(img)
font = None
for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
          "/usr/share/fonts/DejaVuSans-Bold.ttf",
          "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
    if os.path.exists(p):
        font = ImageFont.truetype(p, 34)
        break
if font is None:
    font = ImageFont.load_default()
for i, ch in enumerate(CODE):
    d.text((12 + i * 32, 6), ch, fill=(20, 20, 20), font=font)
# 干扰线
for k in range(3):
    d.line([(0, 12 + k * 14), (W, 20 + k * 10)], fill=(170, 170, 170), width=1)
img.save("/www/captest/cap.png")

open("/www/captest/c1.html", "w", encoding="utf-8").write("""<!doctype html>
<html><head><meta charset="utf-8"><title>验证码测试</title></head>
<body style="font-family:system-ui;padding:40px">
<h2>请输入验证码</h2>
<form onsubmit="event.preventDefault();alert('提交了')">
  <img id="cap" src="cap.png" alt="验证码" style="border:1px solid #ccc;cursor:pointer">
  <br><br>
  <input name="code" placeholder="验证码" style="padding:8px;width:200px">
  <button type="submit" style="padding:8px 16px">提交</button>
</form>
</body></html>""")
print("已生成 /www/captest/{cap.png,c1.html}  验证码=%s" % CODE)
