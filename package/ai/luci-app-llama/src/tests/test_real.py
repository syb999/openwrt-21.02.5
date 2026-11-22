#!/usr/bin/env python3
# 板上跑：真实站点 + 错误凭据，验证"能不能填、能不能点对按钮、判据是否说真话"
import json
import subprocess
import sys

SITES = [
    ("https://github.com/login", "GitHub", "plain"),
    ("https://passport.jd.com/new/login.aspx", "京东", "tab+vc"),
    ("https://www.zhihu.com/signin", "知乎", "sms-default"),
    ("https://passport.bilibili.com/login", "B站", "no-btn"),
]


def call(op, req, timeout=200):
    try:
        r = subprocess.run([sys.executable, "browse.py", op],
                           input=json.dumps(req), capture_output=True,
                           text=True, cwd="/tmp/bz", timeout=timeout)
        return json.loads(r.stdout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "超时 %ds" % timeout}
    except ValueError:
        return {"ok": False, "error": "非 JSON: %s" % (r.stdout or r.stderr)[:120]}


for url, name, tag in SITES:
    print("=" * 74)
    print("%s [%s]" % (name, tag))
    print("=" * 74)
    d = call("autologin", {"url": url,
                           "user": "no_such_acct_9z7@example.com",
                           "pass": "WrongPass_123!",
                           "tries": 1})
    v = d.get("verdict") or {}
    f = d.get("filled") or {}
    if not d.get("ok") and not v:
        print("  流程中断: %s" % (d.get("error") or "")[:110])
        continue
    print("  likely_ok: %-5s  score: %s" % (v.get("likely_ok"), v.get("score")))
    print("  账号框=%s 密码框=%s 按钮=%s 点中=%s" % (
        f.get("user_i"), f.get("pass_i"), f.get("btn_i"), f.get("clicked")))
    print("  填值校验: 账号=%s 密码=%s" % (f.get("user_ok"), f.get("pass_ok")))
    print("  还有密码框=%s 在登录页=%s 标题登录=%s" % (
        v.get("still_has_password"), v.get("on_login_page"), v.get("title_is_login")))
    if d.get("error"):
        print("  error: %s" % d["error"][:100])
    if d.get("diagnosis"):
        print("  诊断: %s" % (d["diagnosis"].get("reasons") or [])[:2])
    print()
