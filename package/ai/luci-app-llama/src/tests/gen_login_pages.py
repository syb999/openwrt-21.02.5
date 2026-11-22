#!/usr/bin/env python3
# 生成一套登录页模板，覆盖真实站点常见的布局差异
import os

OUT = "/tmp/bztest"
os.makedirs(OUT, exist_ok=True)

HEAD = """<!DOCTYPE html><html><head><meta charset="utf-8">
<title>%s</title><style>
body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0;background:#f5f6f8}
nav{background:#223;color:#fff;padding:14px 20px}
.wrap{max-width:380px;margin:60px auto;background:#fff;padding:28px;border-radius:10px;
      box-shadow:0 4px 16px rgba(0,0,0,.08)}
h2{margin:0 0 18px;font-size:20px}
label{display:block;font-size:13px;color:#666;margin:12px 0 5px}
input{width:100%%;box-sizing:border-box;padding:10px;border:1px solid #d5d8dd;border-radius:6px;font-size:14px}
button{width:100%%;margin-top:18px;padding:11px;border:0;border-radius:6px;
       background:#2f6fed;color:#fff;font-size:15px;cursor:pointer}
footer{margin-top:40px;text-align:center;color:#99a}
</style></head><body>%s</body></html>"""


FINISH_JS = """
(function(){
  /* 模拟真实站点：登录成功后登录表单会消失、出现"退出登录"入口。
     以前的测试页只改 title 和 cookie、页面不变，跟真实行为不符 ——
     新的判据（看还有没有密码框）会正确地判成"没登进去"。 */
  function finish(){
    var roots = [document];
    (function collect(r){
      r.querySelectorAll('*').forEach(function(el){
        if (el.shadowRoot) { roots.push(el.shadowRoot); collect(el.shadowRoot); }
      });
    })(document);
    roots.forEach(function(r){
      r.querySelectorAll('input, [contenteditable]').forEach(function(i){
        try { i.remove(); } catch (e) {}
      });
    });
    var m = document.querySelector('.msg');
    if (m) { m.className = 'msg ok'; m.textContent = '登录成功'; }
    var b = document.createElement('div');
    b.textContent = '退出登录';
    b.style.cssText = 'display:inline-block;padding:10px 16px;margin-top:12px;'
      + 'color:#2563eb;cursor:pointer;border:1px solid #d6dae1;border-radius:7px';
    (document.querySelector('.wrap') || document.body).appendChild(b);
    document.title = '登录成功';
  }
  window.__finish = finish;
  /* 只包"登录成功"那几个回调，别碰 sw 这种切 tab 的 */
  ['ok', 'done', 'doLogin'].forEach(function(n){
    var f = window[n];
    if (typeof f === 'function' && !f.__wrapped) {
      var w = function(){ var r = f.apply(this, arguments); try { finish(); } catch (e) {} return r; };
      w.__wrapped = 1;
      window[n] = w;
    }
  });
})();
"""


def page(name, title, body, js):
    html = HEAD % (title, body)
    html = html.replace("</body>",
                        "<script>%s</script>\n<script>%s</script>\n</body>" % (js, FINISH_JS))
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write(html)
    print("  ", name)


# p1 — 最标准：label + 中文 + 按钮文字"登录"
page("p1.html", "标准登录", """
<nav>示例站点</nav>
<div class="wrap">
  <h2>账号登录</h2>
  <label>账号</label><input type="text" name="username">
  <label>密码</label><input type="password" name="password">
  <button onclick="ok('p1')">登录</button>
</div>
<footer>© 示例</footer>""", """
function ok(t){document.cookie='p1_sid=ok_'+t+';path=/';document.title='登录成功';}""")

# p2 — 英文站：只有 placeholder，按钮是 Sign in
page("p2.html", "Sign in", """
<nav>Acme</nav>
<div class="wrap">
  <h2>Welcome back</h2>
  <input type="email" name="email" placeholder="Email address">
  <input type="password" name="pwd" placeholder="Password">
  <button onclick="ok()">Sign in</button>
</div>""", """
function ok(){document.cookie='p2_token=ok_p2;path=/';document.title='Signed in';}""")

