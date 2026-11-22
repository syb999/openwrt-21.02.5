#!/usr/bin/env python3
# 板上跑：探测真实网站登录页的结构
import json
import subprocess
import sys

SITES = [
    ("https://github.com/login", "GitHub"),
    ("https://passport.jd.com/new/login.aspx", "京东"),
    ("https://www.zhihu.com/signin", "知乎"),
    ("https://passport.bilibili.com/login", "B站"),
]


def call(op, req, timeout=180):
    try:
        r = subprocess.run([sys.executable, "browse.py", op],
                           input=json.dumps(req), capture_output=True,
                           text=True, cwd="/tmp/bz", timeout=timeout)
        return json.loads(r.stdout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "超时"}
    except ValueError:
        return {"ok": False, "error": "非 JSON: %s" % (r.stdout or r.stderr)[:100]}


for url, name in SITES:
    print("=" * 72)
    print("%s  %s" % (name, url))
    print("=" * 72)
    d = call("probe", {"url": url, "wait": 3})
    if not d.get("ok"):
        print("  FAIL %s" % (d.get("error") or "")[:120])
        continue
    print("  标题: %r" % (d.get("title") or "")[:40])
    print("  验证码: %s %s" % (d.get("captcha"), d.get("captcha_kind") or ""))
    print("  输入框:")
    for i in d.get("inputs", []):
        print("    [%d] type=%-9s ph=%-16r name=%-10r label=%r" % (
            i["i"], i.get("type"), (i.get("placeholder") or "")[:16],
            (i.get("name") or "")[:10], (i.get("label") or "")[:14]))
    print("  按钮:")
    for b in d.get("buttons", [])[:6]:
        print("    [%d] %-6s %r" % (b["i"], b.get("tag", ""), (b.get("text") or "")[:26]))
    print()
