#!/usr/bin/env python3
# 走 agent 的框架直连路径（不是直接调插件），验证整条链路
import json
import re
import subprocess
import sys
import time

pages = [(1, "标准中文表单"), (2, "英文 Sign in"), (3, "只有 name/id"),
         (4, "带验证码"), (5, "需要先切 tab"), (6, "React 受控"), (7, "复杂布局干扰")]

print("%-4s %-16s %-6s %-8s %-8s %s" % ("页", "特点", "结果", "cookie", "耗时", "说明"))
print("-" * 96)
okn = 0
total = 0
for n, tag in pages:
    task = "账号=testuser 密码=testpass 登录 http://127.0.0.1/bztest/p%d.html" % n
    t0 = time.time()
    r = subprocess.run(["./agent", "--task", task, "--plugins", "./plugins",
                        "--max-steps", "1"],
                       capture_output=True, text=True, timeout=200)
    dt = time.time() - t0
    total += dt
    m = re.search(r"\[browser\] (\{.*)", r.stdout)
    if not m:
        print("p%-3d %-16s %-6s %-8s %5.1fs %s" % (n, tag, "NONE", "-", dt, "没拿到插件输出"))
        continue
    try:
        d = json.loads(m.group(1))
    except ValueError:
        print("p%-3d %-16s %-6s %-8s %5.1fs %s" % (n, tag, "PARSE", "-", dt, "JSON 解析失败"))
        continue
    if d.get("ok"):
        okn += 1
    note = d.get("error") or d.get("warning") or ""
    if d.get("filled"):
        note = "填 %s/%s 点 %s" % (d["filled"].get("user_i"), d["filled"].get("pass_i"),
                                  d["filled"].get("btn_i"))
    print("p%-3d %-16s %-6s %-8s %5.1fs %s" % (
        n, tag, "OK" if d.get("ok") else "FAIL", d.get("cookie_count", 0), dt, note[:46]))

print("-" * 96)
print("成功 %d/%d   总耗时 %.1fs   平均 %.1fs/页" % (okn, len(pages), total, total / len(pages)))