# p3 — 什么提示都没有，只有 name/id（最难）
page("p3.html", "登录", """
<div class="wrap">
  <h2>登录</h2>
  <input type="text" id="uid" name="account">
  <input type="password" id="pwd" name="secret">
  <button onclick="ok()">确认</button>
</div>""", """
function ok(){document.cookie='p3_s=ok_p3;path=/';document.title='ok';}""")

# p4 — 手机号 + 密码 + 验证码（应该明确报错）
page("p4.html", "登录", """
<div class="wrap">
  <h2>手机登录</h2>
  <label>手机号</label><input type="tel" name="mobile">
  <label>密码</label><input type="password" name="password">
  <label>验证码</label>
  <input type="text" name="captcha" style="width:100px;display:inline-block">
  <img src="data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='80' height='34'><text x='8' y='24' font-size='20'>8a3f</text></svg>">
  <button onclick="ok()">登录</button>
</div>""", """
function ok(){document.cookie='p4_sid=ok_p4;path=/';}""")

# p5 — 需要先点"账号登录"tab（预期失败，提示用 steps）
page("p5.html", "登录", """
<div class="wrap">
  <div style="display:flex;gap:10px;margin-bottom:18px">
    <a href="#" id="tabq" onclick="return sw('q')">扫码登录</a>
    <a href="#" id="taba" onclick="return sw('a')">账号登录</a>
  </div>
  <div id="boxq"><p style="color:#888">请用手机扫码</p></div>
  <div id="boxa" style="display:none">
    <input type="text" name="username" placeholder="账号">
    <input type="password" name="password" placeholder="密码">
    <button onclick="ok()">登录</button>
  </div>
</div>""", """
function sw(k){
  document.getElementById('boxq').style.display = k==='q'?'block':'none';
  document.getElementById('boxa').style.display = k==='a'?'block':'none';
  return false;
}
function ok(){document.cookie='p5_sid=ok_p5;path=/';}""")

# p6 — React 风格受控组件：直接改 .value 无效，必须触发 input 事件
page("p6.html", "React 登录", """
<div class="wrap">
  <h2>会员登录</h2>
  <input id="ru" type="text" placeholder="会员账号">
  <input id="rp" type="password" placeholder="登录密码">
  <button id="rb">立即登录</button>
</div>""", """
// 模拟受控组件：只认 input 事件带来的值
var state={u:'',p:''};
var el_u=document.getElementById('ru'), el_p=document.getElementById('rp');
el_u.addEventListener('input',function(){state.u=el_u.value;});
el_p.addEventListener('input',function(){state.p=el_p.value;});
document.getElementById('rb').addEventListener('click',function(){
  if(state.u && state.p){
    document.cookie='p6_sid=ok_p6_'+state.u.length;
    document.title='登录成功';
    /* 这个页面没走全局 ok()，得手动调一次，让"表单消失+退出入口"生效 */
    if (window.__finish) { try { window.__finish(); } catch (e) {} }
  }
  else{document.title='账号或密码为空';}
});""")

# p7 — 复杂布局干扰：导航链接、搜索框、页脚表单全都有
page("p7.html", "门户登录", """
<nav>门户 | <a href="#">首页</a> <a href="#">新闻</a>
  <input type="text" placeholder="站内搜索" style="width:160px;float:right">
</nav>
<div class="wrap">
  <h2>用户登录</h2>
  <label>用户名 / 手机号</label>
  <input type="text" name="loginName" autocomplete="username">
  <label>登录密码</label>
  <input type="password" name="loginPwd" autocomplete="current-password">
  <button type="submit" onclick="ok();return false">登 录</button>
</div>
<footer>
  <input type="text" placeholder="订阅邮箱">
  <button>订阅</button>
</footer>""", """
function ok(){document.cookie='p7_sess=ok_p7;path=/';document.title='登录成功';}""")

# index
idx = "<html><head><meta charset='utf-8'><title>登录页测试集</title></head><body>"
idx += "<h2>登录页模板</h2><ul>"
for i in range(1, 8):
    idx += "<li><a href='p%d.html'>p%d — %s</a></li>" % (
        i, i, ["", "标准中文表单", "英文 Sign in", "只有 name/id", "带验证码",
               "需要先切 tab", "React 受控", "复杂布局干扰"][i])
idx += "</ul></body></html>"
open(os.path.join(OUT, "index.html"), "w", encoding="utf-8").write(idx)
print("   index.html")
print("共 %d 个文件 → %s" % (len(os.listdir(OUT)), OUT))
