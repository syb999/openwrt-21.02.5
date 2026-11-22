#!/usr/bin/env python3
# 板上：用 NPU 的 ppocr 测同一批验证码，跟 tesseract 对比
import glob
import os
import re
import subprocess
import time

print("%-16s %-6s %-10s %s" % ("文件", "答案", "ppocr", "结果"))
print("-" * 60)
summary = {}
for level, tag in ((0, "干净"), (1, "轻微"), (2, "强化")):
    files = sorted(glob.glob("/tmp/captcha/L%d_*.png" % level))
    if not files:
        print("（%s 没有图）" % tag)
        continue
    hit = 0
    t0 = time.time()
    for f in files:
        ans = re.search(r"_([0-9a-z]{4})\.png$", f).group(1)
        try:
            r = subprocess.run(["ppocr", f], capture_output=True, text=True, timeout=30)
            raw = (r.stdout or "").strip()
        except Exception as e:
            raw = ""
            print("  %-14s 异常 %s" % (os.path.basename(f)[:13], str(e)[:30]))
        got = re.sub(r"[^0-9A-Za-z]", "", raw).lower()
        if got == ans:
            hit += 1
        print("  %-14s %-6s %-10s %s" % (os.path.basename(f)[:13], ans,
                                         got or "(空)", "✓" if got == ans else ""))
    dt = time.time() - t0
    summary[tag] = (hit, len(files), dt / max(1, len(files)))
    print()

print("=" * 60)
for tag, (hit, n, per) in summary.items():
    print("%-6s %d/%d = %3.0f%%   平均 %.2fs/张" % (
        tag, hit, n, 100.0 * hit / max(1, n), per))
