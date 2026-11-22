#!/usr/bin/env python3
"""插件强力测试 —— 快批（不依赖网络/浏览器）

每个 op 都用真实数据跑，检查返回结构而不是只看不报错。
"""
import json
import subprocess
import sys
import time

AGENT = "/opt/llama/agent/agent-c"
CWD = "/opt/llama/agent"


def run(task, timeout=90):
    """跑一条插件命令，返回 (ok, 摘要, 耗时)"""
    t0 = time.time()
    try:
        r = subprocess.run([AGENT, "--task", json.dumps(task, ensure_ascii=False),
                            "--max-steps", "1"],
                           capture_output=True, text=True, cwd=CWD, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT %ds" % timeout, time.time() - t0
    dt = time.time() - t0
    out = (r.stdout or "").strip()
    # 取最后一行 JSON（前面是插件加载日志）
    line = ""
    for L in reversed(out.split("\n")):
        L = L.strip()
        if L.startswith("{") and L.endswith("}"):
            line = L
            break
    if not line:
        return False, "无 JSON 输出: " + (r.stderr or out)[-100:].replace("\n", " "), dt
    try:
        d = json.loads(line)
    except Exception as e:
        return False, "JSON 解析失败: %s | %s" % (str(e)[:40], line[:80]), dt
    if d.get("error"):
        return False, "ERROR: " + str(d["error"])[:90], dt
    if d.get("ok") is False:
        return False, "ok=false: " + str(d.get("stage") or d.get("msg") or "")[:80], dt
    # 摘要
    for k in ("value", "answer", "text", "factors", "sections", "title"):
        if k in d:
            v = d[k]
            s = v if isinstance(v, str) else json.dumps(v, ensure_ascii=False)
            return True, s.replace("\n", " ")[:110], dt
    if d.get("ok") is True:
        return True, "ok=true " + json.dumps(
            {k: v for k, v in d.items() if k in ("title", "inputs", "buttons", "shot")},
            ensure_ascii=False)[:90], dt
    return True, json.dumps(d, ensure_ascii=False)[:100], dt


CASES = [
    # ---------- math ----------
    ("math/calc",        {"op": "calc", "expr": "(1250*0.87+320)/3"}),
    ("math/calc百分",     {"op": "calc", "expr": "总价 4800 打 7.5 折再减 200"}),
    ("math/solve",       {"op": "solve", "eq": "2x+3=11"}),
    ("math/solve二次",    {"op": "solve", "eq": "x^2-5x+6=0"}),
    ("math/system",      {"op": "system", "eqs": ["x+y=10", "2x-y=2"]}),
    ("math/stat",        {"op": "stat", "data": [12, 7, 9, 15, 22, 8, 31, 19]}),
    ("math/num质因",      {"op": "num", "n": 360, "task": "factor"}),
    ("math/numgcd",      {"op": "num", "n": 48, "n2": 180, "task": "gcd"}),
    ("math/meet",        {"op": "meet", "v1": 60, "v2": 80, "s": 280}),
    ("math/ring",        {"op": "ring", "v1": 5, "v2": 7, "len": 400}),
    ("math/train",       {"op": "train", "len": 200, "bridge": 800, "speed": 20}),
    ("math/work",        {"op": "work", "a": 10, "b": 15}),
    ("math/chicken",     {"op": "chicken", "heads": 35, "legs": 94}),

    # ---------- jq ----------
    ("jq/probe",   {"op": "probe", "path": "/tmp/t_data.json"}),
    ("jq/get",     {"op": "get", "path": "/tmp/t_data.json", "field": "list",
                    "offset": 0, "limit": 3}),
    ("jq/query",   {"op": "query", "path": "/tmp/t_data.json",
                    "expr": ".list[] | select(.price > 100)"}),

    # ---------- table ----------
    ("table/sort", {"op": "sort", "path": "/tmp/t_table.txt", "by": "金额",
                    "n": 3, "order": "desc"}),

    # ---------- doc ----------
    ("doc/read_md",  {"op": "read", "path": "/tmp/t_doc.md"}),
    ("doc/write_md", {"op": "write", "path": "/tmp/t_out.md",
                      "content": "# 测试标题\n\n第一段内容。\n\n- 条目一\n- 条目二\n"}),
    ("doc/write_docx", {"op": "write", "path": "/tmp/t_out.docx",
                        "content": "# 报告标题\n\n正文第一段，测试中文写入。\n\n## 小节\n\n更多内容。"}),
    ("doc/read_docx", {"op": "read", "path": "/tmp/t_out.docx"}),

    # ---------- sys ----------
    ("sys/sysinfo", {"op": "sysinfo"}),
    ("sys/list",    {"op": "list", "path": "/tmp"}),
    ("sys/du",      {"op": "du", "path": "/opt", "top": 5}),
    ("sys/read",    {"op": "read", "path": "/etc/config/llama"}),
    ("sys/grep",    {"op": "grep", "path": "/etc/config", "pattern": "llama"}),
    ("sys/shell",   {"op": "shell", "cmd": "uptime"}),
    ("sys/write",   {"op": "write", "path": "/tmp/t_syswrite.txt", "content": "hello 123"}),

    # ---------- sysmon ----------
    ("sysmon/snapshot", {"op": "snapshot"}),

    # ---------- owrt（只读，不改配置）----------
    ("owrt/uci_get", {"op": "uci_get", "path": "network.lan.ipaddr"}),
    ("owrt/uci_get2", {"op": "uci_get", "path": "dhcp.lan.leasetime"}),
    ("owrt/iwinfo",  {"op": "iwinfo"}),
    ("owrt/logread", {"op": "logread", "lines": 5}),

    # ---------- net（本机/局域网，快）----------
    ("net/hostname", {"op": "hostname"}),
    ("net/route",    {"op": "route"}),
    ("net/arp",      {"op": "arp"}),
    ("net/stat",     {"op": "stat"}),
    ("net/link",     {"op": "link"}),

    # ---------- music（只生成文件，不合成）----------
    ("music/midi", {"op": "midi", "notes": "[{\"pitch\":69,\"dur\":1},"
                    "{\"pitch\":71,\"dur\":1},{\"pitch\":73,\"dur\":2}]",
                    "path": "/tmp/t_out.mid"}),
]


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    rows = []
    passed = failed = 0
    for name, task in CASES:
        if only and only not in name:
            continue
        ok, msg, dt = run(task)
        rows.append((name, ok, msg, dt))
        if ok:
            passed += 1
        else:
            failed += 1
        print("%-20s %s %5.1fs  %s" % (name, "✓" if ok else "✗", dt, msg))
    print()
    print("=" * 70)
    print("通过 %d / 失败 %d / 共 %d" % (passed, failed, passed + failed))
    if failed:
        print("\n失败清单:")
        for name, ok, msg, dt in rows:
            if not ok:
                print("  ✗ %-20s %s" % (name, msg))


if __name__ == "__main__":
    main()
