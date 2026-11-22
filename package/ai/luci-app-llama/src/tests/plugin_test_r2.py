#!/usr/bin/env python3
"""插件强力测试 第二轮 —— 用各插件的真实参数（字段名按源码来）"""
import json
import subprocess
import sys
import time

AGENT = "/opt/llama/agent/agent-c"
CWD = "/opt/llama/agent"


def run(task, timeout=100):
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
        return False, "无 JSON: " + (r.stderr or out)[-90:].replace("\n", " "), dt
    try:
        d = json.loads(line)
    except Exception as e:
        return False, "JSON坏: %s | %s" % (str(e)[:30], line[:70]), dt
    if d.get("error"):
        return False, "ERR: " + str(d["error"])[:90], dt
    if d.get("ok") is False:
        return False, "ok=false " + str(d.get("stage") or d.get("msg") or "")[:70], dt
    for k in ("value", "answer", "text", "factors", "sections", "title"):
        if k in d:
            v = d[k]
            s = v if isinstance(v, str) else json.dumps(v, ensure_ascii=False)
            return True, s.replace("\n", " ")[:100], dt
    return True, json.dumps(d, ensure_ascii=False)[:90], dt


CASES = [
    # ---------- math（按源码：equation / sub / task 原题）----------
    ("math/calc",      {"op": "calc", "expr": "(1250*0.87+320)/3"}),
    ("math/solve",     {"op": "solve", "equation": "2x+3=11"}),
    ("math/solve2",    {"op": "solve", "equation": "x^2-5x+6=0"}),
    ("math/system",    {"op": "system", "equations": ["x+y=10", "2x-y=2"]}),
    ("math/stat",      {"op": "stat", "data": [12, 7, 9, 15, 22, 8, 31, 19]}),
    ("math/num-factor", {"op": "num", "sub": "factor", "n": 360}),
    ("math/num-gcd",   {"op": "num", "sub": "gcd", "n": 48, "m": 180}),
    ("math/num-fib",   {"op": "num", "sub": "fib", "n": 15}),
    ("math/meet",      {"op": "meet",
                        "task": "甲乙相距280米，甲每分钟走60米，乙每分钟走80米，两人相向而行，几分钟相遇？"}),
    ("math/ring",      {"op": "ring",
                        "task": "环形跑道400米，两人同向跑，甲每秒5米，乙每秒7米，多久乙追上甲？"}),
    ("math/train",     {"op": "train",
                        "task": "火车长200米 速度20米每秒 桥长800米 求过桥时间"}),
    ("math/chicken",   {"op": "chicken",
                        "task": "笼子里鸡和兔共35个头，94只脚，鸡兔各几只？"}),

    # ---------- jq（字段是 file）----------
    ("jq/probe",  {"op": "probe", "file": "/tmp/t_data.json"}),
    ("jq/get",    {"op": "get", "file": "/tmp/t_data.json", "path": ".list",
                   "offset": 0, "limit": 2}),
    ("jq/query",  {"op": "query", "file": "/tmp/t_data.json",
                   "query": ".list[] | select(.price > 100) | .name"}),
    ("jq/keys",   {"op": "keys", "file": "/tmp/t_data.json"}),

    # ---------- table（字段是 sort_by）----------
    ("table/sort",  {"op": "sort", "path": "/tmp/t_table.txt", "sort_by": "金额", "n": 3}),
    ("table/sort2", {"op": "sort", "path": "/tmp/t_table.txt", "sort_by": "数量", "n": 2,
                     "order": "asc"}),

    # ---------- doc（写用 docx/text，不是 write）----------
    ("doc/docx",     {"op": "docx", "path": "/tmp/t2.docx",
                      "content": "# 部署报告\n\n## 硬件\nRK3566 四核 A55。\n\n## 结论\n纯 CPU 推理 4.3 tok/s。"}),
    ("doc/read_docx", {"op": "read", "path": "/tmp/t2.docx"}),
    ("doc/text",     {"op": "text", "path": "/tmp/t2.txt",
                      "content": "第一行\n第二行\n第三行\n"}),
    ("doc/read_txt", {"op": "read", "path": "/tmp/t2.txt"}),
    ("doc/xlsx",     {"op": "xlsx", "path": "/tmp/t2.xlsx",
                      "content": "名称|单价|库存\n保温杯|89|120\n四件套|399|34\n"}),
    ("doc/read_xlsx", {"op": "read", "path": "/tmp/t2.xlsx"}),

    # ---------- sys（write 走纯文本，现在应该路由到 sys）----------
    ("sys/write_txt", {"op": "write", "path": "/tmp/t_syswrite.txt",
                       "content": "hello 123\n第二行"}),
    ("sys/read_txt",  {"op": "read", "path": "/tmp/t_syswrite.txt"}),

    # ---------- owrt（字段是 key）----------
    ("owrt/uci_get", {"op": "uci_get", "key": "network.lan.ipaddr"}),
    ("owrt/uci_get2", {"op": "uci_get", "key": "dhcp.lan.leasetime"}),
    ("owrt/show",    {"op": "uci_show", "package": "network"}),

    # ---------- music（notes 是数组）----------
    ("music/midi", {"op": "midi", "path": "/tmp/t2.mid",
                    "notes": [{"pitch": 69, "dur": 1}, {"pitch": 71, "dur": 1},
                              {"pitch": 73, "dur": 2}, {"pitch": 0, "dur": 1},
                              {"pitch": 76, "dur": 3}]}),
]


def main():
    passed = failed = 0
    fails = []
    for name, task in CASES:
        ok, msg, dt = run(task)
        if ok:
            passed += 1
        else:
            failed += 1
            fails.append((name, msg))
        print("%-18s %s %5.1fs  %s" % (name, "✓" if ok else "✗", dt, msg))
    print("\n" + "=" * 72)
    print("通过 %d / 失败 %d / 共 %d" % (passed, failed, passed + failed))
    if fails:
        print("\n失败:")
        for n, m in fails:
            print("  ✗ %-18s %s" % (n, m))


if __name__ == "__main__":
    main()
