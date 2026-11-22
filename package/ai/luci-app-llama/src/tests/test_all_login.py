#!/usr/bin/env python3
# 9 个登录页全量回归：7 个静态模板 + 2 个 Vue SPA
import json
import subprocess
import sys
import time

cases = [
    ("p1", "静态 · 标准中文表单", "http://127.0.0.1/bztest/p1.html"),
    ("p2", "静态 · 英文 Sign in", "http://127.0.0.1/bztest/p2.html"),
    ("p3", "静态 · 只有 name/id", "http://127.0.0.1/bztest/p3.html"),
    ("p4", "静态 · 带验证码", "http://127.0.0.1/bztest/p4.html"),
    ("p5", "静态 · 需要先切 tab", "http://127.0.0.1/bztest/p5.html"),
    ("p6", "静态 · React 受控", "http://127.0.0.1/bztest/p6.html"),
    ("p7", "静态 · 复杂布局干扰", "http://127.0.0.1/bztest/p7.html"),
    ("v1", "Vue · v-model + localStorage", "http://127.0.0.1/vuetest/v1.html"),
    ("v2", "Vue · 弹窗 + 两步点击 + 懒加载", "http://127.0.0.1/vuetest/v2.html"),
]

print("%-4s %-30s %-5s %-7s %-6s %s" % ("页", "场景", "结果", "cookie", "耗时", "说明"))
print("-" * 104)
okn = 0
total = 0.0
for tag, desc, url in cases:
    req = {"url": url, "user": "testuser", "pass": "testpass1"}
    t0 = time.time()
    r = subprocess.run([sys.executable, "browse.py", "autologin"],
                       input=json.dumps(req), capture_output=True, text=True, timeout=180)
    dt = time.time() - t0
    total += dt
    try:
        d = json.loads(r.stdout)
    except ValueError:
        print("%-4s %-30s %-5s %-7s %5.1fs %s" % (tag, desc, "ERR", "-", dt, "输出非 JSON"))
        continue
    if d.get("ok"):
        okn += 1
        st = d.get("storage") or {}
        keys = list((st.get("local") or {}).keys())
        note = "cookie=%s storage=%s" % (
            d.get("cookie_header", "")[:26] or "-", ",".join(keys) or "-")
        if d.get("filled", {}).get("tab_clicked"):
            note += " tab=" + d["filled"]["tab_clicked"]
    else:
        note = (d.get("error") or "")[:44]
    print("%-4s %-30s %-5s %-7s %5.1fs %s" % (
        tag, desc, "OK" if d.get("ok") else "FAIL",
        d.get("cookie_count", 0), dt, note))

print("-" * 104)
print("通过 %d/%d   总耗时 %.1fs   平均 %.1fs" % (okn, len(cases), total, total / len(cases)))
