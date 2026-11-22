import sys, time
sys.argv = ["browse.py", "noop"]
sys.path.insert(0, "/tmp/bz")
import browse
from selenium.webdriver.common.by import By

drv = browse.new_driver()
try:
    drv.get("https://www.goofish.com/")
    time.sleep(3)
    # 点登录入口
    drv.execute_script(browse.FIND_CLICK_JS, "登录")
    time.sleep(3.5)

    info = browse._probe_deep(drv)
    print("探到输入框:", len(info.get("inputs") or []))

    # ---- 用 probe 给出的 xpath 直接操作（这就是"selenium 能点的命令"）----
    ok = []
    for b in info.get("buttons") or []:
        if b.get("text") == "密码登录" and b.get("xpath"):
            print("找到『密码登录』xpath:", b["xpath"], " frame:", b.get("frame"))
            frames = drv.find_elements(By.TAG_NAME, "iframe")
            if b.get("frame") is not None:
                drv.switch_to.frame(frames[b["frame"]])
            el = drv.find_element(By.XPATH, b["xpath"])
            print("  selenium 按 xpath 定位成功:", el.tag_name, repr(el.text[:12]))
            el.click()          # JS 按钮也能点
            ok.append("clicked")
            drv.switch_to.default_content()
            break

    time.sleep(1.5)
    after = browse._probe_deep(drv)
    pwds = [i for i in (after.get("inputs") or []) if (i.get("type") or "") == "password"]
    print("点完之后密码框数量:", len(pwds))
    for p in pwds:
        print("  密码框 xpath:", p.get("xpath"), " frame:", p.get("frame"))
    print("结果:", "成功 —— xpath 可用，JS 按钮点得动" if pwds else "没切出密码框")
finally:
    drv.quit()
