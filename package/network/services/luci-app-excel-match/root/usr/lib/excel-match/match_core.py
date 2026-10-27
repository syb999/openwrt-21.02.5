"""
Excel 配对引擎（供 LuCI 应用 / 命令行调用）

语义：主表（file1）按「匹配列 match1」去数据表（file2）的「匹配列 match2」找行，
把 file2 的若干个「提取列」搬到主表每一行后面。

重复值的两种处理（dup_mode）：
  pair   : 按出现顺序一一对应（原版行为）——file1 里第 2 个 "A" 配 file2 里第 2 个 "A"
  lookup : 重复值都用第一条（vlookup 语义）

与老脚本相比的修复：
  * 向量化（groupby.cumcount + merge），不再 iterrows()，大表快一个数量级以上
  * 键归一化：去首尾/中间不可见空白、全角→半角、可选忽略大小写、可选数字归一
    （"00123"、"123.0"、"１２３" 都能和 123 对上）
  * 空键不参与匹配（老版里两边都空的会互相配上，串数据）
  * 可选保留主表全部列；可选 sheet / 表头行
  * 列定位支持：列名、1 起始序号、Excel 列字母（A..Z, AA..ZZ）
  * 结果里给出报告：匹配/未匹配数、未匹配样例、重复键、数据表未用到的行
"""

import math
import re
import unicodedata
from collections import Counter

import pandas as pd

__all__ = ["run_match", "read_table", "resolve_column", "MatchError"]


class MatchError(Exception):
    pass


# --------------------------------------------------------------------------
# 键归一化
# --------------------------------------------------------------------------
# 不可见字符：NBSP、全角空格、零宽空格/连接符、BOM、制表/换行、软连字符
_INVISIBLE = dict.fromkeys(map(ord, "\u00a0\u3000\u200b\u200c\u200d\ufeff\t\r\n\u00ad"), None)


def _to_halfwidth(s):
    """全角字符 → 半角（数字、字母、标点、全角空格）。"""
    out = []
    for ch in str(s):
        code = ord(ch)
        if 0xFF01 <= code <= 0xFF5E:            # 全角 ASCII
            out.append(chr(code - 0xFEE0))
        elif code == 0x3000:                     # 全角空格
            out.append(" ")
        else:
            out.append(ch)
    return "".join(out)


def _is_na(v):
    """tuple / 字符串 → False；None / NaN → True。"""
    if v is None:
        return True
    return isinstance(v, float) and math.isnan(v)


def _num_norm(s):
    """把长得像数字的键统一成同一个字符串：去前导零 / 去末尾 .0 / 支持科学计数。"""
    t = s.strip()
    if not t:
        return s
    if not re.fullmatch(r"[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?", t):
        return s
    try:
        f = float(t)
    except ValueError:
        return s
    if math.isnan(f) or math.isinf(f):
        return s
    if f == int(f) and abs(f) < 1e16:
        return str(int(f))
    r = ("%.12f" % f).rstrip("0").rstrip(".")
    return r or s


def normalize_key(value, ignore_case=False, numeric=False, relax=True):
    """
    relax=True  : 去不可见字符 + 全角转半角 + strip（默认开，解决"看着一样却匹配不上"）
    numeric=True: 再对纯数字做归一（00123 → 123，123.0 → 123）
    """
    if value is None:
        return ""
    s = "" if (isinstance(value, float) and math.isnan(value)) else str(value)
    if relax:
        s = s.translate(_INVISIBLE)
        s = unicodedata.normalize("NFKC", s)
        s = _to_halfwidth(s)
        s = s.strip()
        # 内部连续空白压成一个空格（"张 三" vs "张  三"）
        s = re.sub(r"\s+", " ", s)
    if numeric:
        s = _num_norm(s)
    if ignore_case:
        s = s.casefold()
    return s


# --------------------------------------------------------------------------
# 读表 / 列定位
# --------------------------------------------------------------------------
def _sheet_kw(sheet):
    """sheet: None / 名称 / 1 起始序号字符串"""
    if sheet in (None, "", "0", 0):
        return 0
    try:
        return int(sheet) - 1            # 用户看到的 1 是第一张表
    except (TypeError, ValueError):
        return sheet


def read_table(path, sheet=None, header_row=1):
    """
    读成「全部字符串」的 DataFrame：dtype=str + keep_default_na=False，
    避免 123 变成 123.0、空单元格变成 NaN 之后又粘成 'nan'。
    """
    header = max(0, int(header_row or 1) - 1)
    kw = dict(dtype=str, keep_default_na=False, header=header)
    name = str(path).lower()
    try:
        if name.endswith((".xls",)):
            try:
                return pd.read_excel(path, sheet_name=_sheet_kw(sheet), engine="xlrd", **kw)
            except ImportError:
                raise MatchError(
                    "不支持 .xls（旧格式）：这台设备没装 xlrd。"
                    "请用 Excel/WPS 另存为 .xlsx 再上传。")
        return pd.read_excel(path, sheet_name=_sheet_kw(sheet), **kw)
    except MatchError:
        raise
    except Exception as exc:
        raise MatchError("读不了这个 Excel 文件（%s）：%s" % (path, exc))


