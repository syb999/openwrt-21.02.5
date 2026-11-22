import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

WIDTHS = [(1440, 900), (1280, 800), (1024, 768), (900, 700), (768, 700), (500, 800)]

drv = browse.new_driver()
try:
    for w, h in WIDTHS:
        drv.set_window_size(w, h)
        drv.get("http://127.0.0.1/llama-check.htm")
        time.sleep(2)
        drv.execute_script(
            "var els=document.querySelectorAll('button,a,span,i,div');"
            "for(var i=0;i<els.length;i++){var t='';try{t=(els[i].innerText||'').trim();}catch(e){}"
            "var ti=els[i].getAttribute('title')||'';"
            "if(t==='✎'||/设置/.test(ti)){els[i].click();return t;}}")
        time.sleep(2)
        info = drv.execute_script(r"""
        var pl  = document.querySelector('#pluglist');
        var ph  = document.querySelector('.plugrow .ph');
        var row = document.querySelector('.plugrow');
        if (!pl || !ph) return {err: 'no el'};
        var rp = ph.getBoundingClientRect();
        var rl = pl.getBoundingClientRect();
        var rr = row.getBoundingClientRect();
        return {
          vw: window.innerWidth,
          phW: Math.round(rp.width), phH: Math.round(rp.height),
          phLeft: Math.round(rp.left), phRight: Math.round(rp.right),
          listLeft: Math.round(rl.left), listRight: Math.round(rl.right),
          rowW: Math.round(rr.width), rowScrollW: row.scrollWidth,
          insideList: rp.left >= rl.left - 1 && rp.right <= rl.right + 1,
          visible: rp.width > 3 && rp.height > 3 && rp.left >= 0 && rp.right <= window.innerWidth,
          text: (ph.innerText || '')
        };""")
        if info.get("err"):
            print("%4dx%-4d  ERR %s" % (w, h, info["err"]))
            continue
        print("%4dx%-4d  ⓘ %dx%d @%d..%d  列表 %d..%d  在列表内=%s  可见=%s  文字=%r" % (
            w, h, info["phW"], info["phH"], info["phLeft"], info["phRight"],
            info["listLeft"], info["listRight"],
            info["insideList"], info["visible"], info["text"]))
        if w == 900:
            drv.save_screenshot("/tmp/llama_agent/shots/w900.png")
finally:
    drv.quit()
