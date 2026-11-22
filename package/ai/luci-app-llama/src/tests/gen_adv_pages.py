#!/usr/bin/env python3
# 生成 6 个"现代框架特征"登录页，用来验证 selenium 插件的兼容性
#   s1 Shadow DOM      Web Component 里的登录框，querySelectorAll 穿不透
#   s2 多步骤          先账号 → 下一步 → 再密码（Google/企业站那套）
#   s3 div 当按钮      没有 button 标签，纯 div + cursor:pointer
#   s4 懒加载          1.5 秒后才渲染表单（骨架屏）
#   s5 contenteditable 账号框是 contenteditable div，不是 input
#   s6 reCAPTCHA       带"我不是机器人"占位框（过不了，要能明确报出来）
import os

OUT = "/tmp/advtest"
os.makedirs(OUT, exist_ok=True)

BASE_CSS = """
body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:#eef1f6;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#fff;padding:30px 28px;border-radius:12px;width:350px;
      box-shadow:0 8px 30px rgba(0,0,0,.12)}
h2{margin:0 0 18px;font-size:19px;color:#1b2733}
input,textarea{width:100%;box-sizing:border-box;padding:10px;margin-bottom:12px;
      border:1px solid #d6dae1;border-radius:7px;font-size:14px;font-family:inherit}
.btn{width:100%;padding:11px;border:0;border-radius:7px;background:#2563eb;color:#fff;
     font-size:15px;cursor:pointer;text-align:center;user-select:none}
.msg{margin-top:10px;font-size:13px;color:#e33}
.ok{color:#2a7}
.tip{font-size:11.5px;color:#8b98a5;margin-bottom:12px;line-height:1.5}
"""

DONE_JS = """
function done(tag){
  var tok = 'adv_' + Math.random().toString(36).slice(2,10);
  localStorage.setItem('token', tok);
  document.cookie = tag + '_sid=' + tok + ';path=/';
  document.title = '登录成功';
  var m = document.querySelector('.msg');
  if (m) { m.className = 'msg ok'; m.textContent = '登录成功'; }
  /* 模拟真实站点：登录成功后表单消失、出现"退出登录"入口。
     判据就是看这两点，页面不跟着变会被正确判成"没登进去"。 */
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
  var b = document.createElement('div');
  b.textContent = '退出登录';
  b.style.cssText = 'display:inline-block;padding:10px 16px;margin-top:12px;'
    + 'color:#2563eb;cursor:pointer;border:1px solid #d6dae1;border-radius:7px';
  document.body.appendChild(b);
}
"""


def page(title, tip, body, extra_js="", css=""):
    return """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>%s</title>
<style>%s%s</style></head>
<body>
<div class="card">
  <h2>%s</h2>
  <div class="tip">%s</div>
%s
</div>
<script>%s%s</script>
</body></html>""" % (title, BASE_CSS, css, title, tip, body, DONE_JS, extra_js)


pages = {}

# ---------- s1: Shadow DOM ----------
pages["s1_shadow.html"] = page(
    "Shadow DOM 登录",
    "登录框整个在 Web Component 的 shadowRoot 里，普通 querySelectorAll 找不到",
    """<div id="host"></div>""",
    """
customElements.define('login-box', class extends HTMLElement {
  connectedCallback(){
    var sr = this.attachShadow({mode:'open'});
    sr.innerHTML = `
      <style>
        input{width:100%;box-sizing:border-box;padding:10px;margin-bottom:12px;
              border:1px solid #d6dae1;border-radius:7px;font-size:14px}
        .btn{width:100%;padding:11px;border-radius:7px;background:#2563eb;color:#fff;
             font-size:15px;cursor:pointer;text-align:center}
        .msg{margin-top:10px;font-size:13px;color:#e33}
      </style>
      <input id="u" placeholder="用户名" autocomplete="username">
      <input id="p" type="password" placeholder="密码" autocomplete="current-password">
      <div class="btn" id="go">登 录</div>
      <div class="msg" id="m"></div>`;
    var that = this;
    sr.getElementById('go').addEventListener('click', function(){
      var u = sr.getElementById('u').value, p = sr.getElementById('p').value;
      if (!u || !p) { sr.getElementById('m').textContent = '请填账号密码'; return; }
      var tok = 'adv_' + Math.random().toString(36).slice(2,10);
      localStorage.setItem('token', tok);
      document.cookie = 's1_sid=' + tok + ';path=/';
      document.title = '登录成功';
      sr.getElementById('m').textContent = '登录成功';
      sr.getElementById('m').style.color = '#2a7';
      /* shadow 内部也要模拟"表单消失 + 退出入口" */
      sr.querySelectorAll('input').forEach(function(i){ try { i.remove(); } catch (e) {} });
      var ob = document.createElement('div');
      ob.textContent = '退出登录';
      ob.style.cssText = 'padding:10px;color:#2563eb;cursor:pointer;text-align:center';
      sr.appendChild(ob);
    });
  }
});
document.getElementById('host').innerHTML = '<login-box></login-box>';
""",
    "")