def _col_letters_to_index(letters):
    """Excel 列字母 → 0 起始索引（A→0, Z→25, AA→26）。"""
    n = 0
    for ch in letters.upper():
        if not ("A" <= ch <= "Z"):
            return None
        n = n * 26 + (ord(ch) - 64)
    return n - 1


def resolve_column(df, spec, which):
    """列名 / 1 起始序号 / Excel 字母 → 真实列名。"""
    raw = "" if spec is None else str(spec).strip()
    if not raw:
        raise MatchError("%s 的列没填" % which)

    cols = [str(c) for c in df.columns]
    if raw in cols:
        return raw

    # Excel 列字母
    if re.fullmatch(r"[A-Za-z]{1,3}", raw):
        idx = _col_letters_to_index(raw)
        if idx is not None and idx < len(cols):
            return cols[idx]

    # 1 起始序号
    if re.fullmatch(r"\d+", raw):
        idx = int(raw) - 1
        if 0 <= idx < len(cols):
            return cols[idx]

    raise MatchError("%s 里没有列 '%s'。可用的列：%s"
                     % (which, raw, "、".join("%d.%s" % (i + 1, c) for i, c in enumerate(cols[:40]))))


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
def run_match(file1, file2, match1, match2, extract,
              sheet1=None, sheet2=None, header1=1, header2=1,
              ignore_case=False, numeric=False, relax=True,
              keep_all_file1=False, dup_mode="pair"):
    """
    extract: 提取列规格列表（名字/序号/字母）；允许为空（那结果就只有匹配列）
    返回 (DataFrame, report dict)
    """
    df1 = read_table(file1, sheet=sheet1, header_row=header1)
    df2 = read_table(file2, sheet=sheet2, header_row=header2)
    if df1.empty:
        raise MatchError("主表没有数据行")
    if df2.empty:
        raise MatchError("数据表没有数据行")

    c1 = resolve_column(df1, match1, "主表")
    c2 = resolve_column(df2, match2, "数据表")

    specs = extract if isinstance(extract, (list, tuple)) else re.split(r"[,，\s]+", str(extract or ""))
    specs = [s for s in specs if str(s).strip()]
    ex_cols, seen = [], set()
    for spec in specs:
        name = resolve_column(df2, spec, "数据表")
        if name not in seen:
            seen.add(name)
            ex_cols.append(name)

    # 结果列名：与主表已有列冲突时加后缀
    used = set(str(c) for c in df1.columns) if keep_all_file1 else {c1}
    out_names = {}
    for name in ex_cols:
        new = name
        if new in used:
            new = "%s_数据表" % name
            k = 2
            while new in used:
                new = "%s_数据表%d" % (name, k)
                k += 1
        used.add(new)
        out_names[name] = new

    # ---- 键归一化 ----
    raw1 = df1[c1].map(lambda v: normalize_key(v, ignore_case, numeric, relax))
    raw2 = df2[c2].map(lambda v: normalize_key(v, ignore_case, numeric, relax))
    e1 = raw1 == ""
    e2 = raw2 == ""

    # 🔴 空键不能置成 None/NaN 交给 merge：pandas 的左连接遇到 NaN 键会**丢掉整行**
    #    （实测：主表最后那行空编号直接消失，结果少一行）。用一个每行唯一的哨兵字符串，
    #    既不会被匹配上，也不会丢行。
    k1 = raw1.mask(e1, pd.Series(["\x00empty1_%d" % i for i in range(len(raw1))], index=raw1.index))
    k2 = raw2.mask(e2, pd.Series(["\x00empty2_%d" % i for i in range(len(raw2))], index=raw2.index))

    d1 = pd.DataFrame({"_k": k1, "_occ": k1.groupby(k1).cumcount()})
    d1["_row"] = range(len(d1))

    # 提取列留空时 df2[[]] 每行是空 tuple，直接塞进 DataFrame 会
    #   ValueError: array length 0 does not match index length N
    val_series = (list(df2[ex_cols].itertuples(index=False, name=None))
                  if ex_cols else [1] * len(df2))

    if dup_mode == "lookup":
        take = pd.DataFrame({"_k": k2, "_occ": 0, "_vals": val_series})
        take = take[take["_k"].notna()].drop_duplicates("_k", keep="first")
        merged = d1.merge(take, on="_k", how="left")
    else:
        take = pd.DataFrame({"_k": k2,
                             "_occ": k2.groupby(k2).cumcount(),
                             "_vals": val_series})
        take = take[take["_k"].notna()]
        merged = d1.merge(take, on=["_k", "_occ"], how="left")

    merged = merged.sort_values("_row")

    # ---- 组装结果 ----
    if keep_all_file1:
        res = df1.reset_index(drop=True).copy()
    else:
        res = df1[[c1]].reset_index(drop=True).copy()
        res.columns = [c1]

    for name in ex_cols:
        res[out_names[name]] = ""

    vals = merged["_vals"].tolist()
    for i, row_vals in enumerate(vals):
        if _is_na(row_vals) or not ex_cols:
            continue
        for j, name in enumerate(ex_cols):
            res.at[i, out_names[name]] = row_vals[j] if row_vals[j] is not None else ""

    # ---- 报告 ----
    # 🔴 left join 之后「键出现」不等于「配上了」：没配上的行 _vals 是 NaN。
    #    这里必须按 _vals 是否有值来判定，否则未匹配的行会被算成匹配（实测踩过）。
    hit = [not _is_na(v) for v in vals]
    matched = sum(hit)
    un1 = []
    for k, h in zip(merged["_k"].tolist(), hit):
        if (not h) and isinstance(k, str) and not k.startswith("\x00empty") and k not in un1:
            un1.append(k)
    dup2 = {k: n for k, n in Counter(k2.dropna()).items() if n > 1}

    unused2 = 0
    if dup_mode == "pair":
        need = Counter()
        for k, occ, h in zip(merged["_k"], merged["_occ"], hit):
            if h and k is not None and not (isinstance(occ, float) and math.isnan(occ)):
                need[k] += 1
        for k, n in Counter(k2.dropna()).items():
            unused2 += max(0, n - need.get(k, 0))

    # ---- 未匹配的"近似原因"提示：换个归一化就能配上的，提示用户调选项 ----
    hint = {}
    if un1:
        un1_set = set(un1)
        k2_exact = set(k2.dropna().tolist())            # 默认归一化下就已经能配上的键
        for label, kw in (("数字归一", dict(numeric=True)),
                          ("忽略大小写", dict(ignore_case=True))):
            alt2 = set(x for x in (normalize_key(v, **kw) for v in df2[c2].tolist()) if x)
            # 「当前配不上」且「换了归一键就能配上」的键
            alt1 = [k for k in un1_set
                    if k not in k2_exact and normalize_key(k, **kw) in alt2]
            if alt1:
                hint[label] = {"count": len(alt1),
                               "samples": [str(x) for x in alt1[:5]]}

    report = {
        "hints": hint,
        "rows_file1": int(len(df1)),
        "rows_file2": int(len(df2)),
        "matched": int(matched),
        "unmatched": int(len(df1) - matched),
        "empty_key_file1": int(e1.sum()),
        "empty_key_file2": int(e2.sum()),
        "duplicate_keys_file2": dict(list(dup2.items())[:10]),
        "duplicate_key_count": len(dup2),
        "unmatched_keys": [str(x) for x in un1[:10]],
        "unmatched_key_count": len(un1),
        "unused_rows_file2": int(unused2),
        "match1": c1, "match2": c2,
        "extract": ex_cols, "out_names": [out_names[c] for c in ex_cols],
        "dup_mode": dup_mode,
    }
    return res, report


