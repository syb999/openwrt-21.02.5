import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse

drv = browse.new_driver()
drv.set_window_size(1400, 950)
try:
    drv.get("http://127.0.0.1/llama-check.htm")
    time.sleep(3)

    cmd = '{"op":"probe","url":"https://www.goofish.com/"}'
    # 填输入框并发送
    drv.execute_script(
        "var t=document.getElementById('input');"
        "var setter=Object.getOwnPropertyDescriptor(window.HTMLTextAreaElement.prototype,'value').set;"
        "setter.call(t, arguments[0]);"
        "t.dispatchEvent(new Event('input',{bubbles:true}));", cmd)
    time.sleep(0.3)
    drv.execute_script(
        "var b=document.querySelector('.sendbtn,#send,.send');"
        "if(b) b.click(); else {"
        "  t=document.getElementById('input');"
        "  t.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}));"
        "}")
    print("已发送命令:", cmd)

    # 等结果（直连约 10-15 秒）
    for i in range(30):
        time.sleep(1)
        r = drv.execute_script(
            "var bs=document.querySelectorAll('.agent-bubble');"
            "if(!bs.length) return {n:0};"
            "var b=bs[bs.length-1];"
            "return {n:bs.length,"
            "        imgs:b.querySelectorAll('img').length,"
            "        dls:b.querySelectorAll('.fdl,.dlrow').length,"
            "        hasRawJson:/^\\s*\\{/.test((b.innerText||'').trim()),"
            "        text:(b.innerText||'').slice(0,120)};")
        if i % 3 == 0 or r.get("imgs") or r.get("dls"):
            print("  t+%2ds 消息%d 图%d 下载%d 原始JSON=%s" % (
                i+1, r.get("n",0), r.get("imgs",0), r.get("dls",0), r.get("hasRawJson")))
        if r.get("imgs") or r.get("dls"):
            print("  >>> 出图/下载了")
            print("  文本:", repr(r.get("text"))[:160])
            break
    drv.save_screenshot("/tmp/llama_agent/shots/e2e_probe.png")
finally:
    drv.quit()
