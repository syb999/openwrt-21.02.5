#!/usr/bin/python3
"""po -> json dictionary for the view.

The view does NOT use LuCI's own catalogue: /cgi-bin/luci/admin/translations/<lang>
is served with Cache-Control: max-age=31536000, so a browser keeps whatever copy
it first got for a year and every string added later shows up in English.  The
backend serves this file over the RPC channel instead (nothing is cached there).
"""
import json
import re
import sys


def unescape(m):
    # the catalogue is written by the same generator that writes the .po, so a
    # simple unescape is enough
    out = []
    i = 0
    while i < len(m):
        c = m[i]
        if c == "\\" and i + 1 < len(m):
            n = m[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    txt = open(src, encoding="utf-8").read()
    pairs = re.findall(r'msgid "((?:[^"\\]|\\.)*)"\s*\nmsgstr "((?:[^"\\]|\\.)*)"', txt)
    d = {}
    for k, v in pairs:
        k, v = unescape(k), unescape(v)
        if k and v:
            d[k] = v
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, sort_keys=True)
    print("po2json: %d entries -> %s" % (len(d), dst))


if __name__ == "__main__":
    import os
    main()
