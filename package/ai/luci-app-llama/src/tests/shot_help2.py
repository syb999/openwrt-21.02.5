import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
drv.set_window_size(1366, 900)
try:
    drv.get("http://127.0.0.1/llama-check.htm")
    time.sleep(2.5)
    drv.execute_script(
        "var els=document.querySelectorAll('button,a,span,i,div');"
        "for(var i=0;i<els.length;i++){var t='';try{t=(els[i].innerText||'').trim();}catch(e){}"
        "var ti=els[i].getAttribute('title')||'';"
        "if(t==='✎'||/设置/.test(ti)){els[i].click();return t;}}")
    time.sleep(2.5)
    drv.execute_script("var p=document.querySelector('.ph[data-help=browser]'); if(p) p.click();")
    time.sleep(1.2)
    info = drv.execute_script(
        "var b=document.querySelector('.hlp-box'); if(!b) return {found:false};"
        "var r=b.getBoundingClientRect();"
        "var w=getComputedStyle(b.querySelector('.hlp-what'));"
        "var c=b.querySelector('.hlp-cmd');"
        "var cs=c?getComputedStyle(c):null;"
        "return {found:true, h:Math.round(r.height), w:Math.round(r.width),"
        "  whatSize:w.fontSize, ops:b.querySelectorAll('.hlp-op').length,"
        "  cmds:b.querySelectorAll('.hlp-cmd').length,"
        "  cmdSize:cs?cs.fontSize:null, scroll:b.scrollHeight>b.clientHeight};")
    print("弹窗:", info)
    drv.save_screenshot("/tmp/llama_agent/shots/hlp_new.png")
    drv.execute_script("var b=document.querySelector('.hlp-box'); if(b) b.scrollTop = 300;")
    time.sleep(0.4)
    drv.save_screenshot("/tmp/llama_agent/shots/hlp_new2.png")
finally:
    drv.quit()
