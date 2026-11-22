import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
try:
    drv.get("https://www.goofish.com/")
    time.sleep(3)
    t = time.time(); info = drv.execute_script(browse.JS_PROBE); t1 = time.time() - t
    print("主文档 probe: %.2fs  inputs=%d" % (t1, len(info.get("inputs") or [])))

    n = drv.execute_script("return document.querySelectorAll('iframe').length")
    print("iframe 数量:", n)
    srcs = drv.execute_script(
        "return Array.from(document.querySelectorAll('iframe')).map(function(f){"
        "return (f.getAttribute('src')||'').slice(0,60);});")
    for i, s in enumerate(srcs or []):
        print("  iframe[%d] %s" % (i, s))

    t = time.time(); deep = browse._probe_deep(drv); t2 = time.time() - t
    print("_probe_deep: %.2fs  inputs=%d frames=%s" % (
        t2, len(deep.get("inputs") or []), deep.get("frames_with_forms")))
finally:
    drv.quit()
