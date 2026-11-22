#!/usr/bin/env python3
# 模拟一个真实业务系统的完整流程：
#   f1 登录（账号密码）
#   f2 工作台 → 菜单 1.5 秒后才渲染（不 wait_for 就点不到）
#   f3 业务管理 → 有个搜索框，要填关键字 + 按回车
#   f4 搜索结果 → 点"订单详情"
#   f5 目标页 → 发带签名的 API（这才是要抓的）
import os

OUT = "/tmp/flowtest"
os.makedirs(OUT, exist_ok=True)

CSS = """
body{margin:0;font-family:system-ui,sans-serif;background:#f4f6f9}
.top{background:#1e3a5f;color:#fff;padding:12px 20px;font-size:14px}
.wrap{max-width:760px;margin:24px auto;padding:0 16px}
.card{background:#fff;border-radius:10px;padding:24px;box-shadow:0 2px 12px rgba(0,0,0,.06)}
h2{margin:0 0 16px;font-size:18px;color:#1b2733}
a.menu{display:inline-block;padding:12px 20px;background:#2563eb;color:#fff;
       border-radius:8px;text-decoration:none;margin:0 12px 12px 0;font-size:15px}
input{width:100%;box-sizing:border-box;padding:10px;margin-bottom:12px;
      border:1px solid #d6dae1;border-radius:7px;font-size:14px}
.btn{width:100%;padding:11px;border-radius:7px;background:#2563eb;color:#fff;
     font-size:15px;cursor:pointer;text-align:center;border:0}
.crumb{font-size:12px;color:#7d8b99;margin-bottom:14px}
.skel{height:44px;background:#e8edf3;border-radius:8px;margin-bottom:12px}
.log{background:#0b1418;color:#8ff0c8;font-family:monospace;font-size:12px;
     padding:12px;border-radius:8px;margin-top:16px;white-space:pre-wrap}
"""


def page(title, crumb, body, js=""):
    return """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>%s</title><style>%s</style></head>
<body><div class="top">某某业务系统 v3.8</div>
<div class="wrap"><div class="card">
%s
%s
</div></div><script>%s</script></body></html>""" % (
        title, CSS, ('<div class="crumb">%s</div>' % crumb) if crumb else "", body, js)


# f1 登录
open(os.path.join(OUT, "f1.html"), "w", encoding="utf-8").write(page(
    "登录", "", """
<h2>账号登录</h2>
<input id="u" placeholder="用户名">
<input id="p" type="password" placeholder="密码">
<div class="btn" id="go">登 录</div>
""", """
document.getElementById('go').addEventListener('click', function(){
  var u = document.getElementById('u').value, p = document.getElementById('p').value;
  if (!u || !p) { alert('请填账号密码'); return; }
  document.cookie = 'fsid=fk_' + Math.random().toString(36).slice(2,10) + ';path=/';
  location.href = 'f2.html';
});
"""))

# f2 工作台：菜单晚 1.5 秒才出来
open(os.path.join(OUT, "f2.html"), "w", encoding="utf-8").write(page(
    "工作台", "首页", """
<h2>工作台</h2>
<div id="menu"><div class="skel"></div><div class="skel"></div></div>
""", """
setTimeout(function(){
  document.getElementById('menu').innerHTML =
    '<a class="menu" href="f3.html">业务管理</a>' +
    '<a class="menu" href="f3.html">报表中心</a>' +
    '<a class="menu" href="f3.html">系统设置</a>';
}, 1500);
"""))

# f3 业务管理：有搜索框，填完按回车
open(os.path.join(OUT, "f3.html"), "w", encoding="utf-8").write(page(
    "业务管理", "首页 / 业务管理", """
<h2>业务管理</h2>
<div style="font-size:13px;color:#7d8b99;margin-bottom:10px">输入订单号后按回车查询</div>
<input id="q" placeholder="输入订单号搜索">
<div id="hint" style="font-size:12px;color:#9aa7b4">（还没输入）</div>
""", """
var q = document.getElementById('q');
q.addEventListener('keydown', function(e){
  if (e.key === 'Enter') {
    if (!q.value) { document.getElementById('hint').textContent = '请输入订单号'; return; }
    location.href = 'f4.html?kw=' + encodeURIComponent(q.value);
  }
});
"""))

# f4 搜索结果：点订单详情
open(os.path.join(OUT, "f4.html"), "w", encoding="utf-8").write(page(
    "搜索结果", "首页 / 业务管理 / 搜索结果", """
<h2>搜索结果</h2>
<div style="font-size:13px;color:#7d8b99;margin-bottom:12px">找到 1 条记录</div>
<div style="border:1px solid #e3e8ef;border-radius:8px;padding:14px">
  <div style="font-size:13px;margin-bottom:8px">订单号 SO20261119001</div>
  <a class="menu" href="f5.html">订单详情</a>
</div>
"""))

# f5 目标页：发带签名的请求
open(os.path.join(OUT, "f5.html"), "w", encoding="utf-8").write(page(
    "订单详情", "首页 / 业务管理 / 搜索结果 / 订单详情", """
<h2>订单详情 SO20261119001</h2>
<div style="font-size:13px;color:#7d8b99">数据加载中…</div>
<div class="log" id="log"></div>
""", """
function sign(ts, nonce) {
  var s = 'order_salt_7c1e|' + ts + '|' + nonce;
  var h = 0x811c9dc5;
  for (var i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = (h * 0x01000193) >>> 0; }
  return h.toString(16).padStart(8, '0');
}
var ts = Date.now(), nonce = Math.random().toString(36).slice(2, 10), sg = sign(ts, nonce);
document.getElementById('log').textContent = 'X-Sign: ' + sg + '\\nX-Timestamp: ' + ts;
fetch('/api/order/detail/SO20261119001', {
  method: 'GET',
  headers: {
    'X-Sign': sg, 'X-Timestamp': String(ts), 'X-Nonce': nonce,
    'X-Trace-Id': 'trace_' + nonce, 'Accept': 'application/json'
  }
}).catch(function(){});
"""))

print("生成 5 个页面 → %s" % OUT)
for n in sorted(os.listdir(OUT)):
    print("  " + n)
