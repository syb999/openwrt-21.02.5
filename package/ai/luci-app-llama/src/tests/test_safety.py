#!/usr/bin/env python3
# 验证"账号密码错绝不重试"：故意给 tries=3，但应该只试 1 次就停
import json
import subprocess
import sys

CASES = [
    ("GitHub", "https://github.com/login"),
    ("恩山论坛", "https://www.right.com.cn/forum/member.php?mod=logging&action=login"),
    ("知乎", "https://www.zhihu.com/signin"),
    ("B站", "https://passport.bilibili.com/login"),
]


def call(op, req, timeout=220):
    try:
        r = subprocess.run([sys.executable, "browse.py", op],
                           input=json.dumps(req), capture_output=True,
                           text=True, cwd="/tmp/bz", timeout=timeout)
        return json.loads(r.stdout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "超时"}
    except ValueError:
        return {"ok": False, "error": "非 JSON: %s" % (r.stdout or r.stderr)[:120]}


print("%-10s %-8s %-8s %-16s %s" % ("站点", "试几次", "tries_used", "失败类型", "是否出于安全停止"))
print("-" * 74)
for name, url in CASES:
    # 故意给 tries=3，看它会不会真的提交三次
    d = call("autologin", {"url": url,
                           "user": "no_such_acct_9z7", "pass": "WrongPass_9z7!",
                           "tries": 3})
    hist = d.get("history") or []
    print("%-10s %-8s %-8s %-16s %s" % (
        name, 3, d.get("tries_used", len(hist)),
        d.get("failure_kind") or "-",
        "是" if d.get("stopped_for_safety") else "否"))
    if d.get("stopped_for_safety"):
        print("            └ %s" % (d.get("error") or "")[:64])
    # 关键断言：提交次数必须 <= 1
    if len(hist) > 1 and (d.get("failure_kind") == "credentials"):
        print("            !!! 危险：账号密码错却提交了 %d 次" % len(hist))
    print()
