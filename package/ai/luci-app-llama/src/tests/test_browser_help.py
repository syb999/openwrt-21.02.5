import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
try:
    drv.get("http://127.0.0.1/llama.htm")
    time.sleep(3)
    drv.execute_script(
        "var els=document.querySelectorAll('button,a,span,i,div');"
        "for(var i=0;i<els.length;i++){var t='';try{t=(els[i].innerText||'').trim();}catch(e){}"
        "var ti=els[i].getAttribute('title')||'';"
        "if(t==='✎'||/设置/.test(ti)){els[i].click();return t;}}")
    time.sleep(2.5)
    # 点 browser 的 ⓘ（内容最多的那个）
    nm = drv.execute_script(
        "var p = document.querySelector('.ph[data-help=browser]');"
        "if (!p) return ''; p.click(); return 'browser';")
    print("点了:", nm)
    time.sleep(1.2)
    info = drv.execute_script(
        "var b = document.querySelector('.hlp-box');"
        "if (!b) return {found:false};"
        "var r = b.getBoundingClientRect();"
        "return {found:true, h: Math.round(r.height), w: Math.round(r.width),"
        "        ops: b.querySelectorAll('.hlp-op').length,"
        "        hasSample: !!b.querySelector('.hlp-code'),"
        "        hasNote: !!b.querySelector('.hlp-note'),"
        "        scrollable: b.scrollHeight > b.clientHeight,"
        "        maxH: Math.round(window.innerHeight * 0.8)};")
    print("弹窗:", info)
    drv.save_screenshot("/tmp/llama_agent/shots/plug_3_browser.png")
finally:
    drv.quit()