# --------------------------------------------------------------------------
# 命令行：便于离线验证 / 以后给桌面 GUI 复用
# --------------------------------------------------------------------------
def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Excel 配对（命令行版）")
    ap.add_argument("file1"); ap.add_argument("file2")
    ap.add_argument("--match1", required=True); ap.add_argument("--match2", required=True)
    ap.add_argument("--extract", default="")
    ap.add_argument("--out", default="match_result.xlsx")
    ap.add_argument("--sheet1", default=None); ap.add_argument("--sheet2", default=None)
    ap.add_argument("--header1", type=int, default=1); ap.add_argument("--header2", type=int, default=1)
    ap.add_argument("--ignore-case", action="store_true"); ap.add_argument("--numeric", action="store_true")
    ap.add_argument("--no-relax", action="store_true")
    ap.add_argument("--keep-all", action="store_true")
    ap.add_argument("--dup-mode", choices=["pair", "lookup"], default="pair")
    a = ap.parse_args(argv)
    res, rep = run_match(a.file1, a.file2, a.match1, a.match2, a.extract,
                         sheet1=a.sheet1, sheet2=a.sheet2, header1=a.header1, header2=a.header2,
                         ignore_case=a.ignore_case, numeric=a.numeric, relax=not a.no_relax,
                         keep_all_file1=a.keep_all, dup_mode=a.dup_mode)
    res.to_excel(a.out, index=False)
    import json
    print(json.dumps(rep, ensure_ascii=False, indent=2))
    print("写出:", a.out)
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_main(sys.argv[1:]))
