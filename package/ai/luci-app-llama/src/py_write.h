/* 自动生成：docwrite.py 内嵌到这里，doc.so 不依赖外部 .py */
#ifndef PY_WRITE_SRC
#define PY_WRITE_SRC \
    "#!/usr/bin/env python3\n" \
    "\"\"\"docwrite.py — 文档写入/生成引擎（由 doc 插件调用）\n" \
    "\n" \
    "用法：  python3 docwrite.py <op>      数据从 stdin 读（JSON）\n" \
    "        stdout 输出 JSON 结果\n" \
    "\n" \
    "支持的 op：\n" \
    "  text       {\"path\",\"content\"}                     纯文本 / md / csv / json\n" \
    "  xlsx       {\"path\",\"sheets\":[{\"name\",\"rows\":[[...]]}]}         新建\n" \
    "  xlsx_edit  {\"path\",\"sheet\",\"cell\",\"value\"}                     改单元格\n" \
    "  xlsx_read  {\"path\",\"sheet\"}                                    读回来验证\n" \
    "  docx       {\"path\",\"blocks\":[{\"type\":\"h|p|li\",\"text\",\"level\"}]} 新建\n" \
    "  docx_append{\"path\",\"blocks\":[...]}                             追加\n" \
    "  pptx       {\"path\",\"slides\":[{\"title\",\"bullets\":[...]}]}        新建\n" \
    "  pdf        {\"path\",\"blocks\":[...],\"font_size\"}                  生成（PIL 画图 + .txt 伴生）\n" \
    "\n" \
    "设计说明：\n" \
    "  - 板上没有 reportlab/fpdf，PDF 只能用 PIL 画成图片页。这样中文能正常显示，\n" \
    "    但图片里的字不可选、不可搜索 —— 所以同时写一份同名 .txt，\n" \
    "    读取时优先用 .txt，保证「写了能读回来」。\n" \
    "  - 所有写操作都做基本校验（路径、扩展名匹配），写完立刻回读确认。\n" \
    "\"\"\"\n" \
    "import sys\n" \
    "import os\n" \
    "import json\n" \
    "import io\n" \
    "import datetime\n" \
    "\n" \
    "OUT = {}\n" \
    "\n" \
    "\n" \
    "def out(ok=True, **kw):\n" \
    "    d = {\"ok\": 1 if ok else 0}\n" \
    "    d.update(kw)\n" \
    "    # answer：一句话结论。模型有时拿到结果不知道该说什么就卡住，\n" \
    "    # 框架会用这个字段收尾，别让活白干。\n" \
    "    if ok and \"answer\" not in d and d.get(\"path\"):\n" \
    "        fmt = d.get(\"format\") or \"文件\"\n" \
    "        extra = \"\"\n" \
    "        if fmt == \"xlsx\":\n" \
    "            extra = \"，%d 行\" % d.get(\"rows\", 0)\n" \
    "        elif fmt == \"docx\":\n" \
    "            extra = \"，%d 段\" % d.get(\"paragraphs\", 0)\n" \
    "        elif fmt == \"pptx\":\n" \
    "            extra = \"，%d 页\" % d.get(\"slides\", 0)\n" \
    "        elif fmt == \"pdf\":\n" \
    "            extra = \"，%d 页\" % d.get(\"pages\", 0)\n" \
    "        elif fmt == \"text\":\n" \
    "            extra = \"，%d 字\" % d.get(\"chars\", 0)\n" \
    "        d[\"answer\"] = \"已生成 %s%s：%s\" % (fmt, extra, os.path.basename(d[\"path\"]))\n" \
    "    print(json.dumps(d, ensure_ascii=False))\n" \
    "    sys.exit(0 if ok else 1)\n" \
    "\n" \
    "\n" \
    "def fail(msg, hint=\"\"):\n" \
    "    d = {\"ok\": 0, \"error\": msg}\n" \
    "    if hint:\n" \
    "        d[\"hint\"] = hint\n" \
    "    print(json.dumps(d, ensure_ascii=False))\n" \
    "    sys.exit(1)\n" \
    "\n" \
    "\n" \
    "def ensure_dir(path):\n" \
    "    d = os.path.dirname(os.path.abspath(path))\n" \
    "    if d and not os.path.isdir(d):\n" \
    "        try:\n" \
    "            os.makedirs(d)\n" \
    "        except Exception as e:\n" \
    "            fail(\"建目录失败: %s\" % e)\n" \
    "\n" \
    "\n" \
    "def check_ext(path, allowed):\n" \
    "    ext = os.path.splitext(path)[1].lower()\n" \
    "    if ext not in allowed:\n" \
    "        fail(\"扩展名 %s 与操作不符（应为 %s）\" % (ext, \"/\".join(allowed)))\n" \
    "\n" \
    "\n" \
    "# ---------------- 纯文本 ----------------\n" \
    "def do_text(req):\n" \
    "    path = req[\"path\"]\n" \
    "    content = req.get(\"content\", \"\")\n" \
    "    ensure_dir(path)\n" \
    "    with open(path, \"w\", encoding=\"utf-8\") as f:\n" \
    "        f.write(content)\n" \
    "    # 回读验证\n" \
    "    with open(path, \"r\", encoding=\"utf-8\") as f:\n" \
    "        back = f.read()\n" \
    "    if back != content:\n" \
    "        fail(\"写入后回读不一致（文件系统问题？）\")\n" \
    "    out(path=path, chars=len(back), lines=back.count(\"\\n\") + 1,\n" \
    "        format=\"text\", verified=True)\n" \
    "\n" \
    "\n" \
    "# ---------------- 文本 → 结构（3B 生成嵌套 JSON 很容易坏括号，\n" \
    "#                  让它写纯文本更靠谱，这里负责解析） ----------------\n" \
    "def md_to_blocks(md):\n" \
    "    blocks = []\n" \
    "    for raw in str(md).split(\"\\n\"):\n" \
    "        s = raw.rstrip()\n" \
    "        if not s.strip():\n" \
    "            continue\n" \
    "        if s.startswith(\"### \"):\n" \
    "            blocks.append({\"type\": \"h\", \"level\": 3, \"text\": s[4:].strip()})\n" \
    "        elif s.startswith(\"## \"):\n" \
    "            blocks.append({\"type\": \"h\", \"level\": 2, \"text\": s[3:].strip()})\n" \
    "        elif s.startswith(\"# \"):\n" \
    "            blocks.append({\"type\": \"h\", \"level\": 1, \"text\": s[2:].strip()})\n" \
    "        elif s.lstrip().startswith((\"- \", \"* \", \"• \")):\n" \
    "            blocks.append({\"type\": \"li\", \"text\": s.lstrip()[2:].strip()})\n" \
    "        else:\n" \
    "            blocks.append({\"type\": \"p\", \"text\": s.strip()})\n" \
    "    return blocks\n" \
    "\n" \
    "\n" \
    "def csv_to_rows(text):\n" \
    "    import csv\n" \
    "    import io as _io\n" \
    "    rows = []\n" \
    "    for row in csv.reader(_io.StringIO(str(text))):\n" \
    "        if not row or all(c.strip() == \"\" for c in row):\n" \
    "            continue\n" \
    "        # 纯数字转成数字，Excel 里才能求和\n" \
    "        conv = []\n" \
    "        for c in row:\n" \
    "            t = c.strip()\n" \
    "            try:\n" \
    "                conv.append(int(t))\n" \
    "            except ValueError:\n" \
    "                try:\n" \
    "                    conv.append(float(t))\n" \
    "                except ValueError:\n" \
    "                    conv.append(t)\n" \
    "        rows.append(conv)\n" \
    "    return rows\n" \
    "\n" \
    "\n" \
    "def md_to_slides(md):\n" \
    "    \"\"\"每个 # 开头的块算一页，块内的 - 行算要点。\"\"\"\n" \
    "    slides = []\n" \
    "    cur = None\n" \
    "    for raw in str(md).split(\"\\n\"):\n" \
    "        s = raw.strip()\n" \
    "        if not s:\n" \
    "            continue\n" \
    "        if s.startswith(\"#\"):\n" \
    "            if cur:\n" \
    "                slides.append(cur)\n" \
    "            cur = {\"title\": s.lstrip(\"# \").strip(), \"bullets\": []}\n" \
    "        elif s.lstrip().startswith((\"- \", \"* \", \"• \")):\n" \
    "            if cur is None:\n" \
    "                cur = {\"title\": \"\", \"bullets\": []}\n" \
    "            cur[\"bullets\"].append(s.lstrip()[2:].strip())\n" \
    "        else:\n" \
    "            if cur is None:\n" \
    "                cur = {\"title\": s, \"bullets\": []}\n" \
    "            else:\n" \
    "                cur[\"bullets\"].append(s)\n" \
    "    if cur:\n" \
    "        slides.append(cur)\n" \
    "    return slides\n" \
    "\n" \
    "\n" \
    "# ---------------- xlsx ----------------\n" \
    "def _require(mod, pipname):\n" \
    "    try:\n" \
    "        return __import__(mod)\n" \
    "    except ImportError:\n" \
    "        fail(\"缺少 Python 库 %s\" % pipname, \"板上装：pip install %s\" % pipname)\n" \
    "\n" \
    "\n" \
    "def do_xlsx(req):\n" \
    "    openpyxl = _require(\"openpyxl\", \"openpyxl\")\n" \
    "    path = req[\"path\"]\n" \
    "    check_ext(path, [\".xlsx\"])\n" \
    "    sheets = req.get(\"sheets\")\n" \
    "    # 从多个 CSV 文件生成多 sheet（jq 导出的主表 + 各子表）\n" \
    "    if not sheets and req.get(\"csv_files\"):\n" \
    "        import csv as _csvmod\n" \
    "        sheets = []\n" \
    "        for item in req[\"csv_files\"]:\n" \
    "            if isinstance(item, dict):\n" \
    "                fp = item.get(\"path\")\n" \
    "                nm = item.get(\"name\")\n" \
    "            else:\n" \
    "                fp = item\n" \
    "                nm = None\n" \
    "            rows = []\n" \
    "            if fp and os.path.exists(fp):\n" \
    "                with open(fp, encoding=\"utf-8\", newline=\"\") as fh:\n" \
    "                    rows = [r for r in _csvmod.reader(fh)]\n" \
    "            if not nm:\n" \
    "                nm = os.path.splitext(os.path.basename(fp or \"Sheet\"))[0]\n" \
    "            # Excel 表名上限 31 字符，非法字符要换掉\n" \
    "            nm = str(nm)[:31]\n" \
    "            for ch in '[]:*?/\\\\':\n" \
    "                nm = nm.replace(ch, \"_\")\n" \
    "            sheets.append({\"name\": nm or \"Sheet\", \"rows\": rows})\n" \
    "    if not sheets and req.get(\"csv\"):\n" \
    "        # 模型写 CSV 比写嵌套数组稳得多\n" \
    "        sheets = [{\"name\": req.get(\"sheet_name\") or \"Sheet1\",\n" \
    "                   \"rows\": csv_to_rows(req[\"csv\"])}]\n" \
    "    if not sheets:\n" \
    "        sheets = [{\"name\": \"Sheet1\", \"rows\": []}]\n" \
    "    ensure_dir(path)\n" \
    "\n" \
    "    wb = openpyxl.Workbook()\n" \
    "    wb.remove(wb.active)\n" \
    "    for sh in sheets:\n" \
    "        ws = wb.create_sheet(sh.get(\"name\") or \"Sheet1\")\n" \
    "        rows = sh.get(\"rows\") or []\n" \
    "        for r in rows:\n" \
    "            ws.append(list(r))\n" \
    "        # 表头加粗 + 列宽自适应（纯展示体验，不影响数据）\n" \
    "        if rows:\n" \
    "            for c in ws[1]:\n" \
    "                c.font = openpyxl.styles.Font(bold=True)\n" \
    "        for col in range(1, (max(len(r) for r in rows) if rows else 0) + 1):\n" \
    "            w = 10\n" \
    "            for r in rows:\n" \
    "                if col <= len(r) and r[col - 1] is not None:\n" \
    "                    w = max(w, min(len(str(r[col - 1])) * 2 + 4, 40))\n" \
    "            ws.column_dimensions[openpyxl.utils.get_column_letter(col)].width = w\n" \
    "    wb.save(path)\n" \
    "\n" \
    "    # 回读验证\n" \
    "    wb2 = openpyxl.load_workbook(path)\n" \
    "    total = 0\n" \
    "    names = []\n" \
    "    for ws in wb2.worksheets:\n" \
    "        names.append(ws.title)\n" \
    "        total += ws.max_row\n" \
    "    out(path=path, format=\"xlsx\", sheets=names, rows=total, verified=True,\n" \
    "        size=os.path.getsize(path))\n" \
    "\n" \
    "\n" \
    "def do_xlsx_edit(req):\n" \
    "    openpyxl = _require(\"openpyxl\", \"openpyxl\")\n" \
    "    path = req[\"path\"]\n" \
    "    if not os.path.exists(path):\n" \
    "        fail(\"文件不存在: %s\" % path)\n" \
    "    check_ext(path, [\".xlsx\"])\n" \
    "    wb = openpyxl.load_workbook(path)\n" \
    "    sheet = req.get(\"sheet\")\n" \
    "    ws = wb[sheet] if sheet and sheet in wb.sheetnames else wb.worksheets[0]\n" \
    "    cell = req[\"cell\"]\n" \
    "    value = req.get(\"value\")\n" \
    "    # 数字就存成数字，别一律当字符串（不然 Excel 里没法求和）\n" \
    "    try:\n" \
    "        if isinstance(value, str) and value.strip() and \\\n" \
    "                value.strip().replace(\".\", \"\", 1).replace(\"-\", \"\", 1).isdigit():\n" \
    "            value = float(value) if \".\" in value else int(value)\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "    ws[cell] = value\n" \
    "    wb.save(path)\n" \
    "\n" \
    "    wb2 = openpyxl.load_workbook(path)\n" \
    "    got = wb2[ws.title][cell].value\n" \
    "    out(path=path, format=\"xlsx\", sheet=ws.title, cell=cell, value=got,\n" \
    "        verified=(str(got) == str(value)))\n" \
    "\n" \
    "\n" \
    "def do_xlsx_read(req):\n" \
    "    openpyxl = _require(\"openpyxl\", \"openpyxl\")\n" \
    "    path = req[\"path\"]\n" \
    "    wb = openpyxl.load_workbook(path, data_only=True)\n" \
    "    lines = []\n" \
    "    for ws in wb.worksheets:\n" \
    "        lines.append(\"## %s\" % ws.title)\n" \
    "        for row in ws.iter_rows(values_only=True):\n" \
    "            if all(c is None for c in row):\n" \
    "                continue\n" \
    "            lines.append(\" | \".join(\"\" if c is None else str(c) for c in row))\n" \
    "    text = \"\\n\".join(lines)\n" \
    "    out(path=path, format=\"xlsx\", chars=len(text), text=text[:4000])\n" \
    "\n" \
    "\n" \
    "# ---------------- docx ----------------\n" \
    "def _add_blocks(doc, blocks):\n" \
    "    for b in blocks:\n" \
    "        t = b.get(\"type\") or \"p\"\n" \
    "        txt = b.get(\"text\") or \"\"\n" \
    "        if t == \"h\":\n" \
    "            doc.add_heading(txt, level=int(b.get(\"level\") or 1))\n" \
    "        elif t == \"li\":\n" \
    "            doc.add_paragraph(txt, style=\"List Bullet\")\n" \
    "        elif t == \"table\":\n" \
    "            rows = b.get(\"rows\") or []\n" \
    "            if rows:\n" \
    "                tb = doc.add_table(rows=len(rows), cols=len(rows[0]))\n" \
    "                tb.style = \"Table Grid\"\n" \
    "                for i, r in enumerate(rows):\n" \
    "                    for j, c in enumerate(r):\n" \
    "                        tb.cell(i, j).text = \"\" if c is None else str(c)\n" \
    "        else:\n" \
    "            doc.add_paragraph(txt)\n" \
    "\n" \
    "\n" \
    "def do_docx(req):\n" \
    "    docx_mod = _require(\"docx\", \"python-docx\")\n" \
    "    path = req[\"path\"]\n" \
    "    check_ext(path, [\".docx\"])\n" \
    "    ensure_dir(path)\n" \
    "    blocks = req.get(\"blocks\")\n" \
    "    if not blocks and req.get(\"markdown\"):\n" \
    "        blocks = md_to_blocks(req[\"markdown\"])\n" \
    "    doc = docx_mod.Document()\n" \
    "    _add_blocks(doc, blocks or [])\n" \
    "    doc.save(path)\n" \
    "\n" \
    "    d2 = docx_mod.Document(path)\n" \
    "    n = len(d2.paragraphs) + len(d2.tables)\n" \
    "    out(path=path, format=\"docx\", paragraphs=len(d2.paragraphs),\n" \
    "        tables=len(d2.tables), blocks=n, verified=True,\n" \
    "        size=os.path.getsize(path))\n" \
    "\n" \
    "\n" \
    "def do_docx_append(req):\n" \
    "    docx_mod = _require(\"docx\", \"python-docx\")\n" \
    "    path = req[\"path\"]\n" \
    "    if not os.path.exists(path):\n" \
    "        return do_docx(req)\n" \
    "    check_ext(path, [\".docx\"])\n" \
    "    blocks = req.get(\"blocks\")\n" \
    "    if not blocks and req.get(\"markdown\"):\n" \
    "        blocks = md_to_blocks(req[\"markdown\"])\n" \
    "    doc = docx_mod.Document(path)\n" \
    "    _add_blocks(doc, blocks or [])\n" \
    "    doc.save(path)\n" \
    "    d2 = docx_mod.Document(path)\n" \
    "    out(path=path, format=\"docx\", paragraphs=len(d2.paragraphs),\n" \
    "        tables=len(d2.tables), appended=True, verified=True)\n" \
    "\n" \
    "\n" \
    "# ---------------- pptx（5 套主题 × 8 种版式） ----------------\n" \
    "# 配色照搬 Python 版 pptx.js 的 THEMES（那套是看过效果定下来的）：\n" \
    "# 每次随内容挑一套，避免\"所有 PPT 都是同一个蓝色模板\"。\n" \
    "THEMES = [\n" \
    "    {\"name\": \"商务深蓝\", \"accents\": [\"2F5597\", \"4472C4\", \"5B9BD5\", \"8FAADC\"],\n" \
    "     \"bg\": \"FFFFFF\", \"text\": \"1F2A37\", \"soft\": \"F2F5FA\", \"muted\": \"7F8C9A\"},\n" \
    "    {\"name\": \"科技青\", \"accents\": [\"0E7C86\", \"17A2B8\", \"38BDF8\", \"7DD3FC\"],\n" \
    "     \"bg\": \"FFFFFF\", \"text\": \"12333A\", \"soft\": \"EAF6F7\", \"muted\": \"6B8A90\"},\n" \
    "    {\"name\": \"暖橙\", \"accents\": [\"C55A11\", \"ED7D31\", \"F59E0B\", \"FCD34D\"],\n" \
    "     \"bg\": \"FFFCF7\", \"text\": \"33241A\", \"soft\": \"FDEFE3\", \"muted\": \"8A7364\"},\n" \
    "    {\"name\": \"墨紫\", \"accents\": [\"5B2C83\", \"9B59B6\", \"A78BFA\", \"DDD6FE\"],\n" \
    "     \"bg\": \"FFFFFF\", \"text\": \"2A1B36\", \"soft\": \"F4EDF9\", \"muted\": \"82709A\"},\n" \
    "    {\"name\": \"暗夜\", \"accents\": [\"14B8A6\", \"2DD4BF\", \"22D3EE\", \"67E8F9\"],\n" \
    "     \"bg\": \"111827\", \"text\": \"F8FAFC\", \"soft\": \"1F2937\", \"muted\": \"94A3B8\"},\n" \
    "]\n" \
    "FONT = \"Microsoft YaHei\"\n" \
    "\n" \
    "\n" \
    "def _pick_theme(spec, want):\n" \
    "    \"\"\"指定就按名字找；否则用内容做种子 —— 同一份内容永远同一套配色，\n" \
    "    但不同内容会换，避免\"每次都是蓝色\"。\"\"\"\n" \
    "    if want:\n" \
    "        for t in THEMES:\n" \
    "            if want == t[\"name\"]:\n" \
    "                return t\n" \
    "        for i, t in enumerate(THEMES):\n" \
    "            if want == \"theme%d\" % i:\n" \
    "                return t\n" \
    "    seed = 0\n" \
    "    for s2 in (spec.get(\"slides\") or [])[:3]:\n" \
    "        for ch in str(s2.get(\"title\") or \"\"):\n" \
    "            seed = (seed * 31 + ord(ch)) & 0xFFFF\n" \
    "    return THEMES[seed % len(THEMES)]\n" \
    "\n" \
    "\n" \
    "def _rgb(pptx_mod, hexstr):\n" \
    "    return pptx_mod.dml.color.RGBColor.from_string(hexstr)\n" \
    "\n" \
    "\n" \
    "def _blank(prs):\n" \
    "    return prs.slides.add_slide(prs.slide_layouts[6])   # 6 = 完全空白，自己画\n" \
    "\n" \
    "\n" \
    "def _bg(slide, pptx_mod, color):\n" \
    "    fill = slide.background.fill\n" \
    "    fill.solid()\n" \
    "    fill.fore_color.rgb = _rgb(pptx_mod, color)\n" \
    "\n" \
    "\n" \
    "def _rect(slide, pptx_mod, x, y, w, h, color):\n" \
    "    from pptx.util import Inches\n" \
    "    from pptx.enum.shapes import MSO_SHAPE\n" \
    "    sh = slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(x), Inches(y), Inches(w), Inches(h))\n" \
    "    sh.fill.solid()\n" \
    "    sh.fill.fore_color.rgb = _rgb(pptx_mod, color)\n" \
    "    sh.line.fill.background()\n" \
    "    sh.shadow.inherit = False\n" \
    "    return sh\n" \
    "\n" \
    "\n" \
    "def _alpha(shape, pct):\n" \
    "    \"\"\"给纯色填充加透明度。python-pptx 没有直接 API，得往 XML 里塞 <a:alpha>。\"\"\"\n" \
    "    from pptx.oxml.ns import qn\n" \
    "    try:\n" \
    "        sf = shape.fill._xPr.find(qn(\"a:solidFill\"))\n" \
    "        if sf is None:\n" \
    "            return\n" \
    "        clr = sf.find(qn(\"a:srgbClr\"))\n" \
    "        if clr is None:\n" \
    "            return\n" \
    "        for old in clr.findall(qn(\"a:alpha\")):\n" \
    "            clr.remove(old)\n" \
    "        clr.append(clr.makeelement(qn(\"a:alpha\"), {\"val\": str(int(pct * 1000))}))\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "\n" \
    "\n" \
    "def _circle(slide, pptx_mod, x, y, d, color, alpha=None):\n" \
    "    \"\"\"装饰圆。坐标允许为负/超出边界 —— PPT 会自动裁掉，正好做出\"半截构图\"。\"\"\"\n" \
    "    from pptx.util import Inches\n" \
    "    from pptx.enum.shapes import MSO_SHAPE\n" \
    "    sh = slide.shapes.add_shape(MSO_SHAPE.OVAL, Inches(x), Inches(y), Inches(d), Inches(d))\n" \
    "    sh.fill.solid()\n" \
    "    sh.fill.fore_color.rgb = _rgb(pptx_mod, color)\n" \
    "    sh.line.fill.background()\n" \
    "    sh.shadow.inherit = False\n" \
    "    if alpha:\n" \
    "        _alpha(sh, alpha)\n" \
    "    return sh\n" \
    "\n" \
    "\n" \
    "def _badge(slide, pptx_mod, x, y, d, color, txt):\n" \
    "    \"\"\"圆形编号徽章：要点列表用它才有了\"章节感\"。\"\"\"\n" \
    "    _circle(slide, pptx_mod, x, y, d, color)\n" \
    "    _text(slide, pptx_mod, x, y + d * 0.16, d, d * 0.7, txt, d * 26, \"FFFFFF\", True, \"center\")\n" \
    "\n" \
    "\n" \
    "def _text(slide, pptx_mod, x, y, w, h, txt, size, color, bold=False, align=None):\n" \
    "    from pptx.util import Inches, Pt\n" \
    "    from pptx.enum.text import PP_ALIGN\n" \
    "    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))\n" \
    "    tf = tb.text_frame\n" \
    "    tf.word_wrap = True\n" \
    "    p = tf.paragraphs[0]\n" \
    "    p.text = str(txt)\n" \
    "    p.font.size = Pt(size)\n" \
    "    p.font.bold = bold\n" \
    "    p.font.name = FONT\n" \
    "    p.font.color.rgb = _rgb(pptx_mod, color)\n" \
    "    if align == \"center\":\n" \
    "        p.alignment = PP_ALIGN.CENTER\n" \
    "    elif align == \"right\":\n" \
    "        p.alignment = PP_ALIGN.RIGHT\n" \
    "    return tb\n" \
    "\n" \
    "\n" \
    "# ---- 版式：每页都带装饰层，不是白底一堆字 ----\n" \
    "def lay_cover(prs, pptx_mod, th, spec, idx, total):\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"accents\"][0])\n" \
    "    # 右侧两个半透明大圆 + 左侧竖色条：做出\"封面\"的视觉重量\n" \
    "    _circle(sl, pptx_mod, 9.0, 3.2, 6.0, th[\"accents\"][2], 18)\n" \
    "    _circle(sl, pptx_mod, 10.9, 0.6, 3.4, th[\"accents\"][3], 12)\n" \
    "    _rect(sl, pptx_mod, 0, 0, 0.24, 7.5, th[\"accents\"][2])\n" \
    "    _text(sl, pptx_mod, 0.98, 1.95, 9.6, 2.3, spec.get(\"title\", \"\"), 46, \"FFFFFF\", True)\n" \
    "    _rect(sl, pptx_mod, 1.0, 4.5, 2.4, 0.10, th[\"accents\"][3])\n" \
    "    if spec.get(\"subtitle\"):\n" \
    "        _text(sl, pptx_mod, 1.0, 4.88, 8.6, 1.0, spec[\"subtitle\"], 20, th[\"accents\"][3])\n" \
    "    _text(sl, pptx_mod, 0.98, 6.9, 11.6, 0.4, th[\"name\"], 11, th[\"accents\"][3], align=\"right\")\n" \
    "\n" \
    "\n" \
    "def lay_section(prs, pptx_mod, th, spec, idx, total):\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"soft\"])\n" \
    "    _circle(sl, pptx_mod, -1.4, 4.2, 4.6, th[\"accents\"][3], 34)     # 左下大圆\n" \
    "    _circle(sl, pptx_mod, 11.4, -1.0, 3.2, th[\"accents\"][2], 22)    # 右上小圆\n" \
    "    _text(sl, pptx_mod, 0.95, 1.95, 3.6, 1.9, \"%02d\" % idx, 92, th[\"accents\"][1], True)\n" \
    "    _rect(sl, pptx_mod, 1.0, 3.95, 1.7, 0.08, th[\"accents\"][1])\n" \
    "    _text(sl, pptx_mod, 0.95, 4.22, 11.0, 1.4, spec.get(\"title\", \"\"), 34, th[\"text\"], True)\n" \
    "\n" \
    "\n" \
    "def lay_bullets(prs, pptx_mod, th, spec, idx, total):\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"bg\"])\n" \
    "    _circle(sl, pptx_mod, 11.3, -1.2, 3.6, th[\"accents\"][3], 16)    # 右上装饰\n" \
    "    _rect(sl, pptx_mod, 0, 0, 13.333, 0.09, th[\"accents\"][0])\n" \
    "    _rect(sl, pptx_mod, 0.85, 0.50, 0.10, 0.78, th[\"accents\"][1])   # 标题左竖条\n" \
    "    _text(sl, pptx_mod, 1.14, 0.42, 10.6, 0.95, spec.get(\"title\", \"\"), 30, th[\"text\"], True)\n" \
    "\n" \
    "    bullets = (spec.get(\"bullets\") or [])[:5]\n" \
    "    y = 1.72\n" \
    "    step = 0.98 if len(bullets) <= 3 else 0.86\n" \
    "    for i, b in enumerate(bullets):\n" \
    "        _rect(sl, pptx_mod, 0.85, y, 11.6, step - 0.12, th[\"soft\"])   # 卡片底\n" \
    "        _badge(sl, pptx_mod, 1.02, y + (step - 0.12) / 2 - 0.19, 0.38,\n" \
    "               th[\"accents\"][i % 4], str(i + 1))\n" \
    "        _text(sl, pptx_mod, 1.62, y + 0.14, 10.5, step - 0.34, str(b), 17, th[\"text\"])\n" \
    "        y += step\n" \
    "    if total:\n" \
    "        _text(sl, pptx_mod, 11.2, 6.98, 1.6, 0.35, \"%d/%d\" % (idx, total), 11,\n" \
    "              th[\"muted\"], align=\"right\")\n" \
    "\n" \
    "\n" \
    "def lay_two_col(prs, pptx_mod, th, spec, idx, total):\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"bg\"])\n" \
    "    _circle(sl, pptx_mod, 11.6, 5.4, 3.0, th[\"accents\"][2], 12)\n" \
    "    _rect(sl, pptx_mod, 0, 0, 13.333, 0.09, th[\"accents\"][0])\n" \
    "    _rect(sl, pptx_mod, 0.85, 0.50, 0.10, 0.78, th[\"accents\"][1])\n" \
    "    _text(sl, pptx_mod, 1.14, 0.42, 10.6, 0.95, spec.get(\"title\", \"\"), 30, th[\"text\"], True)\n" \
    "\n" \
    "    cols = (spec.get(\"columns\") or [])[:2]\n" \
    "    for i, col in enumerate(cols):\n" \
    "        x = 0.85 + i * 6.0\n" \
    "        _rect(sl, pptx_mod, x, 1.62, 5.6, 4.85, th[\"soft\"])\n" \
    "        _rect(sl, pptx_mod, x, 1.62, 5.6, 0.09, th[\"accents\"][i % 4])     # 卡片顶色条\n" \
    "        _text(sl, pptx_mod, x + 0.32, 1.88, 4.9, 0.72,\n" \
    "              str(col.get(\"title\") or \"\"), 20, th[\"accents\"][i % 4], True)\n" \
    "        yy = 2.72\n" \
    "        for b in (col.get(\"bullets\") or [])[:5]:\n" \
    "            _circle(sl, pptx_mod, x + 0.36, yy + 0.20, 0.09, th[\"accents\"][i % 4])\n" \
    "            _text(sl, pptx_mod, x + 0.60, yy, 4.6, 0.62, str(b), 14, th[\"text\"])\n" \
    "            yy += 0.70\n" \
    "\n" \
    "\n" \
    "def lay_quote(prs, pptx_mod, th, spec, idx, total):\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"soft\"])\n" \
    "    _circle(sl, pptx_mod, -1.0, -1.2, 4.4, th[\"accents\"][3], 30)\n" \
    "    _circle(sl, pptx_mod, 10.8, 4.6, 3.6, th[\"accents\"][2], 20)\n" \
    "    _text(sl, pptx_mod, 1.05, 1.40, 2.0, 1.15, \"\\u201c\", 96, th[\"accents\"][2], True)\n" \
    "    _text(sl, pptx_mod, 1.05, 2.88, 10.8, 2.2, spec.get(\"quote\") or spec.get(\"title\", \"\"),\n" \
    "          26, th[\"text\"])\n" \
    "    _rect(sl, pptx_mod, 1.08, 5.30, 1.3, 0.07, th[\"accents\"][1])\n" \
    "    if spec.get(\"author\"):\n" \
    "        _text(sl, pptx_mod, 1.08, 5.52, 10.0, 0.6, \"— \" + str(spec[\"author\"]), 15, th[\"muted\"])\n" \
    "\n" \
    "\n" \
    "def lay_table(prs, pptx_mod, th, spec, idx, total):\n" \
    "    from pptx.util import Inches, Pt\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"bg\"])\n" \
    "    _rect(sl, pptx_mod, 0, 0, 13.333, 0.09, th[\"accents\"][0])\n" \
    "    _rect(sl, pptx_mod, 0.85, 0.50, 0.10, 0.78, th[\"accents\"][1])\n" \
    "    _text(sl, pptx_mod, 1.14, 0.42, 10.6, 0.95, spec.get(\"title\", \"\"), 30, th[\"text\"], True)\n" \
    "    rows = spec.get(\"rows\") or []\n" \
    "    if not rows:\n" \
    "        return\n" \
    "    nr = min(len(rows), 8)\n" \
    "    nc = len(rows[0])\n" \
    "    shape = sl.shapes.add_table(nr, nc, Inches(0.85), Inches(1.68),\n" \
    "                                Inches(11.6), Inches(min(0.46 * nr, 4.9)))\n" \
    "    tbl = shape.table\n" \
    "    for i in range(nr):\n" \
    "        for j in range(nc):\n" \
    "            cell = tbl.cell(i, j)\n" \
    "            cell.text = \"\" if j >= len(rows[i]) else str(rows[i][j])\n" \
    "            for p in cell.text_frame.paragraphs:\n" \
    "                p.font.size = Pt(13)\n" \
    "                p.font.name = FONT\n" \
    "                p.font.bold = (i == 0)\n" \
    "                p.font.color.rgb = _rgb(pptx_mod, \"FFFFFF\" if i == 0 else th[\"text\"])\n" \
    "            cell.fill.solid()\n" \
    "            cell.fill.fore_color.rgb = _rgb(\n" \
    "                pptx_mod, th[\"accents\"][0] if i == 0 else (th[\"soft\"] if i % 2 else th[\"bg\"]))\n" \
    "\n" \
    "\n" \
    "def lay_data(prs, pptx_mod, th, spec, idx, total):\n" \
    "    \"\"\"数据页：几个大数字卡片并排 —— 纯要点文字讲不清\"指标\"。\"\"\"\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"bg\"])\n" \
    "    _circle(sl, pptx_mod, 11.4, -1.4, 3.8, th[\"accents\"][3], 14)\n" \
    "    _rect(sl, pptx_mod, 0, 0, 13.333, 0.09, th[\"accents\"][0])\n" \
    "    _rect(sl, pptx_mod, 0.85, 0.50, 0.10, 0.78, th[\"accents\"][1])\n" \
    "    _text(sl, pptx_mod, 1.14, 0.42, 10.6, 0.95, spec.get(\"title\", \"\"), 30, th[\"text\"], True)\n" \
    "\n" \
    "    stats = (spec.get(\"stats\") or [])[:4]\n" \
    "    n = len(stats)\n" \
    "    if n == 0:\n" \
    "        return\n" \
    "    w = (11.6 - 0.28 * (n - 1)) / n\n" \
    "    for i, st in enumerate(stats):\n" \
    "        x = 0.85 + i * (w + 0.28)\n" \
    "        _rect(sl, pptx_mod, x, 2.05, w, 2.85, th[\"soft\"])\n" \
    "        _rect(sl, pptx_mod, x, 2.05, w, 0.08, th[\"accents\"][i % 4])\n" \
    "        _text(sl, pptx_mod, x + 0.15, 2.42, w - 0.3, 1.25,\n" \
    "              str(st.get(\"value\") or \"\"), 38, th[\"accents\"][i % 4], True, \"center\")\n" \
    "        _text(sl, pptx_mod, x + 0.15, 3.75, w - 0.3, 0.75,\n" \
    "              str(st.get(\"label\") or \"\"), 14, th[\"text\"], False, \"center\")\n" \
    "\n" \
    "\n" \
    "def lay_closing(prs, pptx_mod, th, spec, idx, total):\n" \
    "    sl = _blank(prs)\n" \
    "    _bg(sl, pptx_mod, th[\"accents\"][0])\n" \
    "    _circle(sl, pptx_mod, -1.2, 3.6, 4.8, th[\"accents\"][2], 20)\n" \
    "    _circle(sl, pptx_mod, 10.4, -1.0, 4.0, th[\"accents\"][3], 14)\n" \
    "    _text(sl, pptx_mod, 0.9, 2.55, 11.5, 1.6, spec.get(\"title\") or \"谢谢\",\n" \
    "          44, \"FFFFFF\", True, \"center\")\n" \
    "    _rect(sl, pptx_mod, 6.0, 4.28, 1.4, 0.08, th[\"accents\"][3])\n" \
    "    if spec.get(\"subtitle\"):\n" \
    "        _text(sl, pptx_mod, 0.9, 4.55, 11.5, 0.9, spec[\"subtitle\"], 18,\n" \
    "              th[\"accents\"][3], align=\"center\")\n" \
    "\n" \
    "\n" \
    "LAYOUTS = {\n" \
    "    \"cover\": lay_cover, \"section\": lay_section, \"bullets\": lay_bullets,\n" \
    "    \"two_col\": lay_two_col, \"quote\": lay_quote, \"table\": lay_table,\n" \
    "    \"data\": lay_data, \"closing\": lay_closing,\n" \
    "}\n" \
    "\n" \
    "\n" \
    "def _guess_layout(s, i, total):\n" \
    "    if s.get(\"layout\") and s[\"layout\"] in LAYOUTS:\n" \
    "        return s[\"layout\"]\n" \
    "    if i == 0:\n" \
    "        return \"cover\"\n" \
    "    if s.get(\"rows\"):\n" \
    "        return \"table\"\n" \
    "    if s.get(\"stats\"):\n" \
    "        return \"data\"\n" \
    "    if s.get(\"quote\"):\n" \
    "        return \"quote\"\n" \
    "    if s.get(\"columns\"):\n" \
    "        return \"two_col\"\n" \
    "    if i == total - 1 and not s.get(\"bullets\"):\n" \
    "        return \"closing\"\n" \
    "    if s.get(\"bullets\") and len(s[\"bullets\"]) == 1 and not s[\"bullets\"][0]:\n" \
    "        return \"section\"\n" \
    "    return \"bullets\"\n" \
    "\n" \
    "\n" \
    "def do_pptx(req):\n" \
    "    pptx_mod = _require(\"pptx\", \"python-pptx\")\n" \
    "    path = req[\"path\"]\n" \
    "    check_ext(path, [\".pptx\"])\n" \
    "    ensure_dir(path)\n" \
    "\n" \
    "    slides = req.get(\"slides\")\n" \
    "    if not slides and req.get(\"markdown\"):\n" \
    "        slides = md_to_slides(req[\"markdown\"])\n" \
    "    if not slides:\n" \
    "        fail(\"slides 为空，至少要有一页\")\n" \
    "\n" \
    "    th = _pick_theme(req, req.get(\"theme\"))\n" \
    "    prs = pptx_mod.Presentation()\n" \
    "    prs.slide_width = pptx_mod.util.Inches(13.333)      # 16:9\n" \
    "    prs.slide_height = pptx_mod.util.Inches(7.5)\n" \
    "\n" \
    "    used = []\n" \
    "    for i, s in enumerate(slides):\n" \
    "        lay = _guess_layout(s, i, len(slides))\n" \
    "        used.append(lay)\n" \
    "        LAYOUTS[lay](prs, pptx_mod, th, s, i + 1, len(slides))\n" \
    "    prs.save(path)\n" \
    "\n" \
    "    p2 = pptx_mod.Presentation(path)\n" \
    "    from collections import Counter\n" \
    "    kinds = dict(Counter(used))\n" \
    "    out(path=path, format=\"pptx\", slides=len(p2.slides), theme=th[\"name\"],\n" \
    "        layouts=kinds, verified=True, size=os.path.getsize(path))\n" \
    "\n" \
    "\n" \
    "# ---------------- pdf（PIL 画图）----------------\n" \
    "CJK_FONTS = [\n" \
    "    \"/opt/chrome-root/usr/share/fonts/truetype/wqy/wqy-microhei.ttc\",\n" \
    "    \"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc\",\n" \
    "    \"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc\",\n" \
    "    \"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc\",\n" \
    "    \"/opt/chrome-root/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf\",\n" \
    "]\n" \
    "\n" \
    "\n" \
    "def _find_font():\n" \
    "    for p in CJK_FONTS:\n" \
    "        if os.path.exists(p):\n" \
    "            return p\n" \
    "    return None\n" \
    "\n" \
    "\n" \
    "def do_pdf(req):\n" \
    "    PIL_Image = _require(\"PIL.Image\", \"Pillow\")\n" \
    "    PIL_Draw = _require(\"PIL.ImageDraw\", \"Pillow\")\n" \
    "    from PIL import Image, ImageDraw, ImageFont  # noqa\n" \
    "\n" \
    "    path = req[\"path\"]\n" \
    "    check_ext(path, [\".pdf\"])\n" \
    "    ensure_dir(path)\n" \
    "\n" \
    "    blocks = req.get(\"blocks\")\n" \
    "    if not blocks and req.get(\"markdown\"):\n" \
    "        blocks = md_to_blocks(req[\"markdown\"])\n" \
    "    if not blocks and req.get(\"content\"):\n" \
    "        blocks = [{\"type\": \"p\", \"text\": req[\"content\"]}]\n" \
    "    if not blocks:\n" \
    "        fail(\"没有内容可写\")\n" \
    "\n" \
    "    font_path = _find_font()\n" \
    "    if not font_path:\n" \
    "        fail(\"找不到可用的字体，无法生成 PDF\",\n" \
    "             \"需要 /usr/share/fonts 下有 wqy-microhei 或 Noto CJK\")\n" \
    "\n" \
    "    W, H = 1240, 1754          # A4 @150dpi\n" \
    "    margin = 90\n" \
    "    line_h = 42\n" \
    "    fs = int(req.get(\"font_size\") or 26)\n" \
    "    try:\n" \
    "        font = ImageFont.truetype(font_path, fs)\n" \
    "        font_b = ImageFont.truetype(font_path, fs + 8)\n" \
    "    except Exception as e:\n" \
    "        fail(\"加载字体失败: %s\" % e)\n" \
    "\n" \
    "    # 先按字符宽度折行（中文没有空格，不能按词折）\n" \
    "    def wrap(text, fnt, max_w, draw):\n" \
    "        lines, cur = [], \"\"\n" \
    "        for ch in text:\n" \
    "            if ch == \"\\n\":\n" \
    "                lines.append(cur)\n" \
    "                cur = \"\"\n" \
    "                continue\n" \
    "            trial = cur + ch\n" \
    "            if draw.textlength(trial, font=fnt) > max_w and cur:\n" \
    "                lines.append(cur)\n" \
    "                cur = ch\n" \
    "            else:\n" \
    "                cur = trial\n" \
    "        if cur:\n" \
    "            lines.append(cur)\n" \
    "        return lines\n" \
    "\n" \
    "    pages = []\n" \
    "    img = None\n" \
    "    draw = None\n" \
    "    y = 0\n" \
    "\n" \
    "    def new_page():\n" \
    "        nonlocal img, draw, y\n" \
    "        img = Image.new(\"RGB\", (W, H), \"white\")\n" \
    "        draw = ImageDraw.Draw(img)\n" \
    "        y = margin\n" \
    "\n" \
    "    def flush():\n" \
    "        nonlocal img\n" \
    "        if img is not None:\n" \
    "            pages.append(img)\n" \
    "\n" \
    "    new_page()\n" \
    "    text_dump = []          # 伴生 txt：保证写了能读回来\n" \
    "\n" \
    "    for b in blocks:\n" \
    "        t = b.get(\"type\") or \"p\"\n" \
    "        txt = \"\" if b.get(\"text\") is None else str(b[\"text\"])\n" \
    "        text_dump.append((\"# \" if t == \"h\" else \"\") + txt)\n" \
    "        fnt = font_b if t == \"h\" else font\n" \
    "        max_w = W - margin * 2\n" \
    "        for ln in wrap(txt, fnt, max_w, draw):\n" \
    "            if y + line_h > H - margin:\n" \
    "                flush()\n" \
    "                new_page()\n" \
    "            draw.text((margin, y), ln, fill=\"black\", font=fnt)\n" \
    "            y += line_h\n" \
    "        y += 10\n" \
    "\n" \
    "    flush()\n" \
    "    if not pages:\n" \
    "        fail(\"生成失败：没有页面\")\n" \
    "\n" \
    "    pages[0].save(path, \"PDF\", resolution=150.0, save_all=True,\n" \
    "                  append_images=pages[1:])\n" \
    "\n" \
    "    # 伴生 .txt：图片 PDF 提取不到文字，读取时用这个\n" \
    "    try:\n" \
    "        with open(path + \".txt\", \"w\", encoding=\"utf-8\") as f:\n" \
    "            f.write(\"\\n\".join(text_dump))\n" \
    "    except Exception:\n" \
    "        pass\n" \
    "\n" \
    "    out(path=path, format=\"pdf\", pages=len(pages), verified=os.path.exists(path),\n" \
    "        size=os.path.getsize(path), font=os.path.basename(font_path),\n" \
    "        note=\"板上没有 reportlab，PDF 是按页面图像渲染的：能看能打印，但文字不可选；\"\n" \
    "             \"已同时写同名 .txt，读取时优先用它\")\n" \
    "\n" \
    "\n" \
    "# ---------------- 入口 ----------------\n" \
    "def main():\n" \
    "    if len(sys.argv) < 2:\n" \
    "        fail(\"用法: docwrite.py <op>  (数据从 stdin 读 JSON)\")\n" \
    "    op = sys.argv[1]\n" \
    "    raw = sys.stdin.read()\n" \
    "    try:\n" \
    "        req = json.loads(raw) if raw.strip() else {}\n" \
    "    except Exception as e:\n" \
    "        fail(\"参数不是合法 JSON: %s\" % e)\n" \
    "    if not req.get(\"path\"):\n" \
    "        fail(\"缺少 path\")\n" \
    "\n" \
    "    handlers = {\n" \
    "        \"text\": do_text,\n" \
    "        \"xlsx\": do_xlsx,\n" \
    "        \"xlsx_edit\": do_xlsx_edit,\n" \
    "        \"xlsx_read\": do_xlsx_read,\n" \
    "        \"docx\": do_docx,\n" \
    "        \"docx_append\": do_docx_append,\n" \
    "        \"pptx\": do_pptx,\n" \
    "        \"pdf\": do_pdf,\n" \
    "    }\n" \
    "    h = handlers.get(op)\n" \
    "    if not h:\n" \
    "        fail(\"不支持的 op: %s（可用 %s）\" % (op, \",\".join(handlers)))\n" \
    "    try:\n" \
    "        h(req)\n" \
    "    except SystemExit:\n" \
    "        raise\n" \
    "    except Exception as e:\n" \
    "        fail(\"执行出错: %s\" % e)\n" \
    "\n" \
    "\n" \
    "if __name__ == \"__main__\":\n" \
    "    main()\n" \
    "\n"
#endif
