import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
drv.set_window_size(1400, 950)
try:
    drv.get("http://127.0.0.1/llama.htm?v=final")
    time.sleep(3)

    # 先发一条消息，产生 AI 头像
    drv.execute_script(
        "var t=document.getElementById('input');"
        "var s=Object.getOwnPropertyDescriptor(window.HTMLTextAreaElement.prototype,'value').set;"
        "s.call(t,'{\"op\":\"snapshot\"}');"
        "t.dispatchEvent(new Event('input',{bubbles:true}));")
    time.sleep(0.3)
    drv.execute_script(
        "var b=document.querySelector('.sendbtn,#send,.send'); if(b) b.click();")
    for _ in range(20):
        time.sleep(1)
        if drv.execute_script("return document.querySelectorAll('.who-ai').length > 0"):
            break

    n = drv.execute_script("return document.querySelectorAll('.who-ai').length")
    print("AI 头像数量:", n, " (可点)")

    # 点 AI 头像 → 应该打开设置
    r = drv.execute_script(
        "var a=document.querySelector('.who-ai');"
        "if(!a) return {err:'no avatar'};"
        "var title=a.title; a.click(); return {title:title};")
    print("点了 AI 头像, title:", r.get("title"))
    time.sleep(1.5)
    st = drv.execute_script(
        "var m=document.querySelector('.qask-mask');"
        "return {opened: !!m, hasPlug: !!(m && m.querySelector('#pluglist')),"
        "        hint: (m && m.querySelector('.pk-hint')||{}).innerText || ''};")
    print("设置弹窗:", st)
    drv.save_screenshot("/tmp/llama_agent/shots/ai_click.png")
finally:
    drv.quit()
