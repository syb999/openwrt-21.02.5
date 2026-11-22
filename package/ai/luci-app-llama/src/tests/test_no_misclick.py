import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
try:
    drv.get("http://127.0.0.1/llama-check.htm")
    time.sleep(2.5)
    drv.execute_script(
        "var els=document.querySelectorAll('button,a,span,i,div');"
        "for(var i=0;i<els.length;i++){var t='';try{t=(els[i].innerText||'').trim();}catch(e){}"
        "var ti=els[i].getAttribute('title')||'';"
        "if(t==='✎'||/设置/.test(ti)){els[i].click();return t;}}")
    time.sleep(2.5)

    def cbs():
        return drv.execute_script(
            "var n=0,t=0;"
            "document.querySelectorAll('.plugrow input[type=checkbox]').forEach(function(c){"
            "  t++; if(c.checked) n++; });"
            "return {checked:n, total:t};")

    print("初始:", cbs())

    # --- 点「说明」三次，checkbox 必须一个都不变 ---
    for name in ("browser", "curl", "doc"):
        drv.execute_script(
            "var p=document.querySelector('.ph[data-help='+arguments[0]+']');"
            "if(p) p.click();", name)
        time.sleep(0.8)
        modal = drv.execute_script("return !!document.querySelector('.hlp-box');")
        st = cbs()
        print("点 %-8s 的说明 → 弹窗=%s  checkbox=%s" % (name, modal, st))
        drv.execute_script(
            "var b=document.querySelector('.hlp-box .ok'); if(b) b.click();")
        time.sleep(0.4)

    print()
    print("=== 再验证点插件名还能正常勾选 ===")
    before = cbs()
    drv.execute_script(
        "var l=document.querySelector('label.plname[for=cb_browser]'); if(l) l.click();")
    time.sleep(0.4)
    after = cbs()
    print("点插件名 browser: %s -> %s  %s" % (
        before, after,
        "切换正常" if before["checked"] != after["checked"] else "!! 没反应"))
    # 再点回来
    drv.execute_script("var l=document.querySelector('label.plname[for=cb_browser]'); if(l) l.click();")
    time.sleep(0.3)
    print("再点一次: ", cbs())
finally:
    drv.quit()
