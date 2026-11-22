import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse
from selenium.webdriver.common.by import By
from selenium.webdriver.common.action_chains import ActionChains

drv = browse.new_driver()
drv.set_window_size(1400, 950)
try:
    drv.get("http://127.0.0.1/llama.htm?t=entries")
    time.sleep(3)

    def close_modal():
        drv.execute_script("var b=document.querySelector('.qask-box .ok, .qask-box .cancel'); if(b) b.click();")
        time.sleep(0.5)

    def has_modal():
        return drv.execute_script("return !!document.querySelector('.qask-mask')")

    # 先发一条消息，确保 .who-ai 存在
    drv.execute_script(
        "var t=document.getElementById('input');"
        "var s=Object.getOwnPropertyDescriptor(window.HTMLTextAreaElement.prototype,'value').set;"
        "s.call(t,'{\"op\":\"snapshot\"}');"
        "t.dispatchEvent(new Event('input',{bubbles:true}));")
    time.sleep(0.3)
    drv.execute_script("var b=document.getElementById('send'); if(b) b.click();")
    for _ in range(15):
        time.sleep(1)
        if drv.execute_script("return document.querySelectorAll('.who-ai').length>0"):
            break

    tests = [
        (".dot",      "左上角绿点"),
        (".brand",    "「本地 AI 助手」整块"),
        ("#setbtn",   "输入框旁齿轮"),
        (".who-ai",   "聊天里的 AI 头像"),
    ]
    for sel, label in tests:
        els = drv.find_elements(By.CSS_SELECTOR, sel)
        if not els:
            print("%-22s ✗ 元素不存在" % label)
            continue
        # 真实鼠标点击
        try:
            ActionChains(drv).move_to_element(els[0]).click().perform()
        except Exception as e:
            print("%-22s 鼠标点击异常: %s" % (label, str(e).splitlines()[0][:50]))
            continue
        time.sleep(1.2)
        ok = has_modal()
        print("%-22s %s" % (label, "✓ 打开设置" if ok else "✗ 没反应"))
        if ok:
            close_modal()
finally:
    drv.quit()
