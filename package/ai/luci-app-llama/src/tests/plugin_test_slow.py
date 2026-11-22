#!/usr/bin/env python3
"""慢批：browser（7 op）+ 网络 + 音乐合成"""
import json
import subprocess
import sys
import time

AGENT = "/opt/llama/agent/agent-c"
CWD = "/opt/llama/agent"


def run(task, timeout=200):
    t0 = time.time()
    try:
        r = subprocess.run([AGENT, "--task", json.dumps(task, ensure_ascii=False),
                            "--max-steps", "1"],
                           capture_output=True, text=True, cwd=CWD, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT %ds" % timeout, time.time() - t0
    dt = time.time() - t0
    out = (r.stdout or "").strip()
    line = ""
    for L in reversed(out.split("\n")):
        L = L.strip()
        if L.startswith("{") and L.endswith("}"):
            line = L
            break
    if not line:
        return False, "无 JSON: " + (r.stderr or out)[-80:].replace("\n", " "), dt
    try:
        d = json.loads(line)
    except Exception as e:
        return False, "JSON坏: %s" % str(e)[:40], dt
    if d.get("error"):
        return False, "ERR: " + str(d["error"])[:80], dt
    if d.get("ok") is False:
        return False, "ok=false " + str(d.get("stage") or d.get("msg") or "")[:60], dt
    parts = []
    if d.get("ok") is True:
        parts.append("ok")
    if d.get("shot"):
        parts.append("shot")
    if d.get("script_path"):
        parts.append("script")
    if d.get("headers_path"):
        parts.append("headers")
    if d.get("cookie") or d.get("cookies"):
        parts.append("cookie")
    if d.get("captcha_text"):
        parts.append("ocr=%s" % d["captcha_text"])
    if d.get("passed"):
        parts.append("passed")
    for k in ("value", "answer", "text", "title"):
        if k in d:
            v = d[k]
            s = v if isinstance(v, str) else json.dumps(v, ensure_ascii=False)
            return True, s.replace("\n", " ")[:95], dt
    return True, json.dumps(d, ensure_ascii=False)[:90], dt


B = "http://127.0.0.1"
CASES = [
    ("browser/probe",    {"op": "probe", "url": B + "/bztest/p1.html", "wait": 2}),
    ("browser/shot",     {"op": "shot", "url": B + "/bztest/p1.html"}),
    ("browser/cookie",   {"op": "cookie", "url": B + "/bztest/p1.html"}),
    ("browser/autologin", {"op": "autologin", "url": B + "/bztest/p1.html",
                           "user": "alice", "pass": "secret123"}),
    ("browser/captcha",  {"op": "captcha", "url": B + "/captest/c1.html", "retry": 2}),
    ("browser/drag_auto", {"op": "drag", "url": B + "/slidertest/s1.html", "auto": True}),
    ("browser/hunt",     {"op": "hunt", "url": B + "/huntest/h1.html",
                          "user": "testuser", "pass": "testpass1",
                          "path": ["订单管理", "订单列表", "今日订单"], "wait": 3}),
    ("net/ping",    {"op": "ping", "host": "10.80.1.1", "count": 3}),
    ("net/port",    {"op": "port", "host": "10.80.1.1", "port": 80}),
    ("net/resolve", {"op": "resolve", "host": "www.baidu.com"}),
    ("sys/http_get", {"op": "http_get", "url": B + "/bztest/p1.html"}),
    ("music/render", {"op": "render", "path": "/tmp/t2.mp3",
                      "notes": [{"pitch": 69, "dur": 1}, {"pitch": 72, "dur": 1},
                                {"pitch": 76, "dur": 2}]}),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    passed = failed = 0
    fails = []
    for name, task in CASES:
        if only and only not in name:
            continue
        ok, msg, dt = run(task)
        if ok:
            passed += 1
        else:
            failed += 1
            fails.append((name, msg))
        print("%-20s %s %6.1fs  %s" % (name, "✓" if ok else "✗", dt, msg), flush=True)
    print("\n" + "=" * 74)
    print("通过 %d / 失败 %d" % (passed, failed))
    if fails:
        print("\n失败:")
        for n, m in fails:
            print("  ✗ %-20s %s" % (n, m))


if __name__ == "__main__":
    main()
