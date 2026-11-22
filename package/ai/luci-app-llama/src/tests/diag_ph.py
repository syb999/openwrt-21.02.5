#!/usr/bin/env python3
# 诊断 ⓘ 图标为什么看不见
import sys
import time

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

    info = drv.execute_script(r"""
    var row = document.querySelector('.plugrow');
    var ph  = document.querySelector('.plugrow .ph');
    var sp  = row ? row.querySelector('span') : null;
    if (!ph) return {err: 'no .ph'};
    var rp = ph.getBoundingClientRect();
    var rr = row.getBoundingClientRect();
    var rs = sp ? sp.getBoundingClientRect() : null;
    var c  = getComputedStyle(ph);
    return {
      row: [Math.round(rr.left), Math.round(rr.width)],
      span: rs ? [Math.round(rs.left), Math.round(rs.width)] : null,
      ph: [Math.round(rp.left), Math.round(rp.width), Math.round(rp.height)],
      phStyle: {display: c.display, visibility: c.visibility, opacity: c.opacity,
                fontSize: c.fontSize, color: c.color, border: c.borderTopWidth + ' ' + c.borderTopColor,
                flex: c.flex},
      phText: (ph.innerText || ''),
      phInView: rp.left + rp.width <= window.innerWidth,
      rowOverflow: row.scrollWidth > row.clientWidth,
      rowScrollW: row.scrollWidth, rowClientW: row.clientWidth
    };
    """)
    print("诊断:")
    for k, v in info.items():
        print("  %-12s %s" % (k, v))
finally:
    drv.quit()
