#!/usr/bin/env python3
# 板上跑：验证插件命令直连（不该走模型）
import json, subprocess, sys

CASES = [
    ('{"op":"probe","url":"https://www.goofish.com/"}', "browser", "probe"),
    ('{"op":"snapshot"}', "sysmon", "snapshot"),
    ('{"op":"calc","expr":"1+1"}', "math", "calc"),
    ('{"op":"ping","host":"10.80.1.1","count":2}', "net", "ping"),
    ('{"op":"sysinfo"}', "sys", "sysinfo"),
]
for task, plug, op in CASES:
    r = subprocess.run(["./agent-c", "--task", task, "--max-steps", "1"],
                       capture_output=True, text=True, cwd="/opt/llama/agent", timeout=120)
    ok = '"ok": true' in r.stdout or '"value"' in r.stdout or '"answer"' in r.stdout
    took_model = "最终回答" in r.stdout
    print("%-46s -> %-8s %s  走模型=%s" % (task[:44], plug,
          "OK" if ok else "FAIL", "是(不该)" if took_model else "否(对)"))
