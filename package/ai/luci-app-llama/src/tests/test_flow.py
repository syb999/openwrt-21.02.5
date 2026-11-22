#!/usr/bin/env python3
# 板上跑：完整 flow 测试 —— 登录 / 等渲染 / 点菜单 / 填搜索 / 回车 / 点详情 / 抓请求
import json
import subprocess
import sys

FLOW = {
    "op": "hunt",
    "url": "http://127.0.0.1/flowtest/f1.html",
    "flow": [
        {"act": "login", "user": "testuser", "pass": "testpass1",
         "page": "登录页", "note": "输账号密码登录"},
        {"act": "wait_for", "text": "业务管理", "tries": 15,
         "page": "工作台", "note": "等左侧菜单渲染出来（1.5 秒后才出）"},
        {"act": "click", "text": "业务管理",
         "page": "工作台", "note": "点业务管理进入模块"},
        {"act": "fill", "field": "订单号", "value": "SO20261119001",
         "page": "业务管理", "note": "搜索框里填订单号"},
        {"act": "press", "key": "Enter", "field": "订单号",
         "page": "业务管理", "note": "在搜索框里按回车触发查询"},
        {"act": "click", "text": "订单详情",
         "page": "搜索结果", "note": "点进订单详情"},
        {"act": "capture",
         "page": "订单详情", "note": "目标页：抓这里发出的请求头"},
    ],
    "wait": 3,
}

r = subprocess.run([sys.executable, "browse.py", "hunt"],
                   input=json.dumps(FLOW, ensure_ascii=False),
                   capture_output=True, text=True, cwd="/tmp/bz", timeout=240)
try:
    d = json.loads(r.stdout)
except ValueError:
    print("输出非 JSON：", (r.stdout or r.stderr)[:300])
    sys.exit(1)

print("ok:", d.get("ok"), " stage:", d.get("stage"),
      " 步数:", "%s/%s" % (d.get("flow_done"), d.get("flow_total")))
print("只留最终页:", d.get("final_page_only"), " 丢弃噪音:", d.get("noise_dropped"))
print("最后一步原始数据:", json.dumps(d.get("steps", [{}])[-1], ensure_ascii=False)[:200])
print()
print("%-3s %-10s %-12s %-5s %s" % ("#", "act", "page", "ok", "说明"))
print("-" * 78)
for s in d.get("steps", []):
    print("%-3s %-10s %-12s %-5s %s" % (
        s.get("step"), s.get("act"), (s.get("page") or "")[:11],
        "OK" if s.get("ok") else "FAIL",
        (s.get("note") or s.get("error") or "")[:40]))
print()
if d.get("error"):
    print("错误:", d["error"])
print("请求数:", d.get("request_count"))
for it in d.get("requests", []):
    print("  ", it.get("method"), it.get("url"))
    print("     ", {k: (v[:22] + "…" if len(v) > 22 else v)
                    for k, v in (it.get("headers") or {}).items()})
print()
print("curl:", (d.get("curl") or "-")[:240])
