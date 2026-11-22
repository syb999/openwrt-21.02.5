import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
try:
    drv.get("https://www.goofish.com/")
    time.sleep(3)
    print("初始 input 总数(DOM):", drv.execute_script("return document.querySelectorAll('input').length"))

    hit = drv.execute_script(browse.FIND_CLICK_JS, "登录")
    print("点『登录』:", hit)
    time.sleep(2.5)

    print("弹窗后 input 总数(DOM):", drv.execute_script("return document.querySelectorAll('input').length"))
    info = drv.execute_script(browse.JS_PROBE)
    print("JS_PROBE 探到:", len(info.get("inputs") or []))

    # 逐个看 DOM 里的 input 及可见性判断
    det = drv.execute_script(r"""
    var out = [];
    document.querySelectorAll('input,textarea').forEach(function(el){
      var r = el.getBoundingClientRect(), s;
      try { s = getComputedStyle(el); } catch(e) { s = {}; }
      out.push({type: el.type || '', ph: (el.placeholder||'').slice(0,14),
                w: Math.round(r.width), h: Math.round(r.height),
                disp: s.display, vis: s.visibility, op: s.opacity,
                inShadow: !!el.getRootNode().host});
    });
    return out.slice(0, 8);
    """)
    for d in (det or []):
        print("  input:", d)
finally:
    drv.quit()
