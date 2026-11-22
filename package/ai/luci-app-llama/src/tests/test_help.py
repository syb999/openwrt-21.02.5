#!/usr/bin/env python3
# 实测：打开页面 → 点设置 → 点插件旁的 ⓘ → 看说明弹窗
import sys
import time

sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

SHOT = "/tmp/llama_agent/shots"
URL = "http://127.0.0.1/llama.htm"

drv = browse.new_driver()
try:
    drv.get(URL)
    time.sleep(3)
    print("页面标题:", drv.title[:40])

    # 1. 找设置按钮（✎）
    r = drv.execute_script(
        "var out = [], els = document.querySelectorAll('button,a,span,i,div');"
        "for (var i = 0; i < els.length; i++) {"
        "  var t = '';"
        "  try { t = (els[i].innerText || '').trim(); } catch (e) {}"
        "  var ti = els[i].getAttribute('title') || '';"
        "  if (t === '✎' || /设置/.test(ti) || /设置/.test(t)) {"
        "    out.push({tag: els[i].tagName, cls: (els[i].className||''),"
        "              txt: t, title: ti});"
        "  }"
        "}"
        "return out.slice(0, 5);")
    print("设置按钮候选:", r)

    ok = drv.execute_script(
        "var els = document.querySelectorAll('button,a,span,i,div');"
        "for (var i = 0; i < els.length; i++) {"
        "  var t = '';"
        "  try { t = (els[i].innerText || '').trim(); } catch (e) {}"
        "  var ti = els[i].getAttribute('title') || '';"
        "  if (t === '✎' || /设置/.test(ti)) { els[i].click(); return t || ti; }"
        "}"
        "return '';")
    print("点了设置:", repr(ok))
    time.sleep(2.5)      # 等插件列表从 env 拉回来

    # 2. 看插件行和 ⓘ
    info = drv.execute_script(
        "var rows = document.querySelectorAll('.plugrow');"
        "var phs = document.querySelectorAll('.plugrow .ph');"
        "return {rows: rows.length, phs: phs.length,"
        "        first: rows.length ? (rows[0].innerText||'').trim().slice(0,40) : '',"
        "        helpBtn: phs.length ? (phs[0].getAttribute('data-help')||'') : ''};")
    print("插件行:", info)
    drv.save_screenshot(SHOT + "/plug_1_list.png")

    if not info.get("phs"):
        print("!! 没找到 ⓘ 图标")
    else:
        # 3. 点第一个 ⓘ
        clicked = drv.execute_script(
            "var p = document.querySelector('.plugrow .ph');"
            "if (!p) return '';"
            "var nm = p.getAttribute('data-help');"
            "p.click();"
            "return nm;")
        print("点了 ⓘ:", repr(clicked))
        time.sleep(1.2)

        # 4. 看弹窗
        modal = drv.execute_script(
            "var b = document.querySelector('.hlp-box');"
            "if (!b) return {found:false};"
            "return {found:true,"
            "        h4: (b.querySelector('h4')||{}).innerText || '',"
            "        what: ((b.querySelector('.hlp-what')||{}).innerText||'').slice(0,80),"
            "        ops: b.querySelectorAll('.hlp-op').length,"
            "        hasSample: !!b.querySelector('.hlp-code'),"
            "        hasNote: !!b.querySelector('.hlp-note'),"
            "        cbsChecked: (function(){var c=0;document.querySelectorAll('.plugrow input[type=checkbox]').forEach(function(x){if(x.checked)c++;});return c;})()};")
        print("弹窗:", modal)
        drv.save_screenshot(SHOT + "/plug_2_help.png")

        # 5. 关掉弹窗，确认 checkbox 没被误点
        closed = drv.execute_script(
            "var b = document.querySelector('.hlp-box .ok');"
            "if (b) { b.click(); return true; } return false;")
        time.sleep(0.5)
        after = drv.execute_script(
            "return {modal: !!document.querySelector('.hlp-box'),"
            "        cbsChecked: (function(){var c=0;document.querySelectorAll('.plugrow input[type=checkbox]').forEach(function(x){if(x.checked)c++;});return c;})()};")
        print("关闭后:", closed, after)
finally:
    drv.quit()