# ---------- s2: 多步骤（账号 → 下一步 → 密码）----------
pages["s2_step.html"] = page(
    "两步登录",
    "先填账号点下一步，密码框才出现（Google/Microsoft 那种流程）",
    """<input id="u" placeholder="邮箱或手机号" autocomplete="username">
<div id="step2" style="display:none">
  <input id="p" type="password" placeholder="密码" autocomplete="current-password">
  <div class="btn" id="go2">登 录</div>
</div>
<div class="btn" id="go1">下一步</div>
<div class="msg" id="m"></div>""",
    """
var step = 1;
document.getElementById('go1').addEventListener('click', function(){
  if (!document.getElementById('u').value) {
    document.getElementById('m').textContent = '请输入账号'; return;
  }
  step = 2;
  document.getElementById('step2').style.display = 'block';
  document.getElementById('go1').style.display = 'none';
  document.getElementById('m').textContent = '';
});
document.getElementById('go2').addEventListener('click', function(){
  if (!document.getElementById('u').value || !document.getElementById('p').value) {
    document.getElementById('m').textContent = '请填完整'; return;
  }
  done('s2');
});
""",
    "")

# ---------- s3: div 当按钮 ----------
pages["s3_divbtn.html"] = page(
    "div 按钮",
    "登录按钮是 div，没有 button 标签、没有 role=button，只有 cursor:pointer",
    """<input id="u" placeholder="账号">
<input id="p" type="password" placeholder="密码">
<div class="fakebtn" id="go">立即登录 &rarr;</div>
<div class="msg" id="m"></div>
<div style="margin-top:14px;font-size:12px;color:#9aa7b4">
  底部干扰链接：<span class="lnk">忘记密码</span> · <span class="lnk">注册新账号</span>
</div>""",
    """
document.getElementById('go').addEventListener('click', function(){
  if (!document.getElementById('u').value || !document.getElementById('p').value) {
    document.getElementById('m').textContent = '请填账号密码'; return;
  }
  done('s3');
});
""",
    """.fakebtn{width:100%;padding:11px;border-radius:7px;background:linear-gradient(90deg,#2563eb,#4f46e5);
  color:#fff;font-size:15px;cursor:pointer;text-align:center;user-select:none}
.lnk{color:#2563eb;cursor:pointer}""")

# ---------- s4: 懒加载表单 ----------
pages["s4_lazy.html"] = page(
    "懒加载表单",
    "打开后 1.5 秒才渲染出表单，之前只有骨架屏",
    """<div id="box">
  <div style="height:38px;background:#eef1f6;border-radius:7px;margin-bottom:12px"></div>
  <div style="height:38px;background:#eef1f6;border-radius:7px;margin-bottom:12px"></div>
  <div style="height:40px;background:#e6ebf2;border-radius:7px"></div>
</div>
<div class="msg" id="m"></div>""",
    """
setTimeout(function(){
  document.getElementById('box').innerHTML =
    '<input id="u" placeholder="用户名">' +
    '<input id="p" type="password" placeholder="密码">' +
    '<div class="btn" id="go">登 录</div>';
  document.getElementById('go').addEventListener('click', function(){
    if (!document.getElementById('u').value || !document.getElementById('p').value) {
      document.getElementById('m').textContent = '请填账号密码'; return;
    }
    done('s4');
  });
}, 1500);
""",
    "")

# ---------- s5: contenteditable ----------
pages["s5_editable.html"] = page(
    "contenteditable 账号框",
    "账号输入框是 contenteditable div，不是 input，读 .value 拿不到内容",
    """<div class="ce" id="u" contenteditable="true" data-ph="用户名"></div>
<input id="p" type="password" placeholder="密码">
<div class="btn" id="go">登 录</div>
<div class="msg" id="m"></div>""",
    """
document.getElementById('go').addEventListener('click', function(){
  var u = (document.getElementById('u').innerText || '').trim();
  var p = document.getElementById('p').value;
  if (!u || !p) { document.getElementById('m').textContent = '请填账号密码'; return; }
  done('s5');
});
""",
    """.ce{width:100%;box-sizing:border-box;padding:10px;margin-bottom:12px;
  border:1px solid #d6dae1;border-radius:7px;font-size:14px;min-height:20px;outline:none}
.ce:empty:before{content:attr(data-ph);color:#aab4c0}""")

# ---------- s6: reCAPTCHA 占位 ----------
pages["s6_recaptcha.html"] = page(
    "带 reCAPTCHA",
    "有 Google reCAPTCHA 那种'我不是机器人'框，自动登录过不去，要能明确报出来",
    """<input id="u" placeholder="账号">
<input id="p" type="password" placeholder="密码">
<div class="rc g-recaptcha" id="rc" data-sitekey="6LeIxAcTAAAAAJcZVRqyHh71UMIEGNQ_MXjiZKhI">
  <div class="rc-box"></div>
  <span>我不是机器人</span>
  <div class="rc-logo">reCAPTCHA</div>
</div>
<div class="btn" id="go">登 录</div>
<div class="msg" id="m"></div>""",
    """
document.getElementById('go').addEventListener('click', function(){
  document.getElementById('m').textContent = '请先完成人机验证';
});
""",
    """.rc{display:flex;align-items:center;gap:10px;border:1px solid #d6dae1;
  border-radius:6px;padding:10px 12px;margin-bottom:12px;background:#fafbfc}
.rc-box{width:20px;height:20px;border:2px solid #b9c2cc;border-radius:3px;flex:none}
.rc span{font-size:13.5px;color:#3c4043;flex:1}
.rc-logo{font-size:9px;color:#9aa0a6}""")

for name, html in pages.items():
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write(html)

print("生成 %d 个页面 → %s" % (len(pages), OUT))
for n in sorted(pages):
    print("  " + n)
